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

#include "yb/integration-tests/external_mini_cluster.h"
#include "yb/integration-tests/mini_cluster.h"
#include "yb/integration-tests/ts_itest-base.h"
#include "yb/integration-tests/yb_mini_cluster_test_base.h"
#include "yb/master/mini_master.h"
#include "yb/master/master.h"
#include "yb/server/server_base.proxy.h"
#include "yb/tserver/mini_tablet_server.h"
#include "yb/tserver/tablet_server.h"
#include "yb/util/auto_flags.h"

DECLARE_bool(TEST_auto_flags_initialized);
DECLARE_bool(disable_auto_flags_management);

// Required for tests with AutoFlags management disabled
DISABLE_PROMOTE_ALL_AUTO_FLAGS_FOR_TEST;

using std::string;

namespace yb {
const string kDisableAutoFlagsManagementFlagName = "disable_auto_flags_management";
const string kTESTAutoFlagsInitializedFlagName = "TEST_auto_flags_initialized";
const string kTrue = "true";
const string kFalse = "false";
const MonoDelta kTimeout = 20s;
const int kNumMasterServers = 3;
const int kNumTServers = 3;

class AutoFlagsMiniCluster : public YBMiniClusterTestBase<MiniCluster> {
 protected:
  void SetUp() override {}
  void TestBody() override {}

 public:
  void RunSetUp() {
    YBMiniClusterTestBase::SetUp();
    MiniClusterOptions opts;
    opts.num_tablet_servers = kNumTServers;
    opts.num_masters = kNumMasterServers;
    cluster_.reset(new MiniCluster(opts));
    ASSERT_OK(cluster_->Start());
  }

  void ValidateConfig() {
    int count_flags = 0;
    auto leader_master = ASSERT_RESULT(cluster_->GetLeaderMiniMaster());
    const AutoFlagsConfigPB leader_config = leader_master->master()->GetAutoFlagConfig();
    for (const auto& per_process_flags : leader_config.promoted_flags()) {
      auto it = std::find(
          per_process_flags.flags().begin(), per_process_flags.flags().end(),
          kTESTAutoFlagsInitializedFlagName);
      ASSERT_NE(it, per_process_flags.flags().end());
      count_flags++;
    }

    if (FLAGS_disable_auto_flags_management) {
      ASSERT_FALSE(FLAGS_TEST_auto_flags_initialized);
      ASSERT_EQ(count_flags, 0);
    } else {
      ASSERT_TRUE(FLAGS_TEST_auto_flags_initialized);
      ASSERT_EQ(count_flags, leader_config.promoted_flags().size());
    }

    for (size_t i = 0; i < cluster_->num_masters(); i++) {
      auto master = cluster_->mini_master(i);
      const AutoFlagsConfigPB follower_config = master->master()->GetAutoFlagConfig();
      ASSERT_EQ(follower_config.DebugString(), leader_config.DebugString());
    }

    ASSERT_OK(cluster_->AddTabletServer());
    ASSERT_OK(cluster_->WaitForTabletServerCount(kNumTServers + 1));

    for (size_t i = 0; i < cluster_->num_tablet_servers(); i++) {
      auto tserver = cluster_->mini_tablet_server(i);
      const AutoFlagsConfigPB tserver_config = tserver->server()->TEST_GetAutoFlagConfig();
      ASSERT_EQ(tserver_config.DebugString(), leader_config.DebugString());
    }
  }
};

TEST(AutoFlagsMiniClusterTest, NewCluster) {
  AutoFlagsMiniCluster cluster;
  cluster.RunSetUp();

  cluster.ValidateConfig();
}

TEST(AutoFlagsDisabledMiniClusterTest, DisableAutoFlagManagement) {
  FLAGS_disable_auto_flags_management = true;

  AutoFlagsMiniCluster cluster;
  cluster.RunSetUp();

  cluster.ValidateConfig();
}

class AutoFlagsExternalMiniClusterTest : public tserver::TabletServerIntegrationTestBase {
 public:
  AutoFlagsExternalMiniClusterTest() {
    FLAGS_num_tablet_servers = kNumTServers;
    FLAGS_num_replicas = kNumTServers;
  }

