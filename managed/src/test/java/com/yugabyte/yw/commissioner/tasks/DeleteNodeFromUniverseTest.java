// Copyright (c) YugaByte, Inc.

package com.yugabyte.yw.commissioner.tasks;

import static com.yugabyte.yw.common.ModelFactory.createUniverse;
import static com.yugabyte.yw.models.TaskInfo.State.Failure;
import static com.yugabyte.yw.models.TaskInfo.State.Success;
import static org.junit.Assert.assertEquals;
import static org.junit.Assert.assertNotNull;
import static org.junit.Assert.assertNull;
import static org.mockito.ArgumentMatchers.any;
import static org.mockito.Mockito.times;
import static org.mockito.Mockito.verify;
import static org.mockito.Mockito.when;

import com.google.common.collect.ImmutableList;
import com.yugabyte.yw.commissioner.tasks.params.NodeTaskParams;
import com.yugabyte.yw.common.ApiUtils;
import com.yugabyte.yw.common.ShellResponse;
import com.yugabyte.yw.forms.UniverseDefinitionTaskParams;
import com.yugabyte.yw.models.AvailabilityZone;
import com.yugabyte.yw.models.Region;
import com.yugabyte.yw.models.TaskInfo;
import com.yugabyte.yw.models.Universe;
import com.yugabyte.yw.models.helpers.NodeDetails;
import com.yugabyte.yw.models.helpers.NodeDetails.NodeState;
import com.yugabyte.yw.models.helpers.TaskType;
import java.util.List;
import java.util.Map;
import java.util.UUID;
import java.util.stream.Collectors;
import org.junit.Before;
import org.junit.Test;
import org.junit.runner.RunWith;
import org.mockito.junit.MockitoJUnitRunner;

@RunWith(MockitoJUnitRunner.class)
public class DeleteNodeFromUniverseTest extends CommissionerBaseTest {

  private Universe defaultUniverse;

  private static final List<TaskType> DELETE_NODE_TASK_SEQUENCE_WITH_INSTANCE =
      ImmutableList.of(
          TaskType.AnsibleDestroyServer, TaskType.DeleteNode, TaskType.UniverseUpdateSucceeded);

  private static final List<TaskType> DELETE_NODE_TASK_SEQUENCE_WITHOUT_INSTANCE =
      ImmutableList.of(TaskType.DeleteNode, TaskType.UniverseUpdateSucceeded);

  @Override
  @Before
  public void setUp() {
    super.setUp();

    Region region = Region.create(defaultProvider, "region-1", "Region 1", "yb-image-1");
    AvailabilityZone.createOrThrow(region, "az-1", "AZ 1", "subnet-1");
    // create default universe
    UniverseDefinitionTaskParams.UserIntent userIntent =
        new UniverseDefinitionTaskParams.UserIntent();
    userIntent.numNodes = 3;
    userIntent.ybSoftwareVersion = "yb-version";
    userIntent.accessKeyCode = "demo-access";
    userIntent.regionList = ImmutableList.of(region.uuid);
    defaultUniverse = createUniverse(defaultCustomer.getCustomerId());
    Universe.saveDetails(
        defaultUniverse.universeUUID,
        ApiUtils.mockUniverseUpdaterWithNodeCallback(
            userIntent,
            node -> {
              if (node.nodeName.equals("host-n1")) {
                node.state = NodeState.InstanceCreated;
              }
            }));
  }

  private TaskInfo submitTask(NodeTaskParams taskParams, String nodeName) {
    taskParams.expectedUniverseVersion = 2;
    taskParams.nodeName = nodeName;
    try {
      UUID taskUUID = commissioner.submit(TaskType.DeleteNodeFromUniverse, taskParams);
      return waitForTask(taskUUID);
    } catch (InterruptedException e) {
      assertNull(e.getMessage());
    }
    return null;
  }

