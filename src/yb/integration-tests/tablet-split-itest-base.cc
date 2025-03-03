// Copyright (c) YugaByte, Inc.
//
// Licensed under the Apache License, Version 2.0 (the "License"); you may not use this file except
// in compliance with the License.  You may obtain a copy of the License at
//
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software distributed under the License
// is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express
// or implied.  See the License for the specific language governing permissions and limitations
// under the License.
//

#include "yb/integration-tests/tablet-split-itest-base.h"

#include <signal.h>

#include <boost/range/adaptor/transformed.hpp>

#include "yb/client/client-test-util.h"
#include "yb/client/session.h"
#include "yb/client/snapshot_test_util.h"
#include "yb/client/table_info.h"
#include "yb/client/transaction.h"
#include "yb/client/yb_op.h"

#include "yb/common/schema.h"
#include "yb/common/ql_expr.h"
#include "yb/common/ql_value.h"
#include "yb/common/wire_protocol.h"

#include "yb/consensus/consensus.h"
#include "yb/consensus/consensus_util.h"

#include "yb/docdb/doc_key.h"
#include "yb/docdb/ql_rowwise_iterator_interface.h"

#include "yb/integration-tests/mini_cluster.h"
#include "yb/integration-tests/test_workload.h"

#include "yb/master/catalog_entity_info.h"
#include "yb/master/master_admin.proxy.h"
#include "yb/master/master_client.pb.h"

#include "yb/rocksdb/db.h"

#include "yb/rpc/messenger.h"

#include "yb/tablet/tablet.h"
#include "yb/tablet/tablet_metadata.h"
#include "yb/tablet/tablet_peer.h"

#include "yb/tserver/mini_tablet_server.h"
#include "yb/tserver/tserver_service.pb.h"
#include "yb/tserver/tserver_service.proxy.h"

#include "yb/yql/cql/ql/util/statement_result.h"

DECLARE_int32(cleanup_split_tablets_interval_sec);
DECLARE_int64(db_block_size_bytes);
DECLARE_int64(db_filter_block_size_bytes);
DECLARE_int64(db_index_block_size_bytes);
DECLARE_int64(db_write_buffer_size);
DECLARE_bool(enable_automatic_tablet_splitting);
DECLARE_int32(raft_heartbeat_interval_ms);
DECLARE_int32(replication_factor);
DECLARE_int32(tserver_heartbeat_metrics_interval_ms);
DECLARE_bool(TEST_do_not_start_election_test_only);
DECLARE_bool(TEST_skip_deleting_split_tablets);
DECLARE_bool(TEST_validate_all_tablet_candidates);

