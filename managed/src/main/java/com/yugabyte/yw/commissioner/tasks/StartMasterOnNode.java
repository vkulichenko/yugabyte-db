/*
 * Copyright 2019 YugaByte, Inc. and Contributors
 *
 * Licensed under the Polyform Free Trial License 1.0.0 (the "License"); you
 * may not use this file except in compliance with the License. You
 * may obtain a copy of the License at
 *
 *     https://github.com/YugaByte/yugabyte-db/blob/master/licenses/POLYFORM-FREE-TRIAL-LICENSE-1.0.0.txt
 */

package com.yugabyte.yw.commissioner.tasks;

import static com.yugabyte.yw.common.Util.areMastersUnderReplicated;

import com.yugabyte.yw.commissioner.BaseTaskDependencies;
import com.yugabyte.yw.commissioner.UserTaskDetails.SubTaskGroupType;
import com.yugabyte.yw.commissioner.tasks.params.NodeTaskParams;
import com.yugabyte.yw.forms.VMImageUpgradeParams.VmUpgradeTaskType;
import com.yugabyte.yw.models.Universe;
import com.yugabyte.yw.models.helpers.NodeDetails;
import com.yugabyte.yw.models.helpers.NodeDetails.NodeState;
import java.util.Arrays;
import java.util.List;
import javax.inject.Inject;
import lombok.extern.slf4j.Slf4j;

// Allows the addition of the master server to a node. Spawns master the process and ensures
// the task waits for the right set of load balance primitives.
@Slf4j
public class StartMasterOnNode extends UniverseDefinitionTaskBase {

  @Inject
  protected StartMasterOnNode(BaseTaskDependencies baseTaskDependencies) {
    super(baseTaskDependencies);
  }

  @Override
  protected NodeTaskParams taskParams() {
    return (NodeTaskParams) taskParams;
  }

  @Override
  public void run() {
    log.info(
        "Started {} task for node {} in univ uuid={}",
        getName(),
        taskParams().nodeName,
        taskParams().universeUUID);
    NodeDetails currentNode = null;
    boolean hitException = false;
    try {
      checkUniverseVersion();

      // Update the DB to prevent other changes from happening.
      Universe universe = lockUniverseForUpdate(taskParams().expectedUniverseVersion);

      currentNode = universe.getNode(taskParams().nodeName);
      if (currentNode == null) {
        String msg = "No node " + taskParams().nodeName + " in universe " + universe.name;
        log.error(msg);
        throw new RuntimeException(msg);
      }

      taskParams().azUuid = currentNode.azUuid;
      taskParams().placementUuid = currentNode.placementUuid;
      if (!instanceExists(taskParams())) {
        String msg = "No instance exists for " + taskParams().nodeName;
        log.error(msg);
        throw new RuntimeException(msg);
      }

      if (currentNode.isMaster) {
        String msg = "Node " + taskParams().nodeName + " already has the Master process running.";
        log.error(msg);
        throw new RuntimeException(msg);
      }

      if (currentNode.state == NodeState.Stopped
          || currentNode.state == NodeState.Removed
          || currentNode.state == NodeState.Decommissioned) {
        String msg =
            "Node "
                + taskParams().nodeName
                + " is in removed or decommissioned state"
                + ", the Master process cannot be started. Use \"Start Node\" instead.";
        log.error(msg);
        throw new RuntimeException(msg);
      }

      if (!areMastersUnderReplicated(currentNode, universe)) {
        String msg =
            "Unable to start the Master process on node "
                + taskParams().nodeName
                + ", no more Masters allowed.";
        log.error(msg);
        throw new RuntimeException(msg);
      }

      log.info(
          "Bringing up master for under replicated universe {} ({})",
          universe.universeUUID,
          universe.name);

      preTaskActions();

      // Update node state to Starting Master.
      createSetNodeStateTask(currentNode, NodeState.Starting)
          .setSubTaskGroupType(SubTaskGroupType.StartingMasterProcess);

      List<NodeDetails> nodeAsList = Arrays.asList(currentNode);

      // Set gflags for master.
      createGFlagsOverrideTasks(
          nodeAsList,
          ServerType.MASTER,
          true /* isShell */,
          VmUpgradeTaskType.None,
          false /*ignoreUseCustomImageConfig*/);

      // Check that installed MASTER software version is consistent.
      createSoftwareInstallTasks(
          nodeAsList, ServerType.MASTER, null, SubTaskGroupType.InstallingSoftware);

      // Update master configuration on the node.
      createConfigureServerTasks(
              nodeAsList,
              params -> {
                params.isMasterInShellMode = true;
                params.updateMasterAddrsOnly = true;
                params.isMaster = true;
              })
          .setSubTaskGroupType(SubTaskGroupType.ConfigureUniverse);

      // Start a master process.
      createStartMasterTasks(nodeAsList)
          .setSubTaskGroupType(SubTaskGroupType.StartingMasterProcess);

      // Mark node as isMaster in YW DB.
      createUpdateNodeProcessTask(taskParams().nodeName, ServerType.MASTER, true)
          .setSubTaskGroupType(SubTaskGroupType.StartingMasterProcess);

      // Wait for the master to be responsive.
      createWaitForServersTasks(nodeAsList, ServerType.MASTER)
          .setSubTaskGroupType(SubTaskGroupType.ConfigureUniverse);

      // Add master to the quorum.
      createChangeConfigTask(currentNode, true /* isAdd */, SubTaskGroupType.ConfigureUniverse);

      // Update all server conf files with new master information.
      createMasterInfoUpdateTask(universe, currentNode);

      // Update the master addresses on the target universes whose source universe belongs to
      // this task.
      createXClusterConfigUpdateMasterAddressesTask();

      // Update node state to running.
      createSetNodeStateTask(currentNode, NodeDetails.NodeState.Live)
          .setSubTaskGroupType(SubTaskGroupType.StartingMasterProcess);

      // Update the swamper target file.
      createSwamperTargetUpdateTask(false /* removeFile */);

      // Mark universe update success to true.
      createMarkUniverseUpdateSuccessTasks()
          .setSubTaskGroupType(SubTaskGroupType.StartingMasterProcess);

      // Run all the tasks.
      getRunnableTask().runSubTasks();
    } catch (Throwable t) {
      log.error("Error executing task {} with error='{}'.", getName(), t.getMessage(), t);
      hitException = true;
      throw t;
    } finally {
      try {
        // Reset the state, on any failure, so that the actions can be retried.
        if (currentNode != null && hitException) {
          setNodeState(taskParams().nodeName, currentNode.state);
        }
      } finally {
        // Mark the update of the universe as done. This will allow future updates to
        // the universe.
        unlockUniverseForUpdate();
      }
    }
    log.info(
        "Finished {} task for node {} in univ uuid={}",
        getName(),
        taskParams().nodeName,
        taskParams().universeUUID);
  }
}