  void UpdateMiniClusterOptions(ExternalMiniClusterOptions* options) override {
    opts_.num_masters = kNumMasterServers;
    opts_ = *options;
  }

  void CheckFlagOnNode(
      const string& flag_name, const string& expected_val, ExternalDaemon* daemon) {
    auto value = ASSERT_RESULT(daemon->GetFlag(flag_name));
    ASSERT_EQ(value, expected_val);
  }

  void CheckFlagOnAllNodes(string flag_name, string expected_val) {
    for (auto* daemon : cluster_->daemons()) {
      CheckFlagOnNode(flag_name, expected_val, daemon);
    }
  }

  uint32_t GetAutoFlagConfigVersion(ExternalDaemon* daemon) {
    server::GetAutoFlagsConfigVersionRequestPB req;
    server::GetAutoFlagsConfigVersionResponsePB resp;
    rpc::RpcController rpc;
    rpc.set_timeout(kTimeout);
    EXPECT_OK(cluster_->GetProxy<server::GenericServiceProxy>(daemon).GetAutoFlagsConfigVersion(
        req, &resp, &rpc));

    return resp.config_version();
  }

 protected:
  ExternalMiniClusterOptions opts_;
};

// Validate AutoFlags in new cluster and make sure it handles process restarts, and addition of
// new nodes.
TEST_F(AutoFlagsExternalMiniClusterTest, NewCluster) {
  BuildAndStart({} /* ts_flags */, {} /* master_flags */);

  CheckFlagOnAllNodes(kTESTAutoFlagsInitializedFlagName, kTrue);

  ExternalMaster* new_master = nullptr;
  cluster_->StartShellMaster(&new_master);

  OpIdPB op_id;
  ASSERT_OK(cluster_->GetLastOpIdForLeader(&op_id));
  ASSERT_OK(cluster_->ChangeConfig(new_master, consensus::ADD_SERVER));
  ASSERT_OK(cluster_->WaitForMastersToCommitUpTo(op_id.index()));

  CheckFlagOnNode(kTESTAutoFlagsInitializedFlagName, kTrue, new_master);

  ASSERT_OK(cluster_->AddTabletServer());
  ASSERT_OK(cluster_->WaitForTabletServerCount(opts_.num_tablet_servers + 1, kTimeout));

  CheckFlagOnAllNodes(kTESTAutoFlagsInitializedFlagName, kTrue);

  for (auto* master : cluster_->master_daemons()) {
    master->Shutdown();
    CHECK_OK(master->Restart());
    CheckFlagOnNode(kTESTAutoFlagsInitializedFlagName, kTrue, master);
    ASSERT_EQ(GetAutoFlagConfigVersion(master), 1);
  }

  for (auto* tserver : cluster_->tserver_daemons()) {
    tserver->Shutdown();
    CHECK_OK(tserver->Restart());
    CheckFlagOnNode(kTESTAutoFlagsInitializedFlagName, kTrue, tserver);
    ASSERT_EQ(GetAutoFlagConfigVersion(tserver), 1);
  }
}

// Create a Cluster with AutoFlags management turned off to simulate a cluster running old code.
// Restart the cluster with AutoFlags management enabled to simulate an upgrade. Make sure nodes
// added to this cluster works as expected.
TEST_F(AutoFlagsExternalMiniClusterTest, UpgradeCluster) {
  string disable_auto_flag_management = "--" + kDisableAutoFlagsManagementFlagName;
  BuildAndStart(
      {disable_auto_flag_management} /* ts_flags */,
      {disable_auto_flag_management} /* master_flags */);

  CheckFlagOnAllNodes(kDisableAutoFlagsManagementFlagName, kTrue);
  CheckFlagOnAllNodes(kTESTAutoFlagsInitializedFlagName, kFalse);

  // Remove the disable_auto_flag_management flag from cluster config
  auto it_master = std::find(
      cluster_->mutable_extra_master_flags()->begin(),
      cluster_->mutable_extra_master_flags()->end(), disable_auto_flag_management);
  cluster_->mutable_extra_master_flags()->erase(it_master);

  auto it_tserver = std::find(
      cluster_->mutable_extra_tserver_flags()->begin(),
      cluster_->mutable_extra_tserver_flags()->end(), disable_auto_flag_management);
  cluster_->mutable_extra_tserver_flags()->erase(it_tserver);

  ASSERT_OK(cluster_->AddTabletServer());
  ASSERT_OK(cluster_->WaitForTabletServerCount(opts_.num_tablet_servers + 1, kTimeout));

  // Add a new tserver
  auto* new_tserver = cluster_->tablet_server(cluster_->num_tablet_servers() - 1);
  CheckFlagOnNode(kDisableAutoFlagsManagementFlagName, kFalse, new_tserver);
  CheckFlagOnNode(kTESTAutoFlagsInitializedFlagName, kFalse, new_tserver);
  ASSERT_EQ(GetAutoFlagConfigVersion(new_tserver), 0);

  // Restart the new tserver
  new_tserver->Shutdown();
  ASSERT_OK(cluster_->WaitForTabletServerCount(opts_.num_tablet_servers, kTimeout));
  CHECK_OK(new_tserver->Restart());
  ASSERT_OK(cluster_->WaitForTabletServerCount(opts_.num_tablet_servers + 1, kTimeout));
  CheckFlagOnNode(kDisableAutoFlagsManagementFlagName, kFalse, new_tserver);
  CheckFlagOnNode(kTESTAutoFlagsInitializedFlagName, kFalse, new_tserver);
  ASSERT_EQ(GetAutoFlagConfigVersion(new_tserver), 0);

  // Add a new master
  ExternalMaster* new_master = nullptr;
  cluster_->StartShellMaster(&new_master);

  OpIdPB op_id;
  ASSERT_OK(cluster_->GetLastOpIdForLeader(&op_id));
  ASSERT_OK(cluster_->ChangeConfig(new_master, consensus::ADD_SERVER));
  ASSERT_OK(cluster_->WaitForMastersToCommitUpTo(op_id.index()));
  CheckFlagOnNode(kDisableAutoFlagsManagementFlagName, kFalse, new_master);
  CheckFlagOnNode(kTESTAutoFlagsInitializedFlagName, kFalse, new_master);
  ASSERT_EQ(GetAutoFlagConfigVersion(new_master), 0);

  // Restart the master
  new_master->Shutdown();
  CHECK_OK(new_master->Restart());
  CheckFlagOnNode(kDisableAutoFlagsManagementFlagName, kFalse, new_master);
  CheckFlagOnNode(kTESTAutoFlagsInitializedFlagName, kFalse, new_master);
  ASSERT_EQ(GetAutoFlagConfigVersion(new_master), 0);

  // Remove disable_auto_flag_management from each process config and restart
  for (auto* master : cluster_->master_daemons()) {
    master->mutable_flags()->clear();
    master->Shutdown();
    CHECK_OK(master->Restart());
    CheckFlagOnNode(kDisableAutoFlagsManagementFlagName, kFalse, master);
    const auto config_version = GetAutoFlagConfigVersion(master);
    if (master == new_master) {
      ASSERT_EQ(config_version, 0);
    } else {
      ASSERT_EQ(config_version, 1);
    }
  }

  for (auto* tserver : cluster_->tserver_daemons()) {
    tserver->mutable_flags()->clear();
    tserver->Shutdown();
    CHECK_OK(tserver->Restart());
    CheckFlagOnNode(kDisableAutoFlagsManagementFlagName, kFalse, tserver);
    const auto config_version = GetAutoFlagConfigVersion(tserver);
    if (tserver == new_tserver) {
      ASSERT_EQ(config_version, 0);
    } else {
      ASSERT_EQ(config_version, 1);
    }
  }

  CheckFlagOnAllNodes(kTESTAutoFlagsInitializedFlagName, kFalse);
}

}  // namespace yb