namespace yb {

Result<size_t> SelectRowsCount(
    const client::YBSessionPtr& session, const client::TableHandle& table) {
  LOG(INFO) << "Running full scan on test table...";
  session->SetTimeout(5s * kTimeMultiplier);
  QLPagingStatePB paging_state;
  size_t row_count = 0;
  for (;;) {
    const auto op = table.NewReadOp();
    auto* const req = op->mutable_request();
    req->set_return_paging_state(true);
    if (paging_state.has_table_id()) {
      if (paging_state.has_read_time()) {
        ReadHybridTime read_time = ReadHybridTime::FromPB(paging_state.read_time());
        if (read_time) {
          session->SetReadPoint(read_time);
        }
      }
      session->SetForceConsistentRead(client::ForceConsistentRead::kTrue);
      *req->mutable_paging_state() = std::move(paging_state);
    }
    RETURN_NOT_OK(session->TEST_ApplyAndFlush(op));
    auto rowblock = ql::RowsResult(op.get()).GetRowBlock();
    row_count += rowblock->row_count();
    if (!op->response().has_paging_state()) {
      break;
    }
    paging_state = op->response().paging_state();
  }
  return row_count;
}

void DumpTableLocations(
    master::CatalogManagerIf* catalog_mgr, const client::YBTableName& table_name) {
  master::GetTableLocationsResponsePB resp;
  master::GetTableLocationsRequestPB req;
  table_name.SetIntoTableIdentifierPB(req.mutable_table());
  req.set_max_returned_locations(std::numeric_limits<int32_t>::max());
  ASSERT_OK(catalog_mgr->GetTableLocations(&req, &resp));
  LOG(INFO) << "Table locations:";
  for (auto& tablet : resp.tablet_locations()) {
    LOG(INFO) << "Tablet: " << tablet.tablet_id()
              << " partition: " << tablet.partition().ShortDebugString();
  }
}

void DumpWorkloadStats(const TestWorkload& workload) {
  LOG(INFO) << "Rows inserted: " << workload.rows_inserted();
  LOG(INFO) << "Rows insert failed: " << workload.rows_insert_failed();
  LOG(INFO) << "Rows read ok: " << workload.rows_read_ok();
  LOG(INFO) << "Rows read empty: " << workload.rows_read_empty();
  LOG(INFO) << "Rows read error: " << workload.rows_read_error();
  LOG(INFO) << "Rows read try again: " << workload.rows_read_try_again();
}

Status SplitTablet(master::CatalogManagerIf* catalog_mgr, const tablet::Tablet& tablet) {
  const auto& tablet_id = tablet.tablet_id();
  LOG(INFO) << "Tablet: " << tablet_id;
  LOG(INFO) << "Number of SST files: " << tablet.TEST_db()->GetCurrentVersionNumSSTFiles();
  std::string properties;
  tablet.TEST_db()->GetProperty(rocksdb::DB::Properties::kAggregatedTableProperties, &properties);
  LOG(INFO) << "DB properties: " << properties;

  return catalog_mgr->SplitTablet(tablet_id, master::ManualSplit::kTrue);
}

Status DoSplitTablet(master::CatalogManagerIf* catalog_mgr, const tablet::Tablet& tablet) {
  const auto& tablet_id = tablet.tablet_id();
  LOG(INFO) << "Tablet: " << tablet_id;
  LOG(INFO) << "Number of SST files: " << tablet.TEST_db()->GetCurrentVersionNumSSTFiles();
  std::string properties;
  tablet.TEST_db()->GetProperty(rocksdb::DB::Properties::kAggregatedTableProperties, &properties);
  LOG(INFO) << "DB properties: " << properties;

  const auto encoded_split_key = VERIFY_RESULT(tablet.GetEncodedMiddleSplitKey());
  std::string partition_split_key = encoded_split_key;
  if (tablet.metadata()->partition_schema()->IsHashPartitioning()) {
    const auto doc_key_hash = VERIFY_RESULT(docdb::DecodeDocKeyHash(encoded_split_key)).value();
    LOG(INFO) << "Middle hash key: " << doc_key_hash;
    partition_split_key = PartitionSchema::EncodeMultiColumnHashValue(doc_key_hash);
  }
  LOG(INFO) << "Partition split key: " << Slice(partition_split_key).ToDebugHexString();

  return catalog_mgr->TEST_SplitTablet(tablet_id, encoded_split_key, partition_split_key);
}

//
// TabletSplitITestBase
//

// Need to define the static constexpr members as well.
template<class MiniClusterType>
constexpr std::chrono::duration<int64> TabletSplitITestBase<MiniClusterType>::kRpcTimeout;

template<class MiniClusterType>
constexpr int TabletSplitITestBase<MiniClusterType>::kDefaultNumRows;

template<class MiniClusterType>
constexpr size_t TabletSplitITestBase<MiniClusterType>::kDbBlockSizeBytes;

template <class MiniClusterType>
void TabletSplitITestBase<MiniClusterType>::SetUp() {
  this->SetNumTablets(3);
  this->create_table_ = false;
  this->mini_cluster_opt_.num_tablet_servers = GetRF();
  client::TransactionTestBase<MiniClusterType>::SetUp();
  proxy_cache_ = std::make_unique<rpc::ProxyCache>(this->client_->messenger());
}

template <class MiniClusterType>
Result<tserver::ReadRequestPB> TabletSplitITestBase<MiniClusterType>::CreateReadRequest(
    const TabletId& tablet_id, int32_t key) {
  tserver::ReadRequestPB req;
  auto op = client::CreateReadOp(key, this->table_, this->kValueColumn);
  auto* ql_batch = req.add_ql_batch();
  *ql_batch = op->request();

  std::string partition_key;
  RETURN_NOT_OK(op->GetPartitionKey(&partition_key));
  const auto& hash_code = PartitionSchema::DecodeMultiColumnHashValue(partition_key);
  ql_batch->set_hash_code(hash_code);
  ql_batch->set_max_hash_code(hash_code);
  req.set_tablet_id(tablet_id);
  req.set_consistency_level(YBConsistencyLevel::CONSISTENT_PREFIX);
  return req;
}

template <class MiniClusterType>
tserver::WriteRequestPB TabletSplitITestBase<MiniClusterType>::CreateInsertRequest(
    const TabletId& tablet_id, int32_t key, int32_t value) {
  tserver::WriteRequestPB req;
  auto op = this->table_.NewWriteOp(QLWriteRequestPB::QL_STMT_INSERT);

  {
    auto op_req = op->mutable_request();
    QLAddInt32HashValue(op_req, key);
    this->table_.AddInt32ColumnValue(op_req, this->kValueColumn, value);
  }

  auto* ql_batch = req.add_ql_write_batch();
  *ql_batch = op->request();

  std::string partition_key;
  EXPECT_OK(op->GetPartitionKey(&partition_key));
  const auto& hash_code = PartitionSchema::DecodeMultiColumnHashValue(partition_key);
  ql_batch->set_hash_code(hash_code);
  req.set_tablet_id(tablet_id);
  return req;
}

template <class MiniClusterType>
Result<std::pair<docdb::DocKeyHash, docdb::DocKeyHash>>
    TabletSplitITestBase<MiniClusterType>::WriteRows(
        client::TableHandle* table, const uint32_t num_rows,
        const int32_t start_key, const int32_t start_value, client::YBSessionPtr session) {
  LOG(INFO) << "Writing " << num_rows << " rows...";

  auto txn = this->CreateTransaction();
  client::YBSessionPtr session_holder;
  if (session) {
    session->SetTransaction(txn);
  } else {
    session = this->CreateSession(txn);
  }

  vector<client::YBqlWriteOpPtr> ops;
  ops.reserve(num_rows);
  for (int32_t i = start_key, v = start_value;
       i < start_key + static_cast<int32_t>(num_rows);
       ++i, ++v) {
    ops.push_back(VERIFY_RESULT(
        client::kv_table_test::WriteRow(table,
                                        session,
                                        i /* key */,
                                        v /* value */,
                                        client::WriteOpType::INSERT,
                                        client::Flush::kFalse)));
    YB_LOG_EVERY_N_SECS(INFO, 10) << "Rows written: " << start_key << "..." << i;
  }
  RETURN_NOT_OK(session->TEST_Flush());

  auto min_hash_code = std::numeric_limits<docdb::DocKeyHash>::max();
  auto max_hash_code = std::numeric_limits<docdb::DocKeyHash>::min();
  for (const auto& op : ops) {
    const auto hash_code = op->GetHashCode();
    min_hash_code = std::min(min_hash_code, hash_code);
    max_hash_code = std::max(max_hash_code, hash_code);
  }

  if (txn) {
    RETURN_NOT_OK(txn->CommitFuture().get());
    LOG(INFO) << "Committed: " << txn->id();
  }

  LOG(INFO) << num_rows << " rows have been written";
  LOG(INFO) << "min_hash_code = " << min_hash_code;
  LOG(INFO) << "max_hash_code = " << max_hash_code;
  return std::make_pair(min_hash_code, max_hash_code);
}

template <class MiniClusterType>
Status TabletSplitITestBase<MiniClusterType>::FlushTestTable() {
  return this->client_->FlushTables(
      {this->table_->id()}, /* add_indexes = */ false, /* timeout_secs = */ 30,
      /* is_compaction = */ false);
}

template <class MiniClusterType>
Result<std::pair<docdb::DocKeyHash, docdb::DocKeyHash>>
    TabletSplitITestBase<MiniClusterType>::WriteRowsAndFlush(
        const uint32_t num_rows, const int32_t start_key) {
  auto result = VERIFY_RESULT(WriteRows(num_rows, start_key));
  RETURN_NOT_OK(FlushTestTable());
  return result;
}

template <class MiniClusterType>
Result<docdb::DocKeyHash> TabletSplitITestBase<MiniClusterType>::WriteRowsAndGetMiddleHashCode(
    uint32_t num_rows) {
  auto min_max_hash_code = VERIFY_RESULT(WriteRowsAndFlush(num_rows, 1));
  const auto split_hash_code = (min_max_hash_code.first + min_max_hash_code.second) / 2;
  LOG(INFO) << "Split hash code: " << split_hash_code;

  RETURN_NOT_OK(CheckRowsCount(num_rows));

  return split_hash_code;
}

template <class MiniClusterType>
Result<scoped_refptr<master::TabletInfo>>
    TabletSplitITestBase<MiniClusterType>::GetSingleTestTabletInfo(
        master::CatalogManagerIf* catalog_mgr) {
  auto tablet_infos = catalog_mgr->GetTableInfo(this->table_->id())->GetTablets();

  SCHECK_EQ(tablet_infos.size(), 1U, IllegalState, "Expect test table to have only 1 tablet");
  return tablet_infos.front();
}

template <class MiniClusterType>
void TabletSplitITestBase<MiniClusterType>::CheckTableKeysInRange(const size_t num_keys) {
  client::TableHandle table;
  ASSERT_OK(table.Open(client::kTableName, this->client_.get()));

  std::vector<int32> keys;
  for (const auto& row : client::TableRange(table)) {
    keys.push_back(row.column(0).int32_value());
  }

  LOG(INFO) << "Total rows read: " << keys.size();

  std::sort(keys.begin(), keys.end());
  int32 prev_key = 0;
  for (const auto& key : keys) {
    if (key != prev_key + 1) {
      LOG(ERROR) << "Keys missed: " << prev_key + 1 << "..." << key - 1;
    }
    prev_key = key;
  }
  LOG(INFO) << "Last key: " << prev_key;

  ASSERT_EQ(prev_key, num_keys);
  ASSERT_EQ(keys.size(), num_keys);
}

template <class MiniClusterType>
Result<bool> TabletSplitITestBase<MiniClusterType>::IsSplittingComplete(
    yb::master::MasterAdminProxy* master_proxy, bool wait_for_parent_deletion) {
  rpc::RpcController controller;
  controller.set_timeout(kRpcTimeout);
  master::IsTabletSplittingCompleteRequestPB is_tablet_splitting_complete_req;
  master::IsTabletSplittingCompleteResponsePB is_tablet_splitting_complete_resp;
  is_tablet_splitting_complete_req.set_wait_for_parent_deletion(wait_for_parent_deletion);

  RETURN_NOT_OK(master_proxy->IsTabletSplittingComplete(is_tablet_splitting_complete_req,
      &is_tablet_splitting_complete_resp, &controller));
  return is_tablet_splitting_complete_resp.is_tablet_splitting_complete();
}

template class TabletSplitITestBase<MiniCluster>;
template class TabletSplitITestBase<ExternalMiniCluster>;

//
// TabletSplitITest
//

TabletSplitITest::TabletSplitITest() = default;
TabletSplitITest::~TabletSplitITest() = default;

void TabletSplitITest::SetUp() {
  FLAGS_cleanup_split_tablets_interval_sec = 1;
  FLAGS_enable_automatic_tablet_splitting = false;
  FLAGS_TEST_validate_all_tablet_candidates = true;
  FLAGS_db_block_size_bytes = kDbBlockSizeBytes;
  // We set other block sizes to be small for following test reasons:
  // 1) To have more granular change of SST file size depending on number of rows written.
  // This helps to do splits earlier and have faster tests.
  // 2) To don't have long flushes when simulating slow compaction/flush. This way we can
  // test compaction abort faster.
  FLAGS_db_filter_block_size_bytes = 2_KB;
  FLAGS_db_index_block_size_bytes = 2_KB;
  // Split size threshold less than memstore size is not effective, because splits are triggered
  // based on flushed SST files size.
  FLAGS_db_write_buffer_size = 100_KB;
  TabletSplitITestBase<MiniCluster>::SetUp();
  snapshot_util_ = std::make_unique<client::SnapshotTestUtil>();
  snapshot_util_->SetProxy(&client_->proxy_cache());
  snapshot_util_->SetCluster(cluster_.get());
}

Result<master::TabletInfos> TabletSplitITest::GetTabletInfosForTable(const TableId& table_id) {
  return VERIFY_RESULT(catalog_manager())->GetTableInfo(table_id)->GetTablets();
}

Result<TabletId> TabletSplitITest::CreateSingleTabletAndSplit(uint32_t num_rows) {
  CreateSingleTablet();
  const auto split_hash_code = VERIFY_RESULT(WriteRowsAndGetMiddleHashCode(num_rows));
  return SplitTabletAndValidate(split_hash_code, num_rows);
}

Result<tserver::GetSplitKeyResponsePB> TabletSplitITest::GetSplitKey(const std::string& tablet_id) {
  auto tserver = cluster_->mini_tablet_server(0);
  auto ts_service_proxy = std::make_unique<tserver::TabletServerServiceProxy>(
      proxy_cache_.get(), HostPort::FromBoundEndpoint(tserver->bound_rpc_addr()));
  tserver::GetSplitKeyRequestPB req;
  req.set_tablet_id(tablet_id);
  rpc::RpcController controller;
  controller.set_timeout(kRpcTimeout);
  tserver::GetSplitKeyResponsePB resp;
  RETURN_NOT_OK(ts_service_proxy->GetSplitKey(req, &resp, &controller));
  return resp;
}

Result<master::SplitTabletResponsePB> TabletSplitITest::SendMasterSplitTabletRpcSync(
    const std::string& tablet_id) {
  auto master = cluster_->mini_master();
  auto master_admin_proxy =
      std::make_unique<master::MasterAdminProxy>(proxy_cache_.get(), master->bound_rpc_addr());

  master::SplitTabletRequestPB req;
  req.set_tablet_id(tablet_id);

  rpc::RpcController controller;
  controller.set_timeout(kRpcTimeout);
  master::SplitTabletResponsePB resp;
  RETURN_NOT_OK(master_admin_proxy->SplitTablet(req, &resp, &controller));
  return resp;
}

Status TabletSplitITest::WaitForTabletSplitCompletion(
    const size_t expected_non_split_tablets,
    const size_t expected_split_tablets,
    size_t num_replicas_online,
    const client::YBTableName& table,
    bool core_dump_on_failure) {
  if (num_replicas_online == 0) {
    num_replicas_online = FLAGS_replication_factor;
  }

  LOG(INFO) << "Waiting for tablet split to be completed... ";
  LOG(INFO) << "expected_non_split_tablets: " << expected_non_split_tablets;
  LOG(INFO) << "expected_split_tablets: " << expected_split_tablets;

  const auto expected_total_tablets = expected_non_split_tablets + expected_split_tablets;
  LOG(INFO) << "expected_total_tablets: " << expected_total_tablets;

  std::vector<tablet::TabletPeerPtr> peers;
  auto s = WaitFor([&] {
    peers = ListTabletPeers(cluster_.get(), ListPeersFilter::kAll);
    size_t num_peers_running = 0;
    size_t num_peers_split = 0;
    size_t num_peers_leader_ready = 0;
    for (const auto& peer : peers) {
      const auto tablet = peer->shared_tablet();
      const auto consensus = peer->shared_consensus();
      if (!tablet || !consensus) {
        break;
      }
      if (tablet->metadata()->table_name() != table.table_name() ||
          tablet->table_type() == TRANSACTION_STATUS_TABLE_TYPE) {
        continue;
      }
      const auto raft_group_state = peer->state();
      const auto tablet_data_state = tablet->metadata()->tablet_data_state();
      const auto leader_status = consensus->GetLeaderStatus(/* allow_stale =*/true);
      if (raft_group_state == tablet::RaftGroupStatePB::RUNNING) {
        ++num_peers_running;
      } else {
        return false;
      }
      num_peers_leader_ready += leader_status == consensus::LeaderStatus::LEADER_AND_READY;
      num_peers_split +=
          tablet_data_state == tablet::TabletDataState::TABLET_DATA_SPLIT_COMPLETED;
    }
    VLOG(1) << "num_peers_running: " << num_peers_running;
    VLOG(1) << "num_peers_split: " << num_peers_split;
    VLOG(1) << "num_peers_leader_ready: " << num_peers_leader_ready;

    return num_peers_running == num_replicas_online * expected_total_tablets &&
           num_peers_split == num_replicas_online * expected_split_tablets &&
           num_peers_leader_ready == expected_total_tablets;
  }, split_completion_timeout_sec_, "Wait for tablet split to be completed");
  if (!s.ok()) {
    for (const auto& peer : peers) {
      const auto tablet = peer->shared_tablet();
      const auto consensus = peer->shared_consensus();
      if (!tablet || !consensus) {
        LOG(INFO) << consensus::MakeTabletLogPrefix(peer->tablet_id(), peer->permanent_uuid())
                  << "no tablet";
        continue;
      }
      if (tablet->table_type() == TRANSACTION_STATUS_TABLE_TYPE) {
        continue;
      }
      LOG(INFO) << consensus::MakeTabletLogPrefix(peer->tablet_id(), peer->permanent_uuid())
                << "raft_group_state: " << AsString(peer->state())
                << " tablet_data_state: "
                << TabletDataState_Name(tablet->metadata()->tablet_data_state())
                << " leader status: "
                << AsString(consensus->GetLeaderStatus(/* allow_stale =*/true));
    }
    if (core_dump_on_failure) {
      LOG(INFO) << "Tablet splitting did not complete. Crashing test with core dump. "
                << "Received error: " << s.ToString();
      raise(SIGSEGV);
    } else {
      LOG(INFO) << "Tablet splitting did not complete. Received error: " << s.ToString();
      return s;
    }
  }
  LOG(INFO) << "Waiting for tablet split to be completed - DONE";

  DumpTableLocations(VERIFY_RESULT(catalog_manager()), table);
  return Status::OK();
}

Result<TabletId> TabletSplitITest::SplitSingleTablet(docdb::DocKeyHash split_hash_code) {
  auto* catalog_mgr = VERIFY_RESULT(catalog_manager());

  auto source_tablet_info = VERIFY_RESULT(GetSingleTestTabletInfo(catalog_mgr));
  const auto source_tablet_id = source_tablet_info->id();

  RETURN_NOT_OK(catalog_mgr->TEST_SplitTablet(source_tablet_info, split_hash_code));
  return source_tablet_id;
}

Result<master::SplitTabletResponsePB> TabletSplitITest::SplitSingleTablet(
    const TabletId& tablet_id) {
  auto master_admin_proxy = std::make_unique<master::MasterAdminProxy>(
      proxy_cache_.get(), client_->GetMasterLeaderAddress());
  rpc::RpcController controller;
  controller.set_timeout(kRpcTimeout);

  master::SplitTabletRequestPB req;
  master::SplitTabletResponsePB resp;
  req.set_tablet_id(tablet_id);

  RETURN_NOT_OK(master_admin_proxy->SplitTablet(req, &resp, &controller));
  return resp;
}

Result<TabletId> TabletSplitITest::SplitTabletAndValidate(
    docdb::DocKeyHash split_hash_code,
    size_t num_rows,
    bool parent_tablet_protected_from_deletion) {
  auto source_tablet_id = VERIFY_RESULT(SplitSingleTablet(split_hash_code));

  // If the parent tablet will not be deleted, then we will expect another tablet at the end.
  const auto expected_split_tablets =
      (FLAGS_TEST_skip_deleting_split_tablets || parent_tablet_protected_from_deletion) ? 1 : 0;

  RETURN_NOT_OK(
      WaitForTabletSplitCompletion(/* expected_non_split_tablets =*/2, expected_split_tablets));

  RETURN_NOT_OK(CheckPostSplitTabletReplicasData(num_rows));

  if (expected_split_tablets > 0) {
    RETURN_NOT_OK(CheckSourceTabletAfterSplit(source_tablet_id));
  }

  return source_tablet_id;
}

Status TabletSplitITest::CheckSourceTabletAfterSplit(const TabletId& source_tablet_id) {
  LOG(INFO) << "Checking source tablet behavior after split...";
  google::FlagSaver saver;
  ANNOTATE_UNPROTECTED_WRITE(FLAGS_TEST_do_not_start_election_test_only) = true;

  size_t tablet_split_insert_error_count = 0;
  size_t not_the_leader_insert_error_count = 0;
  size_t ts_online_count = 0;
  for (auto mini_ts : this->cluster_->mini_tablet_servers()) {
    if (!mini_ts->is_started()) {
      continue;
    }
    ++ts_online_count;
    auto ts_service_proxy = std::make_unique<tserver::TabletServerServiceProxy>(
        proxy_cache_.get(), HostPort::FromBoundEndpoint(mini_ts->bound_rpc_addr()));

    {
      tserver::ReadRequestPB req = VERIFY_RESULT(CreateReadRequest(source_tablet_id, 1 /* key */));

      rpc::RpcController controller;
      controller.set_timeout(kRpcTimeout);
      tserver::ReadResponsePB resp;
      RETURN_NOT_OK(ts_service_proxy->Read(req, &resp, &controller));

      SCHECK(resp.has_error(), InternalError, "Expected error on read from split tablet");
      SCHECK_EQ(
          resp.error().code(),
          tserver::TabletServerErrorPB::TABLET_SPLIT,
          InternalError,
          "Expected error on read from split tablet to be "
          "tserver::TabletServerErrorPB::TABLET_SPLIT");
    }

    {
      tserver::WriteRequestPB req =
          CreateInsertRequest(source_tablet_id, 0 /* key */, 0 /* value */);

      rpc::RpcController controller;
      controller.set_timeout(kRpcTimeout);
      tserver::WriteResponsePB resp;
      RETURN_NOT_OK(ts_service_proxy->Write(req, &resp, &controller));

      SCHECK(resp.has_error(), InternalError, "Expected error on write to split tablet");
      LOG(INFO) << "Error: " << AsString(resp.error());
      switch (resp.error().code()) {
        case tserver::TabletServerErrorPB::TABLET_SPLIT:
          SCHECK_EQ(
              resp.error().status().code(),
              AppStatusPB::ILLEGAL_STATE,
              InternalError,
              "tserver::TabletServerErrorPB::TABLET_SPLIT error should have "
              "AppStatusPB::ILLEGAL_STATE on write to split tablet");
          tablet_split_insert_error_count++;
          break;
        case tserver::TabletServerErrorPB::NOT_THE_LEADER:
          not_the_leader_insert_error_count++;
          break;
        case tserver::TabletServerErrorPB::TABLET_NOT_FOUND:
          // In the case that the source tablet was just hidden instead of deleted.
          tablet_split_insert_error_count++;
          break;
        default:
          return STATUS_FORMAT(InternalError, "Unexpected error: $0", resp.error());
      }
    }
  }
  SCHECK_EQ(
      tablet_split_insert_error_count, 1U, InternalError,
      "Leader should return \"try again\" error on insert.");
  SCHECK_EQ(
      not_the_leader_insert_error_count, ts_online_count - 1, InternalError,
      "Followers should return \"not the leader\" error.");
  return Status::OK();
}

Result<std::vector<tablet::TabletPeerPtr>> TabletSplitITest::ListSplitCompleteTabletPeers() {
  return ListTableInactiveSplitTabletPeers(this->cluster_.get(), VERIFY_RESULT(GetTestTableId()));
}

Result<std::vector<tablet::TabletPeerPtr>> TabletSplitITest::ListPostSplitChildrenTabletPeers() {
  return ListTableActiveTabletPeers(this->cluster_.get(), VERIFY_RESULT(GetTestTableId()));
}

Status TabletSplitITest::WaitForTestTablePostSplitTabletsFullyCompacted(MonoDelta timeout) {
  auto peer_to_str = [](const tablet::TabletPeerPtr& peer) {
    return peer->LogPrefix() +
           (peer->tablet_metadata()->has_been_fully_compacted() ? "Compacted" : "NotCompacted");
  };
  std::vector<std::string> not_compacted_peers;
  auto s = LoggedWaitFor(
      [this, &not_compacted_peers, &peer_to_str]() -> Result<bool> {
        auto peers = ListPostSplitChildrenTabletPeers();
        if (!peers.ok()) {
          return false;
        }
        LOG(INFO) << "Verifying post-split tablet peers:\n"
                  << JoinStrings(*peers | boost::adaptors::transformed(peer_to_str), "\n");
        not_compacted_peers.clear();
        for (auto peer : *peers) {
          if (!peer->tablet_metadata()->has_been_fully_compacted()) {
            not_compacted_peers.push_back(peer_to_str(peer));
          }
        }
        return not_compacted_peers.empty();
      },
      timeout, "Wait for post tablet split compaction to be completed");
  if (!s.ok()) {
    LOG(ERROR) << "Following post-split tablet peers have not been fully compacted:\n"
               << JoinStrings(not_compacted_peers, "\n");
  }
  return s;
}

Result<int> TabletSplitITest::NumPostSplitTabletPeersFullyCompacted() {
  int count = 0;
  for (auto peer : VERIFY_RESULT(ListPostSplitChildrenTabletPeers())) {
    const auto* tablet = peer->tablet();
    if (tablet->metadata()->has_been_fully_compacted()) {
      ++count;
    }
  }
  return count;
}

Result<uint64_t> TabletSplitITest::GetMinSstFileSizeAmongAllReplicas(const std::string& tablet_id) {
  const auto test_table_id = VERIFY_RESULT(GetTestTableId());
  auto peers = ListTabletPeers(this->cluster_.get(), [&tablet_id](auto peer) {
    return peer->tablet_id() == tablet_id;
  });
  if (peers.size() == 0) {
    return STATUS(IllegalState, "Table has no active peer tablets");
  }
  uint64_t min_file_size = std::numeric_limits<uint64_t>::max();
  for (const auto& peer : peers) {
    min_file_size = std::min(
        min_file_size,
        peer->shared_tablet()->GetCurrentVersionSstFilesSize());
  }
  return min_file_size;
}

Status TabletSplitITest::CheckPostSplitTabletReplicasData(
    size_t num_rows, size_t num_replicas_online, size_t num_active_tablets) {
  LOG(INFO) << "Checking post-split tablet replicas data...";

  if (num_replicas_online == 0) {
      num_replicas_online = FLAGS_replication_factor;
  }

  const auto test_table_id = VERIFY_RESULT(GetTestTableId());
  auto active_leader_peers = VERIFY_RESULT(WaitForTableActiveTabletLeadersPeers(
      this->cluster_.get(), test_table_id, num_active_tablets));

  std::unordered_map<TabletId, OpId> last_on_leader;
  for (auto peer : active_leader_peers) {
    last_on_leader[peer->tablet_id()] = peer->shared_consensus()->GetLastReceivedOpId();
  }

  const auto active_peers = ListTableActiveTabletPeers(this->cluster_.get(), test_table_id);

  std::vector<size_t> keys(num_rows, num_replicas_online);
  std::unordered_map<size_t, std::vector<std::string>> key_replicas;
  const auto key_column_id = this->table_.ColumnId(this->kKeyColumn);
  const auto value_column_id = this->table_.ColumnId(this->kValueColumn);
  for (auto peer : active_peers) {
    RETURN_NOT_OK(LoggedWaitFor(
        [&] {
          return peer->shared_consensus()->GetLastAppliedOpId() >=
                 last_on_leader[peer->tablet_id()];
        },
        15s * kTimeMultiplier,
        Format(
             "Waiting for tablet replica $0 to apply all ops from leader ...", peer->LogPrefix())));
    LOG(INFO) << "Last applied op id for " << peer->LogPrefix() << ": "
              << AsString(peer->shared_consensus()->GetLastAppliedOpId());

    const auto shared_tablet = peer->shared_tablet();
    const SchemaPtr schema = shared_tablet->metadata()->schema();
    auto client_schema = schema->CopyWithoutColumnIds();
    auto iter = VERIFY_RESULT(shared_tablet->NewRowIterator(client_schema));
    QLTableRow row;
    std::unordered_set<size_t> tablet_keys;
    while (VERIFY_RESULT(iter->HasNext())) {
      RETURN_NOT_OK(iter->NextRow(&row));
      auto key_opt = row.GetValue(key_column_id);
      SCHECK(key_opt.is_initialized(), InternalError, "Key is not initialized");
      SCHECK_EQ(key_opt, row.GetValue(value_column_id), InternalError, "Wrong value for key");
      auto key = key_opt->int32_value();
      SCHECK(
          tablet_keys.insert(key).second,
          InternalError,
          Format("Duplicate key $0 in tablet $1", key, shared_tablet->tablet_id()));
      SCHECK_GT(
          keys[key - 1]--,
          0U,
          InternalError,
          Format("Extra key $0 in tablet $1", key, shared_tablet->tablet_id()));
      key_replicas[key - 1].push_back(peer->LogPrefix());
    }
  }
  for (size_t key = 1; key <= num_rows; ++key) {
    const auto key_missing_in_replicas = keys[key - 1];
    if (key_missing_in_replicas > 0) {
      LOG(INFO) << Format("Key $0 replicas: $1", key, key_replicas[key - 1]);
      return STATUS_FORMAT(
          InternalError, "Missing key: $0 in $1 replicas", key, key_missing_in_replicas);
    }
  }
  return Status::OK();
}

//
// TabletSplitExternalMiniClusterITest
//

void TabletSplitExternalMiniClusterITest::SetFlags() {
  TabletSplitITestBase<ExternalMiniCluster>::SetFlags();
  for (const auto& master_flag : {
            "--enable_automatic_tablet_splitting=false",
            "--tablet_split_low_phase_shard_count_per_node=-1",
            "--tablet_split_high_phase_shard_count_per_node=-1",
            "--tablet_split_low_phase_size_threshold_bytes=-1",
            "--tablet_split_high_phase_size_threshold_bytes=-1",
            "--tablet_force_split_threshold_bytes=-1",
        }) {
    mini_cluster_opt_.extra_master_flags.push_back(master_flag);
  }

  for (const auto& tserver_flag : std::initializer_list<std::string>{
            Format("--db_block_size_bytes=$0", kDbBlockSizeBytes),
            "--cleanup_split_tablets_interval_sec=1",
            "--tserver_heartbeat_metrics_interval_ms=100",
        }) {
    mini_cluster_opt_.extra_tserver_flags.push_back(tserver_flag);
  }
}

Status TabletSplitExternalMiniClusterITest::SplitTablet(const std::string& tablet_id) {
  master::SplitTabletRequestPB req;
  req.set_tablet_id(tablet_id);
  master::SplitTabletResponsePB resp;
  rpc::RpcController rpc;
  rpc.set_timeout(30s * kTimeMultiplier);

  RETURN_NOT_OK(
      cluster_->GetLeaderMasterProxy<master::MasterAdminProxy>().SplitTablet(req, &resp, &rpc));
  if (resp.has_error()) {
    RETURN_NOT_OK(StatusFromPB(resp.error().status()));
  }
  return Status::OK();
}

Status TabletSplitExternalMiniClusterITest::FlushTabletsOnSingleTServer(
    size_t tserver_idx, const std::vector<yb::TabletId> tablet_ids, bool is_compaction) {
  auto tserver = cluster_->tablet_server(tserver_idx);
  RETURN_NOT_OK(cluster_->FlushTabletsOnSingleTServer(tserver, tablet_ids, is_compaction));
  return Status::OK();
}

Result<std::set<TabletId>> TabletSplitExternalMiniClusterITest::GetTestTableTabletIds(
    size_t tserver_idx) {
  std::set<TabletId> tablet_ids;
  auto res = VERIFY_RESULT(cluster_->GetTablets(cluster_->tablet_server(tserver_idx)));

  for (const auto& tablet : res) {
    if (tablet.table_name() == table_->name().table_name() &&
        // Skip deleted (tombstoned) tablets.
        tablet.state() != tablet::RaftGroupStatePB::SHUTDOWN) {
      tablet_ids.insert(tablet.tablet_id());
    }
  }
  return tablet_ids;
}

Result<std::set<TabletId>> TabletSplitExternalMiniClusterITest::GetTestTableTabletIds() {
  std::set<TabletId> tablet_ids;
  for (size_t i = 0; i < cluster_->num_tablet_servers(); ++i) {
    if (cluster_->tablet_server(i)->IsShutdown() || cluster_->tablet_server(i)->IsProcessPaused()) {
      continue;
    }
    auto res = VERIFY_RESULT(GetTestTableTabletIds(i));
    for (const auto& id : res) {
      tablet_ids.insert(id);
    }
  }
  return tablet_ids;
}

Result<vector<tserver::ListTabletsResponsePB_StatusAndSchemaPB>>
    TabletSplitExternalMiniClusterITest::ListTablets(size_t tserver_idx) {
  vector<tserver::ListTabletsResponsePB_StatusAndSchemaPB> tablets;
  std::set<TabletId> tablet_ids;
  auto res = VERIFY_RESULT(cluster_->ListTablets(cluster_->tablet_server(tserver_idx)));
  for (const auto& tablet : res.status_and_schema()) {
    auto tablet_id = tablet.tablet_status().tablet_id();
    if (tablet.tablet_status().table_name() == table_->name().table_name() &&
        tablet_ids.find(tablet_id) == tablet_ids.end()) {
      tablets.push_back(tablet);
      tablet_ids.insert(tablet_id);
    }
  }
  return tablets;
}

Result<vector<tserver::ListTabletsResponsePB_StatusAndSchemaPB>>
    TabletSplitExternalMiniClusterITest::ListTablets() {
  vector<tserver::ListTabletsResponsePB_StatusAndSchemaPB> tablets;
  std::set<TabletId> tablet_ids;
  for (size_t i = 0; i < cluster_->num_tablet_servers(); ++i) {
    auto res = VERIFY_RESULT(ListTablets(i));
    for (const auto& tablet : res) {
      auto tablet_id = tablet.tablet_status().tablet_id();
      if (tablet_ids.find(tablet_id) == tablet_ids.end()) {
          tablets.push_back(tablet);
          tablet_ids.insert(tablet_id);
      }
    }
  }
  return tablets;
}

Status TabletSplitExternalMiniClusterITest::WaitForTabletsExcept(
    size_t num_tablets, size_t tserver_idx, const TabletId& exclude_tablet) {
  std::set<TabletId> tablets;
  auto status = LoggedWaitFor(
      [&]() -> Result<bool> {
        tablets = VERIFY_RESULT(GetTestTableTabletIds(tserver_idx));
        size_t count = 0;
        for (auto& tablet_id : tablets) {
          if (tablet_id != exclude_tablet) {
            count++;
          }
        }
        return count == num_tablets;
      },
      30s * kTimeMultiplier,
      Format(
          "Waiting for tablet count: $0 at tserver: $1",
          num_tablets,
          cluster_->tablet_server(tserver_idx)->uuid()));
  if (!status.ok()) {
    status = status.CloneAndAppend(Format("Got tablets: $0", tablets));
  }
  return status;
}

Status TabletSplitExternalMiniClusterITest::WaitForTablets(size_t num_tablets, size_t tserver_idx) {
  return WaitForTabletsExcept(num_tablets, tserver_idx, "");
}

Status TabletSplitExternalMiniClusterITest::WaitForTablets(size_t num_tablets) {
  std::set<TabletId> tablets;
  auto status = WaitFor([&]() -> Result<bool> {
    tablets = VERIFY_RESULT(GetTestTableTabletIds());
    return tablets.size() == num_tablets;
  }, 20s * kTimeMultiplier, Format("Waiting for tablet count: $0", num_tablets));
  if (!status.ok()) {
    status = status.CloneAndAppend(Format("Got tablets: $0", tablets));
  }
  return status;
}

Result<TabletId> TabletSplitExternalMiniClusterITest::GetOnlyTestTabletId(size_t tserver_idx) {
  auto tablet_ids = VERIFY_RESULT(GetTestTableTabletIds(tserver_idx));
  if (tablet_ids.size() != 1) {
    return STATUS(InternalError, "Expected one tablet");
  }
  return *tablet_ids.begin();
}

Result<TabletId> TabletSplitExternalMiniClusterITest::GetOnlyTestTabletId() {
  auto tablet_ids = VERIFY_RESULT(GetTestTableTabletIds());
  if (tablet_ids.size() != 1) {
    return STATUS(InternalError, Format("Expected one tablet, got $0", tablet_ids.size()));
  }
  return *tablet_ids.begin();
}

Status TabletSplitExternalMiniClusterITest::SplitTabletCrashMaster(
    bool change_split_boundary, string* split_partition_key) {
  CreateSingleTablet();
  int key = 1, num_rows = 2000;
  RETURN_NOT_OK(WriteRowsAndFlush(num_rows, key));
  key += num_rows;
  auto tablet_id = CHECK_RESULT(GetOnlyTestTabletId());

  RETURN_NOT_OK(cluster_->SetFlagOnMasters("TEST_crash_after_creating_single_split_tablet", "1.0"));
  // Split tablet should crash before creating either tablet
  if (split_partition_key) {
    auto res = VERIFY_RESULT(cluster_->GetSplitKey(tablet_id));
    *split_partition_key = res.split_partition_key();
  }
  RETURN_NOT_OK(SplitTablet(tablet_id));
  auto status = WaitForTablets(3);
  if (status.ok()) {
    return STATUS(IllegalState, "Tablet should not have split");
  }

  RETURN_NOT_OK(RestartAllMasters(cluster_.get()));
  RETURN_NOT_OK(cluster_->SetFlagOnMasters("TEST_crash_after_creating_single_split_tablet", "0.0"));

  if (change_split_boundary) {
    RETURN_NOT_OK(WriteRows(num_rows * 2, key));
    for (size_t i = 0; i < cluster_->num_tablet_servers(); i++) {
      RETURN_NOT_OK(FlushTabletsOnSingleTServer(i, {tablet_id}, false));
    }
  }

  // Wait for tablet split to complete
  auto raft_heartbeat_roundtrip_time = FLAGS_raft_heartbeat_interval_ms * 2ms;
  RETURN_NOT_OK(LoggedWaitFor(
      [this, tablet_id]() -> Result<bool> {
        auto status = SplitTablet(tablet_id);
        if (!status.ok()) {
          return false;
        }
        return WaitForTablets(3).ok();
      },
      5 * raft_heartbeat_roundtrip_time * kTimeMultiplier
      + 2ms * FLAGS_tserver_heartbeat_metrics_interval_ms,
      Format("Wait for tablet to be split: $0", tablet_id)));

  // Wait for parent tablet clean up
  std::this_thread::sleep_for(5 * raft_heartbeat_roundtrip_time * kTimeMultiplier);
  RETURN_NOT_OK(WaitForTablets(2));

  return Status::OK();
}

}  // namespace yb