  private void assertStopNodeSequence(
      List<TaskType> sequence, Map<Integer, List<TaskInfo>> subTasksByPosition, boolean isMaster) {
    int position = 0;
    for (TaskType taskType : sequence) {
      List<TaskInfo> tasks = subTasksByPosition.get(position);
      assertEquals(1, tasks.size());
      assertEquals(taskType, tasks.get(0).getTaskType());
      position++;
    }
  }

  @Test
  public void testDeleteInvalidState() {
    NodeTaskParams taskParams = new NodeTaskParams();
    taskParams.universeUUID = defaultUniverse.universeUUID;

    Universe universe = Universe.getOrBadRequest(defaultUniverse.universeUUID);
    NodeDetails nodeDetails = universe.getNode("host-n2");
    assertNotNull(nodeDetails);

    TaskInfo taskInfo = submitTask(taskParams, "host-n2");
    assertEquals(Failure, taskInfo.getTaskState());

    universe = Universe.getOrBadRequest(defaultUniverse.universeUUID);
    nodeDetails = universe.getNode("host-n2");
    assertNotNull(nodeDetails);

    verify(mockNodeManager, times(0)).nodeCommand(any(), any());
  }

  @Test
  public void testExistingInstance() {
    ShellResponse dummyShellResponse = new ShellResponse();
    dummyShellResponse.message = "true";
    when(mockNodeManager.nodeCommand(any(), any())).thenReturn(dummyShellResponse);

    NodeTaskParams taskParams = new NodeTaskParams();
    taskParams.universeUUID = defaultUniverse.universeUUID;

    Universe universe = Universe.getOrBadRequest(defaultUniverse.universeUUID);
    NodeDetails nodeDetails = universe.getNode("host-n1");
    assertNotNull(nodeDetails);

    TaskInfo taskInfo = submitTask(taskParams, "host-n1");
    assertEquals(Success, taskInfo.getTaskState());

    universe = Universe.getOrBadRequest(defaultUniverse.universeUUID);
    nodeDetails = universe.getNode("host-n1");
    assertNull(nodeDetails);

    // Instance existence check + delete instance + delete volume.
    verify(mockNodeManager, times(3)).nodeCommand(any(), any());
    List<TaskInfo> subTasks = taskInfo.getSubTasks();
    Map<Integer, List<TaskInfo>> subTasksByPosition =
        subTasks.stream().collect(Collectors.groupingBy(TaskInfo::getPosition));
    assertEquals(DELETE_NODE_TASK_SEQUENCE_WITH_INSTANCE.size(), subTasksByPosition.size());
    assertStopNodeSequence(DELETE_NODE_TASK_SEQUENCE_WITH_INSTANCE, subTasksByPosition, true);
  }

  @Test
  public void testNonExistingInstance() {
    when(mockNodeManager.nodeCommand(any(), any())).thenReturn(new ShellResponse());

    NodeTaskParams taskParams = new NodeTaskParams();
    taskParams.universeUUID = defaultUniverse.universeUUID;

    Universe universe = Universe.getOrBadRequest(defaultUniverse.universeUUID);
    NodeDetails nodeDetails = universe.getNode("host-n1");
    assertNotNull(nodeDetails);

    TaskInfo taskInfo = submitTask(taskParams, "host-n1");
    assertEquals(Success, taskInfo.getTaskState());

    universe = Universe.getOrBadRequest(defaultUniverse.universeUUID);
    nodeDetails = universe.getNode("host-n1");
    assertNull(nodeDetails);

    // Instance existence check only.
    verify(mockNodeManager, times(1)).nodeCommand(any(), any());
    List<TaskInfo> subTasks = taskInfo.getSubTasks();
    Map<Integer, List<TaskInfo>> subTasksByPosition =
        subTasks.stream().collect(Collectors.groupingBy(TaskInfo::getPosition));
    assertEquals(DELETE_NODE_TASK_SEQUENCE_WITHOUT_INSTANCE.size(), subTasksByPosition.size());
    assertStopNodeSequence(DELETE_NODE_TASK_SEQUENCE_WITHOUT_INSTANCE, subTasksByPosition, true);
  }
}
