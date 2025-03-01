// Licensed to the Apache Software Foundation (ASF) under one
// or more contributor license agreements.  See the NOTICE file
// distributed with this work for additional information
// regarding copyright ownership.  The ASF licenses this file
// to you under the Apache License, Version 2.0 (the
// "License"); you may not use this file except in compliance
// with the License.  You may obtain a copy of the License at
//
//   http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing,
// software distributed under the License is distributed on an
// "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
// KIND, either express or implied.  See the License for the
// specific language governing permissions and limitations
// under the License.
//
// The following only applies to changes made to this file as part of YugaByte development.
//
// Portions Copyright (c) YugaByte, Inc.
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

#include "yb/master/sys_catalog.h"

#include <cmath>
#include <memory>

#include <boost/optional.hpp>
#include <gflags/gflags.h>
#include <glog/logging.h>
#include <rapidjson/document.h>

#include "yb/client/client.h"

#include "yb/common/index.h"
#include "yb/common/partial_row.h"
#include "yb/common/partition.h"
#include "yb/common/placement_info.h"
#include "yb/common/ql_value.h"
#include "yb/common/schema.h"
#include "yb/common/wire_protocol.h"

#include "yb/consensus/consensus.h"
#include "yb/consensus/consensus_meta.h"
#include "yb/consensus/consensus_peers.h"
#include "yb/consensus/multi_raft_batcher.h"
#include "yb/consensus/log.h"
#include "yb/consensus/log_anchor_registry.h"
#include "yb/consensus/opid_util.h"
#include "yb/consensus/quorum_util.h"
#include "yb/consensus/state_change_context.h"

#include "yb/docdb/doc_rowwise_iterator.h"
#include "yb/docdb/docdb_pgapi.h"

#include "yb/fs/fs_manager.h"

#include "yb/gutil/bind.h"
#include "yb/gutil/casts.h"
#include "yb/gutil/strings/escaping.h"
#include "yb/gutil/strings/split.h"

#include "yb/master/catalog_entity_info.h"
#include "yb/master/catalog_manager_if.h"
#include "yb/master/master.h"
#include "yb/master/master_util.h"
#include "yb/master/sys_catalog_writer.h"

#include "yb/tablet/operations/write_operation.h"
#include "yb/tablet/tablet.h"
#include "yb/tablet/tablet_bootstrap_if.h"
#include "yb/tablet/tablet_metadata.h"
#include "yb/tablet/tablet_options.h"
#include "yb/tablet/tablet_peer.h"
#include "yb/tablet/write_query.h"

#include "yb/tserver/tablet_memory_manager.h"
#include "yb/tserver/ts_tablet_manager.h"
#include "yb/tserver/tserver.pb.h"

#include "yb/util/debug/trace_event.h"
#include "yb/util/flag_tags.h"
#include "yb/util/format.h"
#include "yb/util/logging.h"
#include "yb/util/metrics.h"
#include "yb/util/net/dns_resolver.h"
#include "yb/util/random_util.h"
#include "yb/util/size_literals.h"
#include "yb/util/status_format.h"
#include "yb/util/status_log.h"
#include "yb/util/threadpool.h"

using namespace std::literals; // NOLINT
using namespace yb::size_literals;

using std::shared_ptr;
using std::unique_ptr;

using yb::consensus::CONSENSUS_CONFIG_ACTIVE;
using yb::consensus::CONSENSUS_CONFIG_COMMITTED;
using yb::consensus::ConsensusMetadata;
using yb::consensus::RaftConfigPB;
using yb::consensus::RaftPeerPB;
using yb::log::Log;
using yb::log::LogAnchorRegistry;
using yb::tserver::WriteRequestPB;
using yb::tserver::WriteResponsePB;
using strings::Substitute;
using yb::consensus::StateChangeContext;
using yb::consensus::StateChangeReason;
using yb::consensus::ChangeConfigRequestPB;
using yb::consensus::ChangeConfigRecordPB;

DEFINE_bool(notify_peer_of_removal_from_cluster, true,
            "Notify a peer after it has been removed from the cluster.");
TAG_FLAG(notify_peer_of_removal_from_cluster, hidden);
TAG_FLAG(notify_peer_of_removal_from_cluster, advanced);

METRIC_DEFINE_coarse_histogram(
  server, dns_resolve_latency_during_sys_catalog_setup,
  "yb.master.SysCatalogTable.SetupConfig DNS Resolve",
  yb::MetricUnit::kMicroseconds,
  "Microseconds spent resolving DNS requests during SysCatalogTable::SetupConfig");
METRIC_DEFINE_counter(
  server, sys_catalog_peer_write_count,
  "yb.master.SysCatalogTable Count of Writes",
  yb::MetricUnit::kRequests,
  "Number of writes to disk handled by the system catalog.");

DECLARE_bool(create_initial_sys_catalog_snapshot);
DECLARE_int32(master_discovery_timeout_ms);

DEFINE_int32(sys_catalog_write_timeout_ms, 60000, "Timeout for writes into system catalog");
DEFINE_uint64(copy_tables_batch_bytes, 500_KB, "Max bytes per batch for copy pg sql tables");

DEFINE_test_flag(int32, sys_catalog_write_rejection_percentage, 0,
  "Reject specified percentage of sys catalog writes.");

namespace yb {
namespace master {

constexpr int32_t kDefaultMasterBlockCacheSizePercentage = 25;

std::string SysCatalogTable::schema_column_type() { return kSysCatalogTableColType; }

std::string SysCatalogTable::schema_column_id() { return kSysCatalogTableColId; }

std::string SysCatalogTable::schema_column_metadata() { return kSysCatalogTableColMetadata; }

SysCatalogTable::SysCatalogTable(Master* master, MetricRegistry* metrics,
                                 ElectedLeaderCallback leader_cb)
    : doc_read_context_(std::make_unique<docdb::DocReadContext>(
          BuildTableSchema(), kSysCatalogSchemaVersion)),
      metric_registry_(metrics),
      metric_entity_(METRIC_ENTITY_server.Instantiate(metric_registry_, "yb.master")),
      master_(master),
      leader_cb_(std::move(leader_cb)) {
  CHECK_OK(ThreadPoolBuilder("inform_removed_master").Build(&inform_removed_master_pool_));
  CHECK_OK(ThreadPoolBuilder("raft").Build(&raft_pool_));
  CHECK_OK(ThreadPoolBuilder("prepare").set_min_threads(1).Build(&tablet_prepare_pool_));
  CHECK_OK(ThreadPoolBuilder("append").set_min_threads(1).Build(&append_pool_));
  CHECK_OK(ThreadPoolBuilder("log-sync")
              .set_min_threads(1).Build(&log_sync_pool_));
  CHECK_OK(ThreadPoolBuilder("log-alloc").set_min_threads(1).Build(&allocation_pool_));

  setup_config_dns_histogram_ = METRIC_dns_resolve_latency_during_sys_catalog_setup.Instantiate(
      metric_entity_);
  peer_write_count = METRIC_sys_catalog_peer_write_count.Instantiate(metric_entity_);
}

SysCatalogTable::~SysCatalogTable() {
}

void SysCatalogTable::StartShutdown() {
  if (mem_manager_) {
    mem_manager_->Shutdown();
  }
  auto peer = tablet_peer();
  if (peer) {
    CHECK(peer->StartShutdown());
  }

  if (multi_raft_manager_) {
    multi_raft_manager_->StartShutdown();
  }
}

void SysCatalogTable::CompleteShutdown() {
  auto peer = tablet_peer();
  if (peer) {
    peer->CompleteShutdown(tablet::DisableFlushOnShutdown::kFalse);
  }
  inform_removed_master_pool_->Shutdown();
  raft_pool_->Shutdown();
  tablet_prepare_pool_->Shutdown();
  if (multi_raft_manager_) {
    multi_raft_manager_->CompleteShutdown();
  }
}

Status SysCatalogTable::ConvertConfigToMasterAddresses(
    const RaftConfigPB& config,
    bool check_missing_uuids) {
  auto loaded_master_addresses = std::make_shared<server::MasterAddresses>();
  bool has_missing_uuids = false;
  for (const auto& peer : config.peers()) {
    if (check_missing_uuids && !peer.has_permanent_uuid()) {
      LOG(WARNING) << "No uuid for master peer: " << peer.ShortDebugString();
      has_missing_uuids = true;
      break;
    }

    loaded_master_addresses->push_back({});
    auto& list = loaded_master_addresses->back();
    for (const auto& hp : peer.last_known_private_addr()) {
      list.push_back(HostPortFromPB(hp));
    }
    for (const auto& hp : peer.last_known_broadcast_addr()) {
      list.push_back(HostPortFromPB(hp));
    }
  }

  if (has_missing_uuids) {
    return STATUS(IllegalState, "Trying to load distributed config, but had missing uuids.");
  }

  master_->SetMasterAddresses(loaded_master_addresses);

  return Status::OK();
}

Status SysCatalogTable::CreateAndFlushConsensusMeta(
    FsManager* fs_manager,
    const RaftConfigPB& config,
    int64_t current_term) {
  std::unique_ptr<ConsensusMetadata> cmeta;
  string tablet_id = kSysCatalogTabletId;
  RETURN_NOT_OK_PREPEND(ConsensusMetadata::Create(fs_manager,
                                                  tablet_id,
                                                  fs_manager->uuid(),
                                                  config,
                                                  current_term,
                                                  &cmeta),
                        "Unable to persist consensus metadata for tablet " + tablet_id);
  return Status::OK();
}

Status SysCatalogTable::Load(FsManager* fs_manager) {
  LOG(INFO) << "Trying to load previous SysCatalogTable data from disk";
  // Load Metadata Information from disk
  auto metadata = VERIFY_RESULT(tablet::RaftGroupMetadata::Load(fs_manager, kSysCatalogTabletId));

  // Verify that the schema is the current one
  if (!metadata->schema()->Equals(doc_read_context_->schema)) {
    // TODO: In this case we probably should execute the migration step.
    return(STATUS(Corruption, "Unexpected schema", metadata->schema()->ToString()));
  }

  // Update partition schema of old SysCatalogTable. SysCatalogTable should be non-partitioned.
  if (metadata->partition_schema()->IsHashPartitioning()) {
    LOG(INFO) << "Updating partition schema of SysCatalogTable ...";
    PartitionSchema partition_schema;
    RETURN_NOT_OK(PartitionSchema::FromPB(PartitionSchemaPB(), *metadata->schema(),
                                          &partition_schema));
    metadata->SetPartitionSchema(partition_schema);
    RETURN_NOT_OK(metadata->Flush());
  }

  // TODO(bogdan) we should revisit this as well as next step to understand what happens if you
  // started on this local config, but the consensus layer has a different config? (essentially,
  // if your local cmeta is stale...
  //
  // Allow for statically and explicitly assigning the consensus configuration and roles through
  // the master configuration on startup.
  //
  // TODO: The following assumptions need revisiting:
  // 1. We always believe the local config options for who is in the consensus configuration.
  // 2. We always want to look up all node's UUIDs on start (via RPC).
  //    - TODO: Cache UUIDs. See KUDU-526.
  string tablet_id = metadata->raft_group_id();
  std::unique_ptr<ConsensusMetadata> cmeta;
  RETURN_NOT_OK_PREPEND(ConsensusMetadata::Load(fs_manager, tablet_id, fs_manager->uuid(), &cmeta),
                        "Unable to load consensus metadata for tablet " + tablet_id);

  const RaftConfigPB& loaded_config = cmeta->active_config();
  DCHECK(!loaded_config.peers().empty()) << "Loaded consensus metadata, but had no peers!";

  if (loaded_config.peers().empty()) {
    return STATUS(IllegalState, "Trying to load distributed config, but contains no peers.");
  }

  if (loaded_config.peers().size() > 1) {
    LOG(INFO) << "Configuring consensus for distributed operation...";
    RETURN_NOT_OK(ConvertConfigToMasterAddresses(loaded_config, true));
  } else {
    LOG(INFO) << "Configuring consensus for local operation...";
    // We know we have exactly one peer.
    const auto& peer = loaded_config.peers().Get(0);
    if (!peer.has_permanent_uuid()) {
      return STATUS(IllegalState, "Loaded consesnsus metadata, but peer did not have a uuid");
    }
    if (peer.permanent_uuid() != fs_manager->uuid()) {
      return STATUS(IllegalState, Substitute(
          "Loaded consensus metadata, but peer uuid ($0) was different than our uuid ($1)",
          peer.permanent_uuid(), fs_manager->uuid()));
    }
  }

  RETURN_NOT_OK(SetupTablet(metadata));
  return Status::OK();
}

Status SysCatalogTable::CreateNew(FsManager *fs_manager) {
  LOG(INFO) << "Creating new SysCatalogTable data";
  // Create the new Metadata
  Schema schema = BuildTableSchema();
  PartitionSchema partition_schema;
  RETURN_NOT_OK(PartitionSchema::FromPB(PartitionSchemaPB(), schema, &partition_schema));

  vector<YBPartialRow> split_rows;
  vector<Partition> partitions;
  RETURN_NOT_OK(partition_schema.CreatePartitions(split_rows, schema, &partitions));
  DCHECK_EQ(1, partitions.size());

  auto table_info = std::make_shared<tablet::TableInfo>(
      tablet::Primary::kTrue, kSysCatalogTableId, "", table_name(), TableType::YQL_TABLE_TYPE,
      schema, IndexMap(), boost::none /* index_info */, 0 /* schema_version */, partition_schema);
  string data_root_dir = fs_manager->GetDataRootDirs()[0];
  fs_manager->SetTabletPathByDataPath(kSysCatalogTabletId, data_root_dir);
  auto metadata = VERIFY_RESULT(tablet::RaftGroupMetadata::CreateNew(tablet::RaftGroupMetadataData {
    .fs_manager = fs_manager,
    .table_info = table_info,
    .raft_group_id = kSysCatalogTabletId,
    .partition = partitions[0],
    .tablet_data_state = tablet::TABLET_DATA_READY,
    .snapshot_schedules = {},
  }, data_root_dir));

  RaftConfigPB config;
  RETURN_NOT_OK_PREPEND(SetupConfig(master_->opts(), &config),
                        "Failed to initialize distributed config");

  RETURN_NOT_OK(CreateAndFlushConsensusMeta(fs_manager, config, consensus::kMinimumTerm));

  return SetupTablet(metadata);
}

Status SysCatalogTable::SetupConfig(const MasterOptions& options,
                                    RaftConfigPB* committed_config) {
  // Build the set of followers from our server options.
  auto master_addresses = options.GetMasterAddresses();  // ENG-285

  // Now resolve UUIDs.
  // By the time a SysCatalogTable is created and initted, the masters should be
  // starting up, so this should be fine to do.
  DCHECK(master_->messenger());
  RaftConfigPB resolved_config;
  resolved_config.set_opid_index(consensus::kInvalidOpIdIndex);

  ScopedDnsTracker dns_tracker(setup_config_dns_histogram_);
  for (const auto& list : *options.GetMasterAddresses()) {
    LOG(INFO) << "Determining permanent_uuid for " + yb::ToString(list);
    RaftPeerPB new_peer;
    // TODO: Use ConsensusMetadata to cache the results of these lookups so
    // we only require RPC access to the full consensus configuration on first startup.
    // See KUDU-526.
    RETURN_NOT_OK_PREPEND(
      consensus::SetPermanentUuidForRemotePeer(
        &master_->proxy_cache(),
        std::chrono::milliseconds(FLAGS_master_discovery_timeout_ms),
        list,
        &new_peer),
      Format("Unable to resolve UUID for $0", yb::ToString(list)));
    resolved_config.add_peers()->Swap(&new_peer);
  }

  LOG(INFO) << "Setting up raft configuration: " << resolved_config.ShortDebugString();

  RETURN_NOT_OK(consensus::VerifyRaftConfig(resolved_config, consensus::COMMITTED_QUORUM));

  *committed_config = resolved_config;
  return Status::OK();
}

void SysCatalogTable::SysCatalogStateChanged(
    const string& tablet_id,
    std::shared_ptr<StateChangeContext> context) {
  CHECK_EQ(tablet_id, tablet_peer()->tablet_id());
  shared_ptr<consensus::Consensus> consensus = tablet_peer()->shared_consensus();
  if (!consensus) {
    LOG_WITH_PREFIX(WARNING) << "Received notification of tablet state change "
                             << "but tablet no longer running. Tablet ID: "
                             << tablet_id << ". Reason: " << context->ToString();
    return;
  }

  // We use the active config, in case there is a pending one with this peer becoming the voter,
  // that allows its role to be determined correctly as the LEADER and so loads the sys catalog.
  // Done as part of ENG-286.
  consensus::ConsensusStatePB cstate = context->is_config_locked() ?
      consensus->ConsensusStateUnlocked(CONSENSUS_CONFIG_ACTIVE) :
      consensus->ConsensusState(CONSENSUS_CONFIG_ACTIVE);
  LOG_WITH_PREFIX(INFO) << "SysCatalogTable state changed. Locked=" << context->is_config_locked_
                        << ". Reason: " << context->ToString()
                        << ". Latest consensus state: " << cstate.ShortDebugString();
  PeerRole role = GetConsensusRole(tablet_peer()->permanent_uuid(), cstate);
  LOG_WITH_PREFIX(INFO) << "This master's current role is: "
                        << PeerRole_Name(role);

  // For LEADER election case only, load the sysCatalog into memory via the callback.
  // Note that for a *single* master case, the TABLET_PEER_START is being overloaded to imply a
  // leader creation step, as there is no election done per-se.
  // For the change config case, LEADER is the one which started the operation, so new role is same
  // as its old role of LEADER and hence it need not reload the sysCatalog via the callback.
  if (role == PeerRole::LEADER &&
      (context->reason == StateChangeReason::NEW_LEADER_ELECTED ||
       (cstate.config().peers_size() == 1 &&
        context->reason == StateChangeReason::TABLET_PEER_STARTED))) {
    CHECK_OK(leader_cb_.Run());
  }

  if (context->reason == StateChangeReason::NEW_LEADER_ELECTED) {
    auto client_future = master_->async_client_initializer().get_client_future();

    // Check if client was already initialized, otherwise we don't have to refresh master leader,
    // since it will be fetched as part of initialization.
    if (client_future.wait_for(0ms) == std::future_status::ready) {
      client_future.get()->RefreshMasterLeaderAddressAsync();
    }
  }

  // Perform any further changes for context based reasons.
  // For config change peer update, both leader and follower need to update their in-memory state.
  // NOTE: if there are any errors, we check in debug mode, but ignore the error in non-debug case.
  if (context->reason == StateChangeReason::LEADER_CONFIG_CHANGE_COMPLETE ||
      context->reason == StateChangeReason::FOLLOWER_CONFIG_CHANGE_COMPLETE) {
    int new_count = context->change_record.new_config().peers_size();
    int old_count = context->change_record.old_config().peers_size();

    LOG(INFO) << "Processing context '" << context->ToString()
              << "' - new count " << new_count << ", old count " << old_count;

    // If new_config and old_config have the same number of peers, then the change config must have
    // been a ROLE_CHANGE, thus old_config must have exactly one peer in transition (PRE_VOTER or
    // PRE_OBSERVER) and new_config should have none.
    if (new_count == old_count) {
      auto old_config_peers_transition_count =
          CountServersInTransition(context->change_record.old_config());
      if (old_config_peers_transition_count != 1) {
        LOG(FATAL) << "Expected old config to have one server in transition (PRE_VOTER or "
                   << "PRE_OBSERVER), but found " << old_config_peers_transition_count
                   << ". Config: " << context->change_record.old_config().ShortDebugString();
      }
      auto new_config_peers_transition_count =
          CountServersInTransition(context->change_record.new_config());
      if (new_config_peers_transition_count != 0) {
        LOG(FATAL) << "Expected new config to have no servers in transition (PRE_VOTER or "
                   << "PRE_OBSERVER), but found " << new_config_peers_transition_count
                   << ". Config: " << context->change_record.old_config().ShortDebugString();
      }
    } else if (std::abs(new_count - old_count) != 1) {

      LOG(FATAL) << "Expected exactly one server addition or deletion, found " << new_count
                 << " servers in new config and " << old_count << " servers in old config.";
      return;
    }

    Status s = master_->ResetMemoryState(context->change_record.new_config());
    if (!s.ok()) {
      LOG(WARNING) << "Change Memory state failed " << s.ToString();
      DCHECK(false);
      return;
    }

    // Try to make the removed master, go back to shell mode so as not to ping this cluster.
    // This is best effort and should not perform any fatals or checks.
    if (FLAGS_notify_peer_of_removal_from_cluster &&
        context->reason == StateChangeReason::LEADER_CONFIG_CHANGE_COMPLETE &&
        context->remove_uuid != "") {
      RaftPeerPB peer;
      LOG(INFO) << "Asking " << context->remove_uuid << " to go into shell mode";
      WARN_NOT_OK(GetRaftConfigMember(context->change_record.old_config(),
                                      context->remove_uuid,
                                      &peer),
                  Substitute("Could not find uuid=$0 in config.", context->remove_uuid));
      WARN_NOT_OK(
          inform_removed_master_pool_->SubmitFunc(
              [this, host_port = DesiredHostPort(peer, master_->MakeCloudInfoPB())]() {
            WARN_NOT_OK(master_->InformRemovedMaster(host_port),
                        "Failed to inform removed master " + host_port.ShortDebugString());
          }),
          Substitute("Error submitting removal task for uuid=$0", context->remove_uuid));
    }
  } else {
    VLOG(2) << "Reason '" << context->ToString() << "' provided in state change context, "
            << "no action needed.";
  }
}

Status SysCatalogTable::GoIntoShellMode() {
  CHECK(tablet_peer());
  StartShutdown();
  CompleteShutdown();

  // Remove on-disk log, cmeta and tablet superblocks.
  RETURN_NOT_OK(tserver::DeleteTabletData(tablet_peer()->tablet_metadata(),
                                          tablet::TABLET_DATA_DELETED,
                                          master_->fs_manager()->uuid(),
                                          yb::OpId()));
  RETURN_NOT_OK(tablet_peer()->tablet_metadata()->DeleteSuperBlock());
  RETURN_NOT_OK(master_->fs_manager()->DeleteFileSystemLayout());
  std::shared_ptr<tablet::TabletPeer> null_tablet_peer(nullptr);
  std::atomic_store(&tablet_peer_, null_tablet_peer);
  inform_removed_master_pool_.reset();
  raft_pool_.reset();
  tablet_prepare_pool_.reset();

  return Status::OK();
}

void SysCatalogTable::SetupTabletPeer(const scoped_refptr<tablet::RaftGroupMetadata>& metadata) {
  InitLocalRaftPeerPB();

  multi_raft_manager_ = std::make_unique<consensus::MultiRaftManager>(master_->messenger(),
                                                                      &master_->proxy_cache(),
                                                                      local_peer_pb_.cloud_info());

  // TODO: handle crash mid-creation of tablet? do we ever end up with a
  // partially created tablet here?
  auto tablet_peer = std::make_shared<tablet::TabletPeer>(
      metadata,
      local_peer_pb_,
      scoped_refptr<server::Clock>(master_->clock()),
      metadata->fs_manager()->uuid(),
      Bind(&SysCatalogTable::SysCatalogStateChanged, Unretained(this), metadata->raft_group_id()),
      metric_registry_,
      nullptr /* tablet_splitter */,
      master_->async_client_initializer().get_client_future());

  std::atomic_store(&tablet_peer_, tablet_peer);
}

Status SysCatalogTable::SetupTablet(const scoped_refptr<tablet::RaftGroupMetadata>& metadata) {
  SetupTabletPeer(metadata);

  RETURN_NOT_OK(OpenTablet(metadata));

  return Status::OK();
}

Status SysCatalogTable::OpenTablet(const scoped_refptr<tablet::RaftGroupMetadata>& metadata) {
  CHECK(tablet_peer());

  tablet::TabletPtr tablet;
  scoped_refptr<Log> log;
  consensus::ConsensusBootstrapInfo consensus_info;
  RETURN_NOT_OK(tablet_peer()->SetBootstrapping());
  tablet::TabletOptions tablet_options;

  // Returns a vector that includes the tablet peer associated with master.
  const auto get_peers_lambda = [this]() -> std::vector<tablet::TabletPeerPtr> {
    return { tablet_peer() };
  };

  mem_manager_ = std::make_shared<tserver::TabletMemoryManager>(
      &tablet_options,
      master_->mem_tracker(),
      kDefaultMasterBlockCacheSizePercentage,
      GetMetricEntity(),
      get_peers_lambda);

  tablet::TabletInitData tablet_init_data = {
      .metadata = metadata,
      .client_future = master_->async_client_initializer().get_client_future(),
      .clock = scoped_refptr<server::Clock>(master_->clock()),
      .parent_mem_tracker = master_->mem_tracker(),
      .block_based_table_mem_tracker = mem_manager_->block_based_table_mem_tracker(),
      .metric_registry = metric_registry_,
      .log_anchor_registry = tablet_peer()->log_anchor_registry(),
      .tablet_options = tablet_options,
      .log_prefix_suffix = " P " + tablet_peer()->permanent_uuid(),
      .transaction_participant_context = tablet_peer().get(),
      .local_tablet_filter = client::LocalTabletFilter(),
      // This is only required if the sys catalog tablet is also acting as a transaction status
      // tablet, which it does not as of 12/06/2019. This could have been a nullptr, but putting
      // the TabletPeer here in case we need this for rolling master upgrades when we do enable
      // storing transaction status records in the sys catalog tablet.
      .transaction_coordinator_context = tablet_peer().get(),
      // Disable transactions if we are creating the initial sys catalog snapshot.
      // initdb is much faster with transactions disabled.
      .txns_enabled = tablet::TransactionsEnabled(!FLAGS_create_initial_sys_catalog_snapshot),
      .is_sys_catalog = tablet::IsSysCatalogTablet::kTrue,
      .snapshot_coordinator = &master_->catalog_manager()->snapshot_coordinator(),
      .tablet_splitter = nullptr,
      .allowed_history_cutoff_provider = nullptr,
      .transaction_manager_provider = nullptr,
  };
  tablet::BootstrapTabletData data = {
      .tablet_init_data = tablet_init_data,
      .listener = tablet_peer()->status_listener(),
      .append_pool = append_pool(),
      .allocation_pool = allocation_pool_.get(),
      .log_sync_pool = log_sync_pool(),
      .retryable_requests = nullptr,
  };
  RETURN_NOT_OK(BootstrapTablet(data, &tablet, &log, &consensus_info));

  // TODO: Do we have a setSplittable(false) or something from the outside is
  // handling split in the TS?

  RETURN_NOT_OK_PREPEND(
      tablet_peer()->InitTabletPeer(
          tablet,
          master_->mem_tracker(),
          master_->messenger(),
          &master_->proxy_cache(),
          log,
          tablet->GetTableMetricsEntity(),
          tablet->GetTabletMetricsEntity(),
          raft_pool(),
          tablet_prepare_pool(),
          nullptr /* retryable_requests */,
          multi_raft_manager_.get()),
      "Failed to Init() TabletPeer");

  RETURN_NOT_OK_PREPEND(tablet_peer()->Start(consensus_info),
                        "Failed to Start() TabletPeer");

  tablet_peer()->RegisterMaintenanceOps(master_->maintenance_manager());

  if (!tablet->schema()->Equals(doc_read_context_->schema)) {
    return STATUS(Corruption, "Unexpected schema", tablet->schema()->ToString());
  }
  RETURN_NOT_OK(mem_manager_->Init());

  return Status::OK();
}

std::string SysCatalogTable::LogPrefix() const {
  return Substitute("T $0 P $1 [$2]: ",
                    tablet_peer()->tablet_id(),
                    tablet_peer()->permanent_uuid(),
                    table_name());
}

Status SysCatalogTable::WaitUntilRunning() {
  TRACE_EVENT0("master", "SysCatalogTable::WaitUntilRunning");
  int seconds_waited = 0;
  while (true) {
    Status status = tablet_peer()->WaitUntilConsensusRunning(MonoDelta::FromSeconds(1));
    seconds_waited++;
    if (status.ok()) {
      LOG_WITH_PREFIX(INFO) << "configured and running, proceeding with master startup.";
      break;
    }
    if (status.IsTimedOut()) {
      LOG_WITH_PREFIX(INFO) <<  "not online yet (have been trying for "
                               << seconds_waited << " seconds)";
      continue;
    }
    // if the status is not OK or TimedOut return it.
    return status;
  }
  return Status::OK();
}

Status SysCatalogTable::SyncWrite(SysCatalogWriter* writer) {
  if (PREDICT_FALSE(FLAGS_TEST_sys_catalog_write_rejection_percentage > 0) &&
      RandomUniformInt(1, 99) <= FLAGS_TEST_sys_catalog_write_rejection_percentage) {
    return STATUS(InternalError, "Injected random failure for testing.");
  }

  auto resp = std::make_shared<tserver::WriteResponsePB>();
  // If this is a PG write, them the pgsql write batch is not empty.
  //
  // If this is a QL write, then it is a normal sys_catalog write, so ignore writes that might
  // have filtered out all of the writes from the batch, as they were the same payload as the cow
  // objects that are backing them.
  if (writer->req().ql_write_batch().empty() && writer->req().pgsql_write_batch().empty()) {
    return Status::OK();
  }

  auto latch = std::make_shared<CountDownLatch>(1);
  auto query = std::make_unique<tablet::WriteQuery>(
      writer->leader_term(), CoarseTimePoint::max(), tablet_peer().get(),
      tablet_peer()->tablet(), resp.get());
  query->set_client_request(writer->req());
  query->set_callback(tablet::MakeLatchOperationCompletionCallback(latch, resp));

  tablet_peer()->WriteAsync(std::move(query));
  peer_write_count->Increment();

  {
    int num_iterations = 0;
    auto time = CoarseMonoClock::now();
    auto deadline = time + FLAGS_sys_catalog_write_timeout_ms * 1ms;
    static constexpr auto kWarningInterval = 5s;
    while (!latch->WaitUntil(std::min(deadline, time + kWarningInterval))) {
      ++num_iterations;
      const auto waited_so_far = num_iterations * kWarningInterval;
      LOG(WARNING) << "Waited for " << AsString(waited_so_far) << " for synchronous write to "
                   << "complete. Continuing to wait.";
      time = CoarseMonoClock::now();
      if (time >= deadline) {
        LOG(ERROR) << "Already waited for a total of " << ::yb::ToString(waited_so_far) << ". "
                   << "Returning a timeout from SyncWrite.";
        return STATUS_FORMAT(TimedOut, "SyncWrite timed out after $0", waited_so_far);
      }
    }
  }

  if (resp->has_error()) {
    return StatusFromPB(resp->error().status());
  }
  if (resp->per_row_errors_size() > 0) {
    for (const WriteResponsePB::PerRowErrorPB& error : resp->per_row_errors()) {
      LOG(WARNING) << "row " << error.row_index() << ": " << StatusFromPB(error.error()).ToString();
    }
    return STATUS(Corruption, "One or more rows failed to write");
  }
  return Status::OK();
}

// Schema for the unified SysCatalogTable:
//
// (entry_type, entry_id) -> metadata
//
// entry_type is a enum defined in sys_tables. It indicates
// whether an entry is a table or a tablet.
//
// entry_type is the first part of a compound key as to allow
// efficient scans of entries of only a single type (e.g., only
// scan all of the tables, or only scan all of the tablets).
//
// entry_id is either a table id or a tablet id. For tablet entries,
// the table id that the tablet is associated with is stored in the
// protobuf itself.
Schema SysCatalogTable::BuildTableSchema() {
  SchemaBuilder builder;
  CHECK_OK(builder.AddKeyColumn(kSysCatalogTableColType, INT8));
  CHECK_OK(builder.AddKeyColumn(kSysCatalogTableColId, BINARY));
  CHECK_OK(builder.AddColumn(kSysCatalogTableColMetadata, BINARY));
  return builder.Build();
}

// ==================================================================
// Other methods
// ==================================================================
void SysCatalogTable::InitLocalRaftPeerPB() {
  local_peer_pb_.set_permanent_uuid(master_->fs_manager()->uuid());
  ServerRegistrationPB reg;
  CHECK_OK(master_->GetRegistration(&reg, server::RpcOnly::kTrue));
  TakeRegistration(&reg, &local_peer_pb_);
}

Status SysCatalogTable::Visit(VisitorBase* visitor) {
  TRACE_EVENT0("master", "Visitor::VisitAll");

  auto tablet = tablet_peer()->shared_tablet();
  if (!tablet) {
    return STATUS(ShutdownInProgress, "SysConfig is shutting down.");
  }

  auto start = CoarseMonoClock::Now();

  uint64_t count = 0;
  RETURN_NOT_OK(EnumerateSysCatalog(tablet.get(), doc_read_context_->schema, visitor->entry_type(),
                                    [visitor, &count](const Slice& id, const Slice& data) {
    ++count;
    return visitor->Visit(id, data);
  }));

  auto duration = CoarseMonoClock::Now() - start;
  string id = Format("num_entries_with_type_$0_loaded", std::to_string(visitor->entry_type()));
  if (visitor_duration_metrics_.find(id) == visitor_duration_metrics_.end()) {
    string description = id + " metric for SysCatalogTable::Visit";
    std::unique_ptr<GaugePrototype<uint64>> counter_gauge =
        std::make_unique<OwningGaugePrototype<uint64>>(
            "server", id, description, yb::MetricUnit::kEntries, description,
            yb::MetricLevel::kInfo, yb::EXPOSE_AS_COUNTER);
    visitor_duration_metrics_[id] = metric_entity_->FindOrCreateGauge(
        std::move(counter_gauge), static_cast<uint64>(0) /* initial_value */);
  }
  visitor_duration_metrics_[id]->IncrementBy(count);

  id = Format("duration_ms_loading_entries_with_type_$0", std::to_string(visitor->entry_type()));
  if (visitor_duration_metrics_.find(id) == visitor_duration_metrics_.end()) {
    string description = id + " metric for SysCatalogTable::Visit";
    std::unique_ptr<GaugePrototype<uint64>> duration_gauge =
        std::make_unique<OwningGaugePrototype<uint64>>(
            "server", id, description, yb::MetricUnit::kMilliseconds, description,
            yb::MetricLevel::kInfo);
    visitor_duration_metrics_[id] = metric_entity_->FindOrCreateGauge(
        std::move(duration_gauge), static_cast<uint64>(0) /* initial_value */);
  }
  visitor_duration_metrics_[id]->IncrementBy(ToMilliseconds(duration));
  return Status::OK();
}

// TODO (Sanket): Change this function to use ExtractPgYbCatalogVersionRow.
Status SysCatalogTable::ReadYsqlCatalogVersion(TableId ysql_catalog_table_id,
                                               uint64_t* catalog_version,
                                               uint64_t* last_breaking_version) {
  TRACE_EVENT0("master", "ReadYsqlCatalogVersion");
  return ReadYsqlDBCatalogVersionImpl(
      ysql_catalog_table_id, catalog_version, last_breaking_version, nullptr);
}

Status SysCatalogTable::ReadYsqlAllDBCatalogVersions(
    TableId ysql_catalog_table_id,
    DbOidToCatalogVersionMap* versions) {
  TRACE_EVENT0("master", "ReadYsqlAllDBCatalogVersions");
  return ReadYsqlDBCatalogVersionImpl(ysql_catalog_table_id, nullptr, nullptr, versions);
}

Status SysCatalogTable::ReadYsqlDBCatalogVersionImpl(
    TableId ysql_catalog_table_id,
    uint64_t* catalog_version,
    uint64_t* last_breaking_version,
    DbOidToCatalogVersionMap* versions) {
  const tablet::TabletPtr tablet = tablet_peer()->shared_tablet();
  const auto* meta = tablet->metadata();
  const std::shared_ptr<tablet::TableInfo> ysql_catalog_table_info =
      VERIFY_RESULT(meta->GetTableInfo(ysql_catalog_table_id));
  const Schema& schema = ysql_catalog_table_info->schema();
  auto iter = VERIFY_RESULT(tablet->NewRowIterator(schema.CopyWithoutColumnIds(),
                                                   {} /* read_hybrid_time */,
                                                   ysql_catalog_table_id));
  QLTableRow source_row;
  ColumnId db_oid_id = VERIFY_RESULT(schema.ColumnIdByName(kDbOidColumnName));
  ColumnId version_col_id = VERIFY_RESULT(schema.ColumnIdByName(kCurrentVersionColumnName));
  ColumnId last_breaking_version_col_id =
      VERIFY_RESULT(schema.ColumnIdByName(kLastBreakingVersionColumnName));

  // If 'versions' is set we read all rows. If 'catalog_version/last_breaking_version' are set,
  // we only read the global catalog version.
  if (versions) {
    DCHECK(!catalog_version);
    DCHECK(!last_breaking_version);
    versions->clear();
  } else {
    // If no row is read below then it means version is 0 (not initialized yet).
    if (catalog_version) {
      *catalog_version = 0;
    }
    if (last_breaking_version) {
      *last_breaking_version = 0;
    }
  }

  while (VERIFY_RESULT(iter->HasNext())) {
    RETURN_NOT_OK(iter->NextRow(&source_row));
    auto db_oid_value = source_row.GetValue(db_oid_id);
    if (!db_oid_value) {
      return STATUS(Corruption, "Could not read syscatalog version");
    }
    auto version_col_value = source_row.GetValue(version_col_id);
    if (!version_col_value) {
      return STATUS(Corruption, "Could not read syscatalog version");
    }
    auto last_breaking_version_col_value = source_row.GetValue(last_breaking_version_col_id);
    if (!last_breaking_version_col_value) {
      return STATUS(Corruption, "Could not read syscatalog version");
    }
    if (versions) {
      // When 'versions' is set we read all rows.
      auto insert_result = versions->insert(
        std::make_pair(db_oid_value->uint32_value(),
                       std::make_pair(version_col_value->int64_value(),
                                      last_breaking_version_col_value->int64_value())));
      // There should not be any duplicate db_oid because it is a primary key.
      DCHECK(insert_result.second);
    } else {
      // Otherwise, we only read the global catalog version.
      if (catalog_version) {
        *catalog_version = version_col_value->int64_value();
      }
      if (last_breaking_version) {
        *last_breaking_version = last_breaking_version_col_value->int64_value();
      }
      // The table pg_yb_catalog_version has db_oid as primary key in ASC order and we use the
      // row for template1 to store global catalog version. The db_oid of template1 is 1, which
      // is the smallest db_oid. Therefore we only need to read the first row to retrieve the
      // global catalog version.
      return Status::OK();
    }
  }

  return Status::OK();
}

Result<shared_ptr<TablespaceIdToReplicationInfoMap>> SysCatalogTable::ReadPgTablespaceInfo() {
  TRACE_EVENT0("master", "ReadPgTablespaceInfo");

  const tablet::TabletPtr tablet = tablet_peer()->shared_tablet();

  const auto& pg_tablespace_info =
      VERIFY_RESULT(tablet->metadata()->GetTableInfo(kPgTablespaceTableId));
  const Schema& schema = pg_tablespace_info->schema();
  auto iter = VERIFY_RESULT(tablet->NewRowIterator(schema.CopyWithoutColumnIds(),
                                                   {} /* read_hybrid_time */,
                                                   kPgTablespaceTableId));
  QLTableRow source_row;
  ColumnId oid_col_id = VERIFY_RESULT(schema.ColumnIdByName("oid"));
  ColumnId options_id =
      VERIFY_RESULT(schema.ColumnIdByName("spcoptions"));

  // Loop through the pg_tablespace catalog table. Each row in this table represents
  // a tablespace. Populate 'tablespace_map' with the tablespace id and corresponding
  // placement info for each tablespace encountered in this catalog table.
  auto tablespace_map = std::make_shared<TablespaceIdToReplicationInfoMap>();
  while (VERIFY_RESULT(iter->HasNext())) {
    RETURN_NOT_OK(iter->NextRow(&source_row));
    // Fetch the oid.
    auto oid = source_row.GetValue(oid_col_id);
    if (!oid) {
      return STATUS(Corruption, "Could not read oid column from pg_tablespace");
    }

    // Get the tablespace id.
    const TablespaceId tablespace_id = GetPgsqlTablespaceId(oid->uint32_value());

    // Fetch the options specified for the tablespace.
    const auto& options = source_row.GetValue(options_id);
    if (!options) {
      return STATUS(Corruption, "Could not read spcoptions column from pg_tablespace");
    }

    VLOG(2) << "Tablespace " << tablespace_id << " -> " << options.value().DebugString();

    // If no spcoptions found, then this tablespace has no placement info
    // associated with it. Tables associated with this tablespace will not
    // have any custom placement policy.
    if (options->binary_value().empty()) {
      // Storing boost::none lets the client know that the tables associated with
      // this tablespace will not have any custom placement policy for them.
      const auto& ret = tablespace_map->emplace(tablespace_id, boost::none);
      // This map should not have already had an element associated with this
      // tablespace.
      DCHECK(ret.second);
      continue;
    }

    // Parse the reloptions array associated with this tablespace and construct
    // the ReplicationInfoPB. The ql_value is just the raw value read from the pg_tablespace
    // catalog table. This was stored in postgres as a text array, but processed by DocDB as
    // a binary value. So first process this binary value and convert it to text array of options.
    auto placement_options = VERIFY_RESULT(docdb::ExtractTextArrayFromQLBinaryValue(
          options.value()));

    // Fetch the status and print the tablespace option along with the status.
    ReplicationInfoPB replication_info;
    PlacementInfoConverter::Placement placement =
      VERIFY_RESULT(PlacementInfoConverter::FromQLValue(placement_options));

    PlacementInfoPB* live_replicas = replication_info.mutable_live_replicas();
    for (const auto& block : placement.placement_infos) {
      auto pb = live_replicas->add_placement_blocks();
      pb->mutable_cloud_info()->set_placement_cloud(block.cloud);
      pb->mutable_cloud_info()->set_placement_region(block.region);
      pb->mutable_cloud_info()->set_placement_zone(block.zone);
      pb->set_min_num_replicas(block.min_num_replicas);

      if (block.leader_preference < 0) {
        return STATUS(InvalidArgument, "leader_preference cannot be negative");
      } else if (static_cast<size_t>(block.leader_preference) > placement.placement_infos.size()) {
        return STATUS(
            InvalidArgument,
            "Priority value cannot be more than the number of zones in the preferred list since "
            "each priority should be associated with at least one zone from the list");
      } else if (block.leader_preference > 0) {
        // Contiguity has already been validated at YSQL layer
        while (replication_info.multi_affinitized_leaders_size() < block.leader_preference) {
          replication_info.add_multi_affinitized_leaders();
        }

        auto zone_set =
            replication_info.mutable_multi_affinitized_leaders(block.leader_preference - 1);
        auto ci = zone_set->add_zones();
        ci->set_placement_cloud(block.cloud);
        ci->set_placement_region(block.region);
        ci->set_placement_zone(block.zone);
      }
    }
    live_replicas->set_num_replicas(placement.num_replicas);

    const auto& ret = tablespace_map->emplace(tablespace_id, replication_info);
    // This map should not have already had an element associated with this
    // tablespace.
    DCHECK(ret.second);
  }

  return tablespace_map;
}

Status SysCatalogTable::ReadTablespaceInfoFromPgYbTablegroup(
    const uint32_t database_oid,
    TableToTablespaceIdMap *table_tablespace_map) {
  TRACE_EVENT0("master", "ReadTablespaceInfoFromPgYbTablegroup");

  if (!table_tablespace_map)
    return STATUS(InternalError, "tablegroup_tablespace_map not initialized");

  const tablet::TabletPtr tablet = tablet_peer()->shared_tablet();

  const auto &pg_yb_tablegroup_id = GetPgsqlTableId(database_oid, kPgYbTablegroupTableOid);
  const auto& pg_tablegroup_info =
    VERIFY_RESULT(tablet->metadata()->GetTableInfo(pg_yb_tablegroup_id));

  Schema projection;
  RETURN_NOT_OK(pg_tablegroup_info->schema().CreateProjectionByNames({"oid", "grptablespace"},
                &projection,
                pg_tablegroup_info->schema().num_key_columns()));
  auto iter = VERIFY_RESULT(tablet->NewRowIterator(projection.CopyWithoutColumnIds(),
                                                   {} /* read_hybrid_time */,
                                                   pg_yb_tablegroup_id));
  const auto oid_col_id = VERIFY_RESULT(projection.ColumnIdByName("oid")).rep();
  const auto tablespace_col_id = VERIFY_RESULT(projection.ColumnIdByName("grptablespace")).rep();

  QLTableRow source_row;

  // Loop through the pg_yb_tablegroup catalog table. Each row in this table represents
  // a tablegroup. Populate 'table_tablespace_map' with the tablegroup id and corresponding
  // tablespace id for each tablegroup

  while (VERIFY_RESULT(iter->HasNext())) {
    RETURN_NOT_OK(iter->NextRow(&source_row));
    // Fetch the tablegroup oid.
    const auto tablegroup_oid_col = source_row.GetValue(oid_col_id);
    if (!tablegroup_oid_col) {
      return STATUS(Corruption, "Could not read oid column from pg_yb_tablegroup");
    }
    const uint32_t tablegroup_oid = tablegroup_oid_col->uint32_value();

    // Fetch the tablespace oid.
    const auto& tablespace_oid_col = source_row.GetValue(tablespace_col_id);
    if (!tablespace_oid_col) {
      return STATUS(Corruption, "Could not read grptablespace column from pg_yb_tablegroup");
    }
    const uint32_t tablespace_oid = tablespace_oid_col->uint32_value();

    const TablegroupId tablegroup_id = GetPgsqlTablegroupId(database_oid, tablegroup_oid);
    const TableId parent_table_id = GetTablegroupParentTableId(tablegroup_id);
    boost::optional<TablespaceId> tablespace_id = boost::none;

    // If no valid tablespace found, then this tablegroup has no placement info
    // associated with it. Tables associated with this tablegroup will not
    // have any custom placement policy.
    if (tablespace_oid != kInvalidOid) {
      tablespace_id = GetPgsqlTablespaceId(tablespace_oid);
    }
    VLOG(2) << "Tablegroup oid: " << tablegroup_oid << " Tablespace oid: " << tablespace_oid;

    const auto& ret = table_tablespace_map->emplace(parent_table_id, tablespace_id);
    // This map should not have already had an element associated with this
    // tablegroup.
    DCHECK(ret.second);
  }
  return Status::OK();
}

Status SysCatalogTable::ReadPgClassInfo(
    const uint32_t database_oid,
    const bool is_colocated_database,
    TableToTablespaceIdMap* table_to_tablespace_map) {

  TRACE_EVENT0("master", "ReadPgClass");

  if (!table_to_tablespace_map) {
    return STATUS(InternalError, "table_to_tablespace_map not initialized");
  }

  const tablet::TabletPtr tablet = tablet_peer()->shared_tablet();

  const auto& pg_table_id = GetPgsqlTableId(database_oid, kPgClassTableOid);
  const auto& table_info = VERIFY_RESULT(
      tablet->metadata()->GetTableInfo(pg_table_id));
  const Schema& schema = table_info->schema();

  Schema projection;
  std::vector<GStringPiece> col_names = {"oid", "relname", "reltablespace", "relkind"};

  if (is_colocated_database) {
    VLOG(5) << "Scanning pg_class for colocated database oid " << database_oid;
    col_names.emplace_back("reloptions");
  }

  RETURN_NOT_OK(schema.CreateProjectionByNames(col_names,
                                               &projection,
                                               schema.num_key_columns()));
  const auto oid_col_id = VERIFY_RESULT(projection.ColumnIdByName("oid")).rep();
  const auto relname_col_id = VERIFY_RESULT(projection.ColumnIdByName("relname")).rep();
  const auto relkind_col_id = VERIFY_RESULT(projection.ColumnIdByName("relkind")).rep();
  const auto tablespace_col_id = VERIFY_RESULT(projection.ColumnIdByName("reltablespace")).rep();

  auto iter = VERIFY_RESULT(tablet->NewRowIterator(
    projection.CopyWithoutColumnIds(), {} /* read_hybrid_time */, pg_table_id));
  {
    auto doc_iter = down_cast<docdb::DocRowwiseIterator*>(iter.get());
    PgsqlConditionPB cond;
    cond.add_operands()->set_column_id(oid_col_id);
    cond.set_op(QL_OP_GREATER_THAN_EQUAL);
    // All rows in pg_class table with oid less than kPgFirstNormalObjectId correspond to system
    // catalog tables. They can be skipped, as tablespace information is relevant only for user
    // created tables.
    cond.add_operands()->mutable_value()->set_uint32_value(kPgFirstNormalObjectId);
    const std::vector<docdb::KeyEntryValue> empty_key_components;
    docdb::DocPgsqlScanSpec spec(
        projection, rocksdb::kDefaultQueryId, empty_key_components, empty_key_components,
        &cond, boost::none /* hash_code */, boost::none /* max_hash_code */, nullptr /* where */);
    RETURN_NOT_OK(doc_iter->Init(spec));
  }

  QLTableRow row;
  // pg_class table contains a row for every database object (tables/indexes/
  // composite types etc). Each such row contains a lot of information about the
  // database object itself. But here, we are trying to fetch table->tablespace
  // information. We iterate through every row in the catalog table and try to build
  // the table/index->placement info.
  while (VERIFY_RESULT(iter->HasNext())) {
    RETURN_NOT_OK(iter->NextRow(&row));

    // Process the oid of this table/index.
    const auto& oid_col = row.GetValue(oid_col_id);
    if (!oid_col) {
      return STATUS(Corruption, "Could not read oid column from pg_class");
    }
    const uint32_t oid = oid_col->uint32_value();

    const auto& relname_col = row.GetValue(relname_col_id);
    const std::string table_name = relname_col->string_value();

    // Skip rows that pertain to relation types that do not have use for tablespaces.
    const auto& relkind_col = row.GetValue(relkind_col_id);
    if (!relkind_col) {
      return STATUS(Corruption, "Could not read relkind column from pg_class for oid " +
          std::to_string(oid));
    }

    const char relkind = relkind_col->int8_value();
    // From PostgreSQL docs: r = ordinary table, i = index, S = sequence, t = TOAST table,
    // v = view, m = materialized view, c = composite type, f = foreign table,
    // p = partitioned table, I = partitioned index
    if (relkind != 'r' && relkind != 'i' && relkind != 'p' && relkind != 'I') {
      // This database object is not a table/index/partitioned table/partitioned index.
      // Skip this.
      continue;
    }

    bool is_colocated_table = false;
    if (is_colocated_database) {
      // A table in a colocated database is colocated unless it opted out
      // of colocation.
      is_colocated_table = true;
      const auto reloptions_col_id = VERIFY_RESULT(projection.ColumnIdByName("reloptions")).rep();
      const auto& reloptions_col = row.GetValue(reloptions_col_id);
      if (!reloptions_col) {
        return STATUS(Corruption, "Could not read reloptions column from pg_class for oid " +
            std::to_string(oid));
      }
      if (!reloptions_col->binary_value().empty()) {
        auto reloptions = VERIFY_RESULT(docdb::ExtractTextArrayFromQLBinaryValue(
            reloptions_col.value()));
        for (const auto& reloption : reloptions) {
          if (reloption.compare("colocated=false") == 0) {
            is_colocated_table = false;
            break;
          }
        }
      }
    }

    if (is_colocated_table) {
      // This is a colocated table. This cannot have a tablespace associated with it.
      VLOG(5) << "Table { oid: " << oid << ", name: " << table_name << " }"
              << " skipped as it is colocated";
      continue;
    }

    // Process the tablespace oid for this table/index.
    const auto& tablespace_oid_col = row.GetValue(tablespace_col_id);
    if (!tablespace_oid_col) {
      return STATUS(Corruption, "Could not read tablespace column from pg_class");
    }

    const uint32 tablespace_oid = tablespace_oid_col->uint32_value();
    VLOG(1) << "Table { oid: " << oid << ", name: " << table_name << " }"
            << " has tablespace oid " << tablespace_oid;

    boost::optional<TablespaceId> tablespace_id = boost::none;
    // If the tablespace oid is kInvalidOid then it means this table was created
    // without a custom tablespace and its properties just default to cluster level
    // policies.
    if (tablespace_oid != kInvalidOid) {
      tablespace_id = GetPgsqlTablespaceId(tablespace_oid);
    }
    const auto& ret = table_to_tablespace_map->emplace(
        GetPgsqlTableId(database_oid, oid), tablespace_id);
    // The map should not have a duplicate entry with the same oid.
    DCHECK(ret.second);
  }
  return Status::OK();
}

Result<uint32_t> SysCatalogTable::ReadPgClassRelnamespace(const uint32_t database_oid,
                                                          const uint32_t table_oid) {
  TRACE_EVENT0("master", "ReadPgClassRelnamespace");

  const tablet::TabletPtr tablet = tablet_peer()->shared_tablet();

  const auto& pg_table_id = GetPgsqlTableId(database_oid, kPgClassTableOid);
  const auto& table_info = VERIFY_RESULT(tablet->metadata()->GetTableInfo(pg_table_id));
  const Schema& schema = table_info->schema();

  Schema projection;
  RETURN_NOT_OK(schema.CreateProjectionByNames({"oid", "relnamespace"}, &projection,
                schema.num_key_columns()));
  const auto oid_col_id = VERIFY_RESULT(projection.ColumnIdByName("oid")).rep();
  const auto relnamespace_col_id = VERIFY_RESULT(projection.ColumnIdByName("relnamespace")).rep();
  auto iter = VERIFY_RESULT(tablet->NewRowIterator(
      projection.CopyWithoutColumnIds(), {} /* read_hybrid_time */, pg_table_id));
  {
    auto doc_iter = down_cast<docdb::DocRowwiseIterator*>(iter.get());
    PgsqlConditionPB cond;
    cond.add_operands()->set_column_id(oid_col_id);
    cond.set_op(QL_OP_EQUAL);
    cond.add_operands()->mutable_value()->set_uint32_value(table_oid);
    const std::vector<docdb::KeyEntryValue> empty_key_components;
    docdb::DocPgsqlScanSpec spec(
        projection, rocksdb::kDefaultQueryId, empty_key_components, empty_key_components,
        &cond, boost::none /* hash_code */, boost::none /* max_hash_code */, nullptr /* where */);
    RETURN_NOT_OK(doc_iter->Init(spec));
  }

  // pg_class table contains a row for every database object (tables/indexes/
  // composite types etc). Each such row contains a lot of information about the
  // database object itself. But here, we are trying to fetch table->relnamespace
  // information only.
  uint32 oid = kInvalidOid;
  if (VERIFY_RESULT(iter->HasNext())) {
    QLTableRow row;
    RETURN_NOT_OK(iter->NextRow(&row));

    // Process the relnamespace oid for this table/index.
    const auto& relnamespace_oid_col = row.GetValue(relnamespace_col_id);
    if (!relnamespace_oid_col) {
      return STATUS(Corruption, "Could not read relnamespace column from pg_class");
    }

    oid = relnamespace_oid_col->uint32_value();
    VLOG(1) << "Table oid: " << table_oid << " relnamespace oid: " << oid;
  }

  if (oid == kInvalidOid) {
    // This error is thrown in the case that the table is deleted in YSQL but not docdb.
    // Currently, this is checked for in the backup flow, see gh #13361.
    return STATUS(NotFound, "Not found or invalid relnamespace oid for table oid " +
        std::to_string(table_oid));
  }

  return oid;
}

Result<string> SysCatalogTable::ReadPgNamespaceNspname(const uint32_t database_oid,
                                                       const uint32_t relnamespace_oid) {
  TRACE_EVENT0("master", "ReadPgNamespaceNspname");

  const tablet::TabletPtr tablet = tablet_peer()->shared_tablet();

  const auto& pg_table_id = GetPgsqlTableId(database_oid, kPgNamespaceTableOid);
  const auto& table_info = VERIFY_RESULT(tablet->metadata()->GetTableInfo(pg_table_id));
  const Schema& schema = table_info->schema();

  Schema projection;
  RETURN_NOT_OK(schema.CreateProjectionByNames({"oid", "nspname"}, &projection,
                schema.num_key_columns()));
  const auto oid_col_id = VERIFY_RESULT(projection.ColumnIdByName("oid")).rep();
  const auto nspname_col_id = VERIFY_RESULT(projection.ColumnIdByName("nspname")).rep();
  auto iter = VERIFY_RESULT(tablet->NewRowIterator(
      projection.CopyWithoutColumnIds(), {} /* read_hybrid_time */, pg_table_id));
  {
    auto doc_iter = down_cast<docdb::DocRowwiseIterator*>(iter.get());
    PgsqlConditionPB cond;
    cond.add_operands()->set_column_id(oid_col_id);
    cond.set_op(QL_OP_EQUAL);
    cond.add_operands()->mutable_value()->set_uint32_value(relnamespace_oid);
    const std::vector<docdb::KeyEntryValue> empty_key_components;
    docdb::DocPgsqlScanSpec spec(
        projection, rocksdb::kDefaultQueryId, empty_key_components, empty_key_components,
        &cond, boost::none /* hash_code */, boost::none /* max_hash_code */, nullptr /* where */);
    RETURN_NOT_OK(doc_iter->Init(spec));
  }

  string name;
  if (VERIFY_RESULT(iter->HasNext())) {
    QLTableRow row;
    RETURN_NOT_OK(iter->NextRow(&row));

    // Process the relnamespace oid for this table/index.
    const auto& nspname_col = row.GetValue(nspname_col_id);
    if (!nspname_col) {
      return STATUS(Corruption, "Could not read nspname column from pg_namespace");
    }

    name = nspname_col->string_value();
    VLOG(1) << "relnamespace oid: " << relnamespace_oid << " nspname: " << name;
  }

  if (name.empty()) {
    return STATUS(Corruption, "Not found or empty nspname for relnamespace oid " +
        std::to_string(relnamespace_oid));
  }

  return name;
}

Result<std::unordered_map<string, uint32_t>> SysCatalogTable::ReadPgAttributeInfo(
    const uint32_t database_oid, const uint32_t table_oid) {
  TRACE_EVENT0("master", "ReadPgAttributeInfo");

  const tablet::TabletPtr tablet = tablet_peer()->shared_tablet();

  const auto& pg_table_id = GetPgsqlTableId(database_oid, kPgAttributeTableOid);
  const auto& table_info = VERIFY_RESULT(tablet->metadata()->GetTableInfo(pg_table_id));
  const Schema& schema = table_info->schema();

  Schema projection;
  RETURN_NOT_OK(schema.CreateProjectionByNames(
      {"attrelid", "attnum", "attname", "atttypid"}, &projection, schema.num_key_columns()));
  const auto attrelid_col_id = VERIFY_RESULT(projection.ColumnIdByName("attrelid")).rep();
  const auto attnum_col_id = VERIFY_RESULT(projection.ColumnIdByName("attnum")).rep();
  const auto attname_col_id = VERIFY_RESULT(projection.ColumnIdByName("attname")).rep();
  const auto atttypid_col_id = VERIFY_RESULT(projection.ColumnIdByName("atttypid")).rep();

  auto iter = VERIFY_RESULT(tablet->NewRowIterator(
      projection.CopyWithoutColumnIds(), {} /* read_hybrid_time */, pg_table_id));
  {
    auto doc_iter = down_cast<docdb::DocRowwiseIterator*>(iter.get());
    PgsqlConditionPB cond;
    cond.add_operands()->set_column_id(attrelid_col_id);
    cond.set_op(QL_OP_EQUAL);
    cond.add_operands()->mutable_value()->set_uint32_value(table_oid);
    const std::vector<docdb::KeyEntryValue> empty_key_components;
    docdb::DocPgsqlScanSpec spec(
        projection, rocksdb::kDefaultQueryId, empty_key_components, empty_key_components, &cond,
        boost::none /* hash_code */, boost::none /* max_hash_code */, nullptr /* where */);
    RETURN_NOT_OK(doc_iter->Init(spec));
  }

  std::unordered_map<string, uint32_t> type_oid_map;
  while (VERIFY_RESULT(iter->HasNext())) {
    QLTableRow row;
    RETURN_NOT_OK(iter->NextRow(&row));

    const auto& attnum_col = row.GetValue(attnum_col_id);

    if (!attnum_col) {
      return STATUS_FORMAT(
          Corruption, "Could not read attnum column from pg_attribute for attrelid $0:", table_oid);
    }

    if (attnum_col->int16_value() < 0) {
      // Ignore system columns.
      VLOG(1) << "Ignoring system column (attnum = " << attnum_col->int16_value()
              << ") for attrelid $0:" << table_oid;
      continue;
    }

    const auto& attname_col = row.GetValue(attname_col_id);
    const auto& atttypid_col = row.GetValue(atttypid_col_id);

    if (!attname_col || !atttypid_col) {
      std::string corrupted_col = !attname_col ? "attname" : "atttypid";
      return STATUS_FORMAT(
          Corruption,
          "Could not read $0 column from pg_attribute for attrelid: $1 database_oid: $2",
          corrupted_col, table_oid, database_oid);
    }
    string attname = attname_col->string_value();
    uint32_t atttypid = atttypid_col->uint32_value();

    if (atttypid == 0) {
      // Ignore dropped columns.
      VLOG(1) << "Ignoring dropped column " << attname << " (atttypid = 0)"
              << " for attrelid $0:" << table_oid;
      continue;
    }

    type_oid_map[attname] = atttypid;
    VLOG(1) << "attrelid: " << table_oid << " attname: " << attname << " atttypid: " << atttypid;
  }
  return type_oid_map;
}

Result<std::unordered_map<uint32_t, string>> SysCatalogTable::ReadPgEnum(
    const uint32_t database_oid) {
  TRACE_EVENT0("master", "ReadPgEnum");

  const tablet::TabletPtr tablet = tablet_peer()->shared_tablet();
  const auto& pg_table_id = GetPgsqlTableId(database_oid, kPgEnumTableOid);
  const auto& table_info = VERIFY_RESULT(tablet->metadata()->GetTableInfo(pg_table_id));
  const Schema& schema = table_info->schema();

  Schema projection;
  RETURN_NOT_OK(schema.CreateProjectionByNames(
      {"oid", "enumlabel"}, &projection, schema.num_key_columns()));
  const auto oid_col_id = VERIFY_RESULT(projection.ColumnIdByName("oid")).rep();
  const auto enumlabel_col_id = VERIFY_RESULT(projection.ColumnIdByName("enumlabel")).rep();

  auto iter = VERIFY_RESULT(tablet->NewRowIterator(
      projection.CopyWithoutColumnIds(), {} /* read_hybrid_time */, pg_table_id));
  {
    auto doc_iter = down_cast<docdb::DocRowwiseIterator*>(iter.get());
    const std::vector<docdb::KeyEntryValue> empty_key_components;
    docdb::DocPgsqlScanSpec spec(
        projection, rocksdb::kDefaultQueryId, empty_key_components, empty_key_components,
        nullptr /* cond */, boost::none /* hash_code */, boost::none /* max_hash_code */,
        nullptr /* where */);
    RETURN_NOT_OK(doc_iter->Init(spec));
  }

  std::unordered_map<uint32_t, string> enumlabel_map;
  while (VERIFY_RESULT(iter->HasNext())) {
    QLTableRow row;
    RETURN_NOT_OK(iter->NextRow(&row));

    const auto& oid_col = row.GetValue(oid_col_id);
    const auto& enumlabel_col = row.GetValue(enumlabel_col_id);

    if (!oid_col || !enumlabel_col) {
      std::string corrupted_col = !oid_col ? "oid" : "enumlabel";
      return STATUS_FORMAT(
          Corruption, "Could not read $0 column from pg_enum for database id $1:", corrupted_col,
          database_oid);
    }
    uint32_t oid = oid_col->uint32_value();
    string enumlabel = enumlabel_col->string_value();

    enumlabel_map[oid] = enumlabel;
    VLOG(1) << "Database oid: " << database_oid << " enum oid: " << oid
            << " enumlabel: " << enumlabel;
  }
  return enumlabel_map;
}

Result<std::unordered_map<uint32_t, PgTypeInfo>> SysCatalogTable::ReadPgTypeInfo(
    const uint32_t database_oid, vector<uint32_t>* type_oids) {
  TRACE_EVENT0("master", "ReadPgTypeInfo");
  const tablet::TabletPtr tablet = tablet_peer()->shared_tablet();

  const auto& pg_table_id = GetPgsqlTableId(database_oid, kPgTypeTableOid);
  const auto& table_info = VERIFY_RESULT(tablet->metadata()->GetTableInfo(pg_table_id));
  const Schema& schema = table_info->schema();

  Schema projection;
  RETURN_NOT_OK(schema.CreateProjectionByNames(
      {"oid", "typtype", "typbasetype"}, &projection, schema.num_key_columns()));
  const auto oid_col_id = VERIFY_RESULT(projection.ColumnIdByName("oid")).rep();
  const auto typtype_col_id = VERIFY_RESULT(projection.ColumnIdByName("typtype")).rep();
  const auto typbasetype_col_id = VERIFY_RESULT(projection.ColumnIdByName("typbasetype")).rep();

  auto iter = VERIFY_RESULT(tablet->NewRowIterator(
      projection.CopyWithoutColumnIds(), {} /* read_hybrid_time */, pg_table_id));
  {
    auto doc_iter = down_cast<docdb::DocRowwiseIterator*>(iter.get());
    PgsqlConditionPB cond;
    cond.add_operands()->set_column_id(oid_col_id);
    cond.set_op(QL_OP_IN);
    std::sort(type_oids->begin(), type_oids->end());
    auto seq_value = cond.add_operands()->mutable_value()->mutable_list_value();
    for (auto const type_oid : *type_oids) {
      seq_value->add_elems()->set_uint32_value(type_oid);
    }

    const std::vector<docdb::KeyEntryValue> empty_key_components;
    docdb::DocPgsqlScanSpec spec(
        projection, rocksdb::kDefaultQueryId, empty_key_components, empty_key_components, &cond,
        boost::none /* hash_code */, boost::none /* max_hash_code */, nullptr /* where */);
    RETURN_NOT_OK(doc_iter->Init(spec));
  }

  std::unordered_map<uint32_t, PgTypeInfo> type_oid_info_map;
  while (VERIFY_RESULT(iter->HasNext())) {
    QLTableRow row;
    RETURN_NOT_OK(iter->NextRow(&row));

    const auto& oid_col = row.GetValue(oid_col_id);
    const auto& typtype_col = row.GetValue(typtype_col_id);
    const auto& typbasetype_col = row.GetValue(typbasetype_col_id);

    if (!oid_col || !typtype_col || !typbasetype_col) {
      std::string corrupted_col;
      if (!oid_col) {
        corrupted_col = "oid";
      } else if (!typtype_col) {
        corrupted_col = "typtype";
      } else {
        corrupted_col = "typbasetype";
      }
      return STATUS_FORMAT(
          Corruption,
          "Could not read $0 column from pg_attribute for databaseoid: $1:", corrupted_col,
          database_oid);
    }

    const uint32_t oid = oid_col->uint32_value();
    const char typtype = typtype_col->int8_value();
    const uint32_t typbasetype = typbasetype_col->uint32_value();

    type_oid_info_map.insert({oid, PgTypeInfo(typtype, typbasetype)});

    VLOG(1) << "oid: " << oid << " typtype: " << typtype << " typbasetype: " << typbasetype;
  }
  return type_oid_info_map;
}

Status SysCatalogTable::CopyPgsqlTables(
    const vector<TableId>& source_table_ids, const vector<TableId>& target_table_ids,
    const int64_t leader_term) {
  TRACE_EVENT0("master", "CopyPgsqlTables");

  std::unique_ptr<SysCatalogWriter> writer = NewWriter(leader_term);

  RSTATUS_DCHECK_EQ(
      source_table_ids.size(), target_table_ids.size(), InvalidArgument,
      "size mismatch between source tables and target tables");

  int batch_count = 0, total_count = 0, total_bytes = 0;
  const tablet::TabletPtr tablet = tablet_peer()->shared_tablet();
  const auto* meta = tablet->metadata();
  for (size_t i = 0; i < source_table_ids.size(); ++i) {
    auto& source_table_id = source_table_ids[i];
    auto& target_table_id = target_table_ids[i];

    const std::shared_ptr<tablet::TableInfo> source_table_info =
        VERIFY_RESULT(meta->GetTableInfo(source_table_id));
    const std::shared_ptr<tablet::TableInfo> target_table_info =
        VERIFY_RESULT(meta->GetTableInfo(target_table_id));
    const Schema source_projection = source_table_info->schema().CopyWithoutColumnIds();
    std::unique_ptr<docdb::YQLRowwiseIteratorIf> iter = VERIFY_RESULT(
        tablet->NewRowIterator(source_projection, {}, source_table_id));
    QLTableRow source_row;

    while (VERIFY_RESULT(iter->HasNext())) {
      RETURN_NOT_OK(iter->NextRow(&source_row));

      RETURN_NOT_OK(writer->InsertPgsqlTableRow(
          source_table_info->schema(), source_row, target_table_id, target_table_info->schema(),
          target_table_info->schema_version, true /* is_upsert */));

      ++total_count;
      if (FLAGS_copy_tables_batch_bytes > 0 && 0 == (total_count % 128)) {
          // Break up the write into batches of roughly the same serialized size
          // in order to avoid uncontrolled large network writes.
          // ByteSizeLong is an expensive calculation so do not perform it each time

        size_t batch_bytes = writer->req().ByteSizeLong();
        if (batch_bytes > FLAGS_copy_tables_batch_bytes) {
          RETURN_NOT_OK(SyncWrite(writer.get()));

          total_bytes += batch_bytes;
          ++batch_count;
          LOG(INFO) << Format(
              "CopyPgsqlTables: Batch# $0 copied $1 rows with $2 bytes", batch_count,
              writer->req().pgsql_write_batch_size(), HumanizeBytes(batch_bytes));

          writer = NewWriter(leader_term);
        }
      }
    }
  }

  if (writer->req().pgsql_write_batch_size() > 0) {
    RETURN_NOT_OK(SyncWrite(writer.get()));
    size_t batch_bytes = writer->req().ByteSizeLong();
    total_bytes += batch_bytes;
    ++batch_count;
    LOG(INFO) << Format(
        "CopyPgsqlTables: Batch# $0 copied $1 rows with $2 bytes", batch_count,
        writer->req().pgsql_write_batch_size(), HumanizeBytes(batch_bytes));
  }

  LOG(INFO) << Format(
      "CopyPgsqlTables: Copied total $0 rows, total $1 bytes in $2 batches", total_count,
      HumanizeBytes(total_bytes), batch_count);
  return Status::OK();
}

Status SysCatalogTable::DeleteYsqlSystemTable(const string& table_id) {
  tablet_peer()->tablet_metadata()->RemoveTable(table_id);
  return Status::OK();
}

const Schema& SysCatalogTable::schema() {
  return doc_read_context_->schema;
}

const docdb::DocReadContext& SysCatalogTable::doc_read_context() {
  return *doc_read_context_;
}

Status SysCatalogTable::FetchDdlLog(google::protobuf::RepeatedPtrField<DdlLogEntryPB>* entries) {
  auto tablet = tablet_peer()->shared_tablet();
  if (!tablet) {
    return STATUS(ShutdownInProgress, "SysConfig is shutting down.");
  }

  return EnumerateSysCatalog(
      tablet.get(), doc_read_context_->schema, SysRowEntryType::DDL_LOG_ENTRY,
      [entries](const Slice& id, const Slice& data) -> Status {
    *entries->Add() = VERIFY_RESULT(pb_util::ParseFromSlice<DdlLogEntryPB>(data));
    return Status::OK();
  });
}

std::string SysCatalogTable::tablet_id() const {
  return tablet_peer()->tablet_id();
}

} // namespace master
} // namespace yb
