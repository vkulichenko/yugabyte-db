package com.yugabyte.yw.controllers;

import static com.yugabyte.yw.common.ApiUtils.getTestUserIntent;
import static com.yugabyte.yw.common.AssertHelper.assertAuditEntry;
import static com.yugabyte.yw.common.AssertHelper.assertBadRequest;
import static com.yugabyte.yw.common.AssertHelper.assertOk;
import static com.yugabyte.yw.common.AssertHelper.assertPlatformException;
import static com.yugabyte.yw.common.AssertHelper.assertValue;
import static com.yugabyte.yw.common.FakeApiHelper.doRequestWithAuthToken;
import static com.yugabyte.yw.common.FakeApiHelper.doRequestWithAuthTokenAndBody;
import static com.yugabyte.yw.common.ModelFactory.awsProvider;
import static com.yugabyte.yw.common.ModelFactory.createUniverse;
import static com.yugabyte.yw.common.PlacementInfoUtil.getAzUuidToNumNodes;
import static com.yugabyte.yw.common.PlacementInfoUtil.updateUniverseDefinition;
import static com.yugabyte.yw.forms.UniverseConfigureTaskParams.ClusterOperationType.CREATE;
import static com.yugabyte.yw.forms.UniverseConfigureTaskParams.ClusterOperationType.EDIT;
import static org.hamcrest.CoreMatchers.allOf;
import static org.hamcrest.CoreMatchers.equalTo;
import static org.hamcrest.CoreMatchers.notNullValue;
import static org.hamcrest.MatcherAssert.assertThat;
import static org.junit.Assert.assertEquals;
import static org.junit.Assert.assertNotNull;
import static org.junit.Assert.assertNull;
import static org.junit.Assert.assertThrows;
import static org.junit.Assert.assertTrue;
import static org.mockito.ArgumentMatchers.any;
import static org.mockito.ArgumentMatchers.eq;
import static org.mockito.Mockito.verify;
import static org.mockito.Mockito.when;
import static play.test.Helpers.contentAsString;

import com.fasterxml.jackson.databind.JsonNode;
import com.fasterxml.jackson.databind.ObjectMapper;
import com.fasterxml.jackson.databind.node.ArrayNode;
import com.fasterxml.jackson.databind.node.ObjectNode;
import com.fasterxml.jackson.databind.type.TypeFactory;
import com.google.common.collect.ImmutableList;
import com.google.common.collect.ImmutableMap;
import com.yugabyte.yw.cloud.PublicCloudConstants;
import com.yugabyte.yw.commissioner.Common;
import com.yugabyte.yw.common.ApiUtils;
import com.yugabyte.yw.common.ModelFactory;
import com.yugabyte.yw.common.PlacementInfoUtil;
import com.yugabyte.yw.common.PlatformServiceException;
import com.yugabyte.yw.forms.UniverseConfigureTaskParams;
import com.yugabyte.yw.forms.UniverseDefinitionTaskParams;
import com.yugabyte.yw.forms.UniverseDefinitionTaskParams.Cluster;
import com.yugabyte.yw.forms.UniverseDefinitionTaskParams.UserIntent;
import com.yugabyte.yw.forms.UniverseTaskParams;
import com.yugabyte.yw.forms.UpgradeParams;
import com.yugabyte.yw.forms.UpgradeTaskParams.UpgradeTaskType;
import com.yugabyte.yw.models.AccessKey;
import com.yugabyte.yw.models.AvailabilityZone;
import com.yugabyte.yw.models.CustomerTask;
import com.yugabyte.yw.models.InstanceType;
import com.yugabyte.yw.models.NodeInstance;
import com.yugabyte.yw.models.Provider;
import com.yugabyte.yw.models.Region;
import com.yugabyte.yw.models.Universe;
import com.yugabyte.yw.models.helpers.DeviceInfo;
import com.yugabyte.yw.models.helpers.NodeDetails;
import com.yugabyte.yw.models.helpers.NodeDetails.NodeState;
import com.yugabyte.yw.models.helpers.PlacementInfo;
import com.yugabyte.yw.models.helpers.TaskType;
import java.io.IOException;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.HashSet;
import java.util.List;
import java.util.Map;
import java.util.UUID;
import java.util.function.Consumer;
import java.util.stream.Collectors;
import junitparams.JUnitParamsRunner;
import junitparams.Parameters;
import org.junit.Test;
import org.junit.runner.RunWith;
import org.mockito.ArgumentCaptor;
import play.libs.Json;
import play.mvc.Result;

@RunWith(JUnitParamsRunner.class)
public class UniverseUiOnlyControllerTest extends UniverseCreateControllerTestBase {

  @Override
  public Result sendCreateRequest(ObjectNode bodyJson) {
    return doRequestWithAuthTokenAndBody(
        "POST", "/api/customers/" + customer.uuid + "/universes", authToken, bodyJson);
  }

  @Override
  public Result sendPrimaryCreateConfigureRequest(ObjectNode bodyJson) {
    bodyJson.put("currentClusterType", "PRIMARY");
    bodyJson.put("clusterOperation", "CREATE");
    return sendOldApiConfigureRequest(bodyJson);
  }

  @Override
  public Result sendPrimaryEditConfigureRequest(ObjectNode bodyJson) {
    bodyJson.put("currentClusterType", "PRIMARY");
    bodyJson.put("clusterOperation", "EDIT");
    return sendOldApiConfigureRequest(bodyJson);
  }

  public Result sendAsyncCreateConfigureRequest(ObjectNode topJson) {
    topJson.put("currentClusterType", "ASYNC");
    topJson.put("clusterOperation", "CREATE");
    return sendOldApiConfigureRequest(topJson);
  }

  public Result sendOldApiConfigureRequest(ObjectNode topJson) {
    return doRequestWithAuthTokenAndBody(
        "POST", "/api/customers/" + customer.uuid + "/universe_configure", authToken, topJson);
  }

  @Override
  protected JsonNode getUniverseJson(Result universeCreateResponse) {
    return Json.parse(contentAsString(universeCreateResponse));
  }

  @Override
  protected JsonNode getUniverseDetailsJson(Result universeConfigureResponse) {
    return Json.parse(contentAsString(universeConfigureResponse));
  }

  /** Migrated to {@link UniverseControllerTest#testUniverseFindByName(String, int)} */
  @Test
  @Parameters({
    "FakeUniverse, 0",
    "TestUniverse, 1",
  })
  public void testFind(String name, int expected) {
    createUniverse("TestUniverse", customer.getCustomerId());
    String url = "/api/customers/" + customer.uuid + "/universes/find?name=" + name;
    Result result = doRequestWithAuthToken("GET", url, authToken);

    JsonNode json = Json.parse(contentAsString(result));
    assertTrue(json.isArray());
    assertEquals(expected, json.size());
    assertAuditEntry(0, customer.uuid);
  }

  @Test
  public void testConfigureInvalidParams() {
    ObjectNode bodyJson = Json.newObject();
    ObjectNode userIntentJson =
        Json.newObject()
            .put("universeName", "Foo_Bar")
            .put("replicationFactor", 3)
            .put("numNodes", 3)
            .put("provider", UUID.randomUUID().toString());
    ArrayNode regionList = Json.newArray().add(UUID.randomUUID().toString());
    userIntentJson.set("regionList", regionList);
    ArrayNode clustersJsonArray =
        Json.newArray().add(Json.newObject().set("userIntent", userIntentJson));
    bodyJson.set("clusters", clustersJsonArray);
    bodyJson.put("clusterOperation", "CREATE");

    Result result = assertPlatformException(() -> sendOldApiConfigureRequest(bodyJson));
    assertBadRequest(result, "currentClusterType");

    bodyJson.remove("clusterOperation");
    bodyJson.put("currentClusterType", "PRIMARY");
    result = assertPlatformException(() -> sendOldApiConfigureRequest(bodyJson));
    assertBadRequest(result, "clusterOperation");
  }

  @Test
  public void testConfigureCreateWithReadOnlyClusters() {
    Provider p = ModelFactory.awsProvider(customer);
    Region r = Region.create(p, "region-1", "PlacementRegion 1", "default-image");
    AvailabilityZone.createOrThrow(r, "az-1", "PlacementAZ 1", "subnet-1");
    AvailabilityZone.createOrThrow(r, "az-2", "PlacementAZ 2", "subnet-2");

    Region rReadOnly = Region.create(p, "region-readOnly-1", "PlacementRegion 1", "default-image");
    AvailabilityZone.createOrThrow(rReadOnly, "az-readOnly-1", "PlacementAZ 1", "subnet-1");
    AvailabilityZone.createOrThrow(rReadOnly, "az-readOnly-2", "PlacementAZ 2", "subnet-2");
    InstanceType i =
        InstanceType.upsert(p.uuid, "c3.xlarge", 10, 5.5, new InstanceType.InstanceTypeDetails());

    UniverseDefinitionTaskParams taskParams = new UniverseDefinitionTaskParams();
    taskParams.nodePrefix = "univWithReadOnlyCreate";
    UUID readOnlyUuid0 = UUID.randomUUID();
    UUID readOnlyUuid1 = UUID.randomUUID();
    taskParams.upsertPrimaryCluster(getTestUserIntent(r, p, i, 5), null);
    taskParams.upsertCluster(getTestUserIntent(rReadOnly, p, i, 5), null, readOnlyUuid0);
    taskParams.upsertCluster(getTestUserIntent(rReadOnly, p, i, 5), null, readOnlyUuid1);

    PlacementInfoUtil.updateUniverseDefinition(
        taskParams, customer.getCustomerId(), taskParams.getPrimaryCluster().uuid, CREATE);
    PlacementInfoUtil.updateUniverseDefinition(
        taskParams, customer.getCustomerId(), readOnlyUuid0, CREATE);
    PlacementInfoUtil.updateUniverseDefinition(
        taskParams, customer.getCustomerId(), readOnlyUuid1, CREATE);

    UniverseDefinitionTaskParams.Cluster primaryCluster = taskParams.getPrimaryCluster();
    List<PlacementInfo.PlacementAZ> azList =
        primaryCluster.placementInfo.cloudList.get(0).regionList.get(0).azList;
    assertEquals(azList.size(), 2);

    UniverseDefinitionTaskParams.Cluster readOnlyCluster0 =
        taskParams.getClusterByUuid(readOnlyUuid0);
    azList = readOnlyCluster0.placementInfo.cloudList.get(0).regionList.get(0).azList;
    assertEquals(azList.size(), 2);

    UniverseDefinitionTaskParams.Cluster readOnlyCluster1 =
        taskParams.getClusterByUuid(readOnlyUuid1);
    azList = readOnlyCluster1.placementInfo.cloudList.get(0).regionList.get(0).azList;
    assertEquals(azList.size(), 2);

    Map<UUID, Integer> azUUIDToNumNodeMap = getAzUuidToNumNodes(primaryCluster.placementInfo);
    Map<UUID, Integer> azUUIDToNumNodeMapReadOnly0 =
        getAzUuidToNumNodes(readOnlyCluster0.placementInfo);
    Map<UUID, Integer> azUUIDToNumNodeMapReadOnly1 =
        getAzUuidToNumNodes(readOnlyCluster1.placementInfo);
    for (Map.Entry<UUID, Integer> entry : azUUIDToNumNodeMapReadOnly0.entrySet()) {
      UUID uuid = entry.getKey();
      int numNodes = entry.getValue();
      if (azUUIDToNumNodeMap.containsKey(uuid)) {
        int prevNumNodes = azUUIDToNumNodeMap.get(uuid);
        azUUIDToNumNodeMap.put(uuid, prevNumNodes + numNodes);
      } else {
        azUUIDToNumNodeMap.put(uuid, numNodes);
      }
    }
    for (Map.Entry<UUID, Integer> entry : azUUIDToNumNodeMapReadOnly1.entrySet()) {
      UUID uuid = entry.getKey();
      int numNodes = entry.getValue();
      if (azUUIDToNumNodeMap.containsKey(uuid)) {
        int prevNumNodes = azUUIDToNumNodeMap.get(uuid);
        azUUIDToNumNodeMap.put(uuid, prevNumNodes + numNodes);
      } else {
        azUUIDToNumNodeMap.put(uuid, numNodes);
      }
    }
    ObjectNode topJson = (ObjectNode) Json.toJson(taskParams);
    Result result = sendAsyncCreateConfigureRequest(topJson);

    assertOk(result);
    JsonNode json = Json.parse(contentAsString(result));
    assertTrue(json.get("nodeDetailsSet").isArray());
    ArrayNode nodeDetailJson = (ArrayNode) json.get("nodeDetailsSet");
    assertEquals(15, nodeDetailJson.size());
    assertTrue(areConfigObjectsEqual(nodeDetailJson, azUUIDToNumNodeMap));
    assertAuditEntry(1, customer.uuid);
  }

  @Test
  public void testConfigureEditOnPremInvalidNodeAZCombo_fail() {
    Provider p = ModelFactory.newProvider(customer, Common.CloudType.onprem);
    Region r = Region.create(p, "region-1", "PlacementRegion 1", "default-image");
    AvailabilityZone az1 = AvailabilityZone.createOrThrow(r, "az-1", "PlacementAZ 1", "subnet-1");
    AvailabilityZone az2 = AvailabilityZone.createOrThrow(r, "az-2", "PlacementAZ 2", "subnet-2");

    List<AvailabilityZone> azList = new ArrayList<>();
    azList.add(az1);
    azList.add(az2);

    UniverseDefinitionTaskParams taskParams = setupOnPremTestData(6, p, r, azList);

    InstanceType i =
        InstanceType.upsert(p.uuid, "type.small", 10, 5.5, new InstanceType.InstanceTypeDetails());

    taskParams.nodePrefix = "test_uni";
    UniverseDefinitionTaskParams.UserIntent userIntent = getTestUserIntent(r, p, i, 5);
    userIntent.providerType = Common.CloudType.onprem;
    userIntent.instanceType = "type.small";
    taskParams.upsertPrimaryCluster(userIntent, null);
    UniverseDefinitionTaskParams.Cluster primaryCluster = taskParams.getPrimaryCluster();
    updateUniverseDefinition(taskParams, customer.getCustomerId(), primaryCluster.uuid, CREATE);

    // Set the nodes state to inUse
    int k = 0;
    for (NodeInstance ni : NodeInstance.listByProvider(p.uuid)) {
      if (k < 5) {
        k++;
        ni.setInUse(true);
        ni.save();
      } else {
        break;
      }
    }

    // Simulate a running universe by setting existing nodes to Live state.
    for (NodeDetails nd : taskParams.nodeDetailsSet) {
      nd.state = NodeState.Live;
    }

    // Set placement info with addition of nodes that is more than what has been configured
    for (int m = 0; m < 7; m++) {
      NodeDetails nd = new NodeDetails();
      nd.state = NodeState.ToBeAdded;
      nd.azUuid = az1.uuid;
      nd.placementUuid = primaryCluster.uuid;
      taskParams.nodeDetailsSet.add(nd);
    }
    // HERE
    ObjectNode topJson = (ObjectNode) Json.toJson(taskParams);
    Result result = assertPlatformException(() -> sendPrimaryEditConfigureRequest(topJson));
    assertBadRequest(result, "Invalid Node/AZ combination for given instance type type.small");
    assertAuditEntry(0, customer.uuid);
  }

  @Test
  public void testCustomConfigureEditWithPureExpand() {
    Provider p = ModelFactory.awsProvider(customer);
    Universe u = createUniverse(customer.getCustomerId());

    Region r = Region.create(p, "region-1", "PlacementRegion 1", "default-image");
    AvailabilityZone.createOrThrow(r, "az-1", "PlacementAZ 1", "subnet-1");
    AvailabilityZone.createOrThrow(r, "az-2", "PlacementAZ 2", "subnet-2");
    AvailabilityZone.createOrThrow(r, "az-3", "PlacementAZ 3", "subnet-3");
    InstanceType i =
        InstanceType.upsert(p.uuid, "c3.xlarge", 10, 5.5, new InstanceType.InstanceTypeDetails());

    UniverseDefinitionTaskParams utd = new UniverseDefinitionTaskParams();
    utd.universeUUID = u.universeUUID;
    UniverseDefinitionTaskParams.UserIntent ui = getTestUserIntent(r, p, i, 5);
    ui.universeName = u.name;
    ui.ybSoftwareVersion = "1.0";
    ui.preferredRegion = ui.regionList.get(0);
    utd.upsertPrimaryCluster(ui, null);
    PlacementInfoUtil.updateUniverseDefinition(
        utd,
        customer.getCustomerId(),
        utd.getPrimaryCluster().uuid,
        UniverseConfigureTaskParams.ClusterOperationType.CREATE);
    Universe.UniverseUpdater updater = universe -> universe.setUniverseDetails(utd);
    Universe.saveDetails(u.universeUUID, updater);
    u = Universe.getOrBadRequest(u.universeUUID);
    int totalNumNodesAfterExpand = 0;
    Map<UUID, Integer> azUuidToNumNodes =
        getAzUuidToNumNodes(u.getUniverseDetails().nodeDetailsSet);
    for (Map.Entry<UUID, Integer> entry : azUuidToNumNodes.entrySet()) {
      totalNumNodesAfterExpand += entry.getValue() + 1;
      azUuidToNumNodes.put(entry.getKey(), entry.getValue() + 1);
    }
    UniverseDefinitionTaskParams editTestUTD = u.getUniverseDetails();
    UniverseDefinitionTaskParams.Cluster primaryCluster = editTestUTD.getPrimaryCluster();
    primaryCluster.userIntent.numNodes = totalNumNodesAfterExpand;
    primaryCluster.placementInfo = constructPlacementInfoObject(azUuidToNumNodes);

    ObjectNode editJson = (ObjectNode) Json.toJson(editTestUTD);
    Result result = sendPrimaryEditConfigureRequest(editJson);
    assertOk(result);
    JsonNode json = Json.parse(contentAsString(result));
    assertTrue(json.get("nodeDetailsSet").isArray());
    ArrayNode nodeDetailJson = (ArrayNode) json.get("nodeDetailsSet");
    assertEquals(nodeDetailJson.size(), totalNumNodesAfterExpand);
    assertTrue(areConfigObjectsEqual(nodeDetailJson, azUuidToNumNodes));
    assertAuditEntry(1, customer.uuid);
    assertTrue(json.get("updateOptions").isArray());
    ArrayNode updateOptionsJson = (ArrayNode) json.get("updateOptions");
    assertEquals(1, updateOptionsJson.size());
    assertEquals("FULL_MOVE", updateOptionsJson.get(0).asText());
  }

  @Test
  public void testOnPremConfigureValidAZNodeComboNonEmptyNodeDetailsSet() {
    Provider p = ModelFactory.newProvider(customer, Common.CloudType.onprem);
    Region r = Region.create(p, "region-1", "PlacementRegion 1", "default-image");
    AvailabilityZone az1 = AvailabilityZone.createOrThrow(r, "az-1", "PlacementAZ 1", "subnet-1");

    List<AvailabilityZone> azList = new ArrayList<>();
    azList.add(az1);

    UniverseDefinitionTaskParams taskParams = setupOnPremTestData(6, p, r, azList);

    UniverseDefinitionTaskParams.Cluster primaryCluster = taskParams.getPrimaryCluster();

    updateUniverseDefinition(taskParams, customer.getCustomerId(), primaryCluster.uuid, CREATE);

    ObjectNode topJson = (ObjectNode) Json.toJson(taskParams);
    Result result = sendPrimaryCreateConfigureRequest(topJson);
    assertOk(result);
    assertAuditEntry(1, customer.uuid);
  }

  @Test
  public void testUniverseCreateWithInvalidTServerJson() {
    UUID fakeTaskUUID = UUID.randomUUID();
    when(mockCommissioner.submit(any(TaskType.class), any(UniverseDefinitionTaskParams.class)))
        .thenReturn(fakeTaskUUID);

    Provider p = ModelFactory.awsProvider(customer);
    String accessKeyCode = "someKeyCode";
    AccessKey.create(p.uuid, accessKeyCode, new AccessKey.KeyInfo());
    Region r = Region.create(p, "region-1", "PlacementRegion 1", "default-image");
    AvailabilityZone.createOrThrow(r, "az-1", "PlacementAZ 1", "subnet-1");
    AvailabilityZone.createOrThrow(r, "az-2", "PlacementAZ 2", "subnet-2");
    AvailabilityZone.createOrThrow(r, "az-3", "PlacementAZ 3", "subnet-3");
    Universe u = createUniverse(customer.getCustomerId());
    InstanceType i =
        InstanceType.upsert(p.uuid, "c3.xlarge", 10, 5.5, new InstanceType.InstanceTypeDetails());

    ObjectNode bodyJson = Json.newObject();
    ObjectNode userIntentJson =
        Json.newObject()
            .put("masterGFlags", "abcd")
            .put("universeName", u.name)
            .put("instanceType", i.getInstanceTypeCode())
            .put("replicationFactor", 3)
            .put("numNodes", 3)
            .put("provider", p.uuid.toString())
            .put("accessKeyCode", accessKeyCode);
    ArrayNode regionList = Json.newArray().add(r.uuid.toString());
    userIntentJson.set("regionList", regionList);
    Cluster cluster = u.getUniverseDetails().clusters.get(0);
    ObjectNode clusterJson = Json.newObject();
    clusterJson.set("userIntent", userIntentJson);
    clusterJson.set("uuid", Json.toJson(cluster.uuid));
    ArrayNode clustersJsonArray = Json.newArray().add(clusterJson);
    bodyJson.set("clusters", clustersJsonArray);
    bodyJson.set("nodeDetailsSet", Json.newArray());

    String url = "/api/customers/" + customer.uuid + "/universes/" + u.universeUUID;
    Result result = doRequestWithAuthTokenAndBody("PUT", url, authToken, bodyJson);

    assertOk(result);
    JsonNode json = Json.parse(contentAsString(result));
    assertValue(json, "universeUUID", u.universeUUID.toString());
    assertNotNull(json.get("universeDetails"));
    assertTrue(json.get("universeConfig").asText().isEmpty());

    CustomerTask th = CustomerTask.find.query().where().eq("task_uuid", fakeTaskUUID).findOne();
    assertNotNull(th);
    assertThat(th.getCustomerUUID(), allOf(notNullValue(), equalTo(customer.uuid)));
    assertThat(th.getTargetName(), allOf(notNullValue(), equalTo("Test Universe")));
    assertThat(th.getType(), allOf(notNullValue(), equalTo(CustomerTask.TaskType.Update)));
    assertAuditEntry(1, customer.uuid);
  }

  @Test
  public void testUniverseExpand() {
    UUID fakeTaskUUID = UUID.randomUUID();
    when(mockCommissioner.submit(any(TaskType.class), any(UniverseDefinitionTaskParams.class)))
        .thenReturn(fakeTaskUUID);

    Provider p = ModelFactory.awsProvider(customer);
    String accessKeyCode = "someKeyCode";
    AccessKey.create(p.uuid, accessKeyCode, new AccessKey.KeyInfo());
    Region r = Region.create(p, "region-1", "PlacementRegion 1", "default-image");
    AvailabilityZone.createOrThrow(r, "az-1", "PlacementAZ 1", "subnet-1");
    AvailabilityZone.createOrThrow(r, "az-2", "PlacementAZ 2", "subnet-2");
    AvailabilityZone.createOrThrow(r, "az-3", "PlacementAZ 3", "subnet-3");
    Universe u = createUniverse(customer.getCustomerId());
    InstanceType i =
        InstanceType.upsert(p.uuid, "c3.xlarge", 10, 5.5, new InstanceType.InstanceTypeDetails());

    ObjectNode bodyJson = Json.newObject();
    ObjectNode userIntentJson =
        Json.newObject()
            .put("universeName", u.name)
            .put("numNodes", 5)
            .put("instanceType", i.getInstanceTypeCode())
            .put("replicationFactor", 3)
            .put("provider", p.uuid.toString())
            .put("accessKeyCode", accessKeyCode);
    ArrayNode regionList = Json.newArray().add(r.uuid.toString());
    userIntentJson.set("regionList", regionList);
    Cluster cluster = u.getUniverseDetails().clusters.get(0);
    ObjectNode clusterJson = Json.newObject();
    clusterJson.set("userIntent", userIntentJson);
    clusterJson.set("uuid", Json.toJson(cluster.uuid));
    ArrayNode clustersJsonArray = Json.newArray().add(clusterJson);
    bodyJson.set("clusters", clustersJsonArray);
    bodyJson.set("nodeDetailsSet", Json.newArray());

    String url = "/api/customers/" + customer.uuid + "/universes/" + u.universeUUID;
    Result result = doRequestWithAuthTokenAndBody("PUT", url, authToken, bodyJson);

    assertOk(result);
    JsonNode json = Json.parse(contentAsString(result));
    assertValue(json, "universeUUID", u.universeUUID.toString());
    JsonNode universeDetails = json.get("universeDetails");
    assertNotNull(universeDetails);
    JsonNode clustersJson = universeDetails.get("clusters");
    assertNotNull(clustersJson);
    JsonNode primaryClusterJson = clustersJson.get(0);
    assertNotNull(primaryClusterJson);
    assertNotNull(primaryClusterJson.get("userIntent"));
    assertAuditEntry(1, customer.uuid);

    fakeTaskUUID = UUID.randomUUID();
    when(mockCommissioner.submit(any(TaskType.class), any(UniverseDefinitionTaskParams.class)))
        .thenReturn(fakeTaskUUID);
    // Try universe expand only, and re-check.
    userIntentJson.put("numNodes", 9);
    result = doRequestWithAuthTokenAndBody("PUT", url, authToken, bodyJson);
    assertOk(result);
    json = Json.parse(contentAsString(result));
    assertValue(json, "universeUUID", u.universeUUID.toString());
    universeDetails = json.get("universeDetails");
    assertNotNull(universeDetails);
    clustersJson = universeDetails.get("clusters");
    assertNotNull(clustersJson);
    primaryClusterJson = clustersJson.get(0);
    assertNotNull(primaryClusterJson);
    assertNotNull(primaryClusterJson.get("userIntent"));
    assertAuditEntry(2, customer.uuid);
  }

  @Test
  public void testUniverseExpandWithTransitNodes() {
    UUID fakeTaskUUID = UUID.randomUUID();

    Provider p = ModelFactory.awsProvider(customer);
    Region r = Region.create(p, "region-1", "PlacementRegion 1", "default-image");
    AvailabilityZone.createOrThrow(r, "az-1", "PlacementAZ 1", "subnet-1");
    AvailabilityZone.createOrThrow(r, "az-2", "PlacementAZ 2", "subnet-2");
    AvailabilityZone.createOrThrow(r, "az-3", "PlacementAZ 3", "subnet-3");
    Universe u = createUniverse(customer.getCustomerId());
    Universe.saveDetails(u.universeUUID, ApiUtils.mockUniverseUpdater());
    InstanceType i =
        InstanceType.upsert(p.uuid, "c3.xlarge", 10, 5.5, new InstanceType.InstanceTypeDetails());

    setInTransitNode(u.universeUUID);
    ObjectNode bodyJson = Json.newObject();
    ObjectNode userIntentJson =
        Json.newObject()
            .put("universeName", u.name)
            .put("numNodes", 5)
            .put("instanceType", i.getInstanceTypeCode())
            .put("replicationFactor", 3)
            .put("provider", p.uuid.toString());
    ArrayNode regionList = Json.newArray().add(r.uuid.toString());
    userIntentJson.set("regionList", regionList);
    ArrayNode clustersJsonArray =
        Json.newArray().add(Json.newObject().set("userIntent", userIntentJson));
    bodyJson.set("clusters", clustersJsonArray);

    String url = "/api/customers/" + customer.uuid + "/universes/" + u.universeUUID;
    Result result =
        assertPlatformException(
            () -> doRequestWithAuthTokenAndBody("PUT", url, authToken, bodyJson));
    assertBadRequest(result, "as it has nodes in one of");
    assertNull(CustomerTask.find.query().where().eq("task_uuid", fakeTaskUUID).findOne());
    assertAuditEntry(0, customer.uuid);
  }

  @Test
  public void testUniverseUpdateWithInvalidParams() {
    Universe u = createUniverse(customer.getCustomerId());
    String url = "/api/customers/" + customer.uuid + "/universes/" + u.universeUUID;
    Result result =
        assertPlatformException(
            () -> doRequestWithAuthTokenAndBody("PUT", url, authToken, Json.newObject()));
    assertBadRequest(result, "clusters: This field is required");
    assertAuditEntry(0, customer.uuid);
  }

  @Test
  public void testUniverseUpdateWithChangingIP() {
    UUID fakeTaskUUID = UUID.randomUUID();
    when(mockCommissioner.submit(any(TaskType.class), any(UniverseDefinitionTaskParams.class)))
        .thenReturn(fakeTaskUUID);

    Provider p = ModelFactory.awsProvider(customer);
    String accessKeyCode = "someKeyCode";
    AccessKey.create(p.uuid, accessKeyCode, new AccessKey.KeyInfo());
    Region r = Region.create(p, "region-1", "PlacementRegion 1", "default-image");
    AvailabilityZone.createOrThrow(r, "az-1", "PlacementAZ 1", "subnet-1");
    Universe u = createUniverse(customer.getCustomerId());
    InstanceType i =
        InstanceType.upsert(p.uuid, "c3.xlarge", 10, 5.5, new InstanceType.InstanceTypeDetails());

    ObjectNode userIntentJson =
        Json.newObject()
            .put("universeName", u.name)
            .put("numNodes", 3)
            .put("instanceType", i.getInstanceTypeCode())
            .put("replicationFactor", 3)
            .put("provider", p.uuid.toString())
            .put("accessKeyCode", accessKeyCode);
    ArrayNode regionList = Json.newArray().add(r.uuid.toString());
    userIntentJson.set("regionList", regionList);
    ArrayNode nodeDetailsJsonArray = Json.newArray();
    Universe.saveDetails(
        u.universeUUID,
        universe -> {
          UniverseDefinitionTaskParams universeDetails = universe.getUniverseDetails();
          Cluster cluster = universeDetails.clusters.get(0);
          cluster.userIntent = Json.fromJson(userIntentJson, UserIntent.class);
          for (int idx = 0; idx < 3; idx++) {
            NodeDetails node = ApiUtils.getDummyNodeDetails(idx, NodeState.Live);
            node.placementUuid = cluster.uuid;
            nodeDetailsJsonArray.add(Json.toJson(node));
            if (idx == 1) {
              node.cloudInfo.private_ip += "1"; // changing ip from 10.0.0.1 to 10.0.0.11
            }
            universeDetails.nodeDetailsSet.add(node);
          }
        });

    Cluster cluster = u.getUniverseDetails().clusters.get(0);
    ObjectNode clusterJson = Json.newObject();
    clusterJson.set("userIntent", userIntentJson);
    clusterJson.set("uuid", Json.toJson(cluster.uuid));
    ArrayNode clustersJsonArray = Json.newArray().add(clusterJson);

    ObjectNode bodyJson = Json.newObject();
    bodyJson.set("clusters", clustersJsonArray);
    bodyJson.set("nodeDetailsSet", nodeDetailsJsonArray);

    String url = "/api/customers/" + customer.uuid + "/universes/" + u.universeUUID;
    Result result =
        assertPlatformException(
            () -> doRequestWithAuthTokenAndBody("PUT", url, authToken, bodyJson));
    assertBadRequest(result, "Illegal attempt to change private ip to 10.0.0.1 for node host-n1");
  }

  @Test
  public void testUniverseUpdateIllegalIP() {
    UUID fakeTaskUUID = UUID.randomUUID();
    when(mockCommissioner.submit(any(TaskType.class), any(UniverseDefinitionTaskParams.class)))
        .thenReturn(fakeTaskUUID);

    Provider p = ModelFactory.awsProvider(customer);
    String accessKeyCode = "someKeyCode";
    AccessKey.create(p.uuid, accessKeyCode, new AccessKey.KeyInfo());
    Region r = Region.create(p, "region-1", "PlacementRegion 1", "default-image");
    AvailabilityZone.createOrThrow(r, "az-1", "PlacementAZ 1", "subnet-1");
    Universe u = createUniverse(customer.getCustomerId());
    InstanceType i =
        InstanceType.upsert(p.uuid, "c3.xlarge", 10, 5.5, new InstanceType.InstanceTypeDetails());

    ObjectNode userIntentJson =
        Json.newObject()
            .put("universeName", u.name)
            .put("numNodes", 3)
            .put("instanceType", i.getInstanceTypeCode())
            .put("replicationFactor", 3)
            .put("provider", p.uuid.toString())
            .put("providerType", p.code)
            .put("accessKeyCode", accessKeyCode);
    ArrayNode regionList = Json.newArray().add(r.uuid.toString());
    userIntentJson.set("regionList", regionList);
    ArrayNode nodeDetailsJsonArray = Json.newArray();
    Universe.saveDetails(
        u.universeUUID,
        universe -> {
          UniverseDefinitionTaskParams universeDetails = universe.getUniverseDetails();
          Cluster cluster = universeDetails.clusters.get(0);
          cluster.userIntent = Json.fromJson(userIntentJson, UserIntent.class);
          for (int idx = 0; idx < 3; idx++) {
            NodeDetails node = ApiUtils.getDummyNodeDetails(idx, NodeState.ToBeAdded);
            node.nodeName = "";
            node.placementUuid = cluster.uuid;
            if (idx == 0) {
              node.cloudInfo.private_ip = FORBIDDEN_IP_2;
            }
            nodeDetailsJsonArray.add(Json.toJson(node));
            universeDetails.nodeDetailsSet.add(node);
          }
        });

    Cluster cluster = u.getUniverseDetails().clusters.get(0);
    ObjectNode clusterJson = Json.newObject();
    clusterJson.set("userIntent", userIntentJson);
    clusterJson.set("uuid", Json.toJson(cluster.uuid));
    ArrayNode clustersJsonArray = Json.newArray().add(clusterJson);

    ObjectNode bodyJson = Json.newObject();
    bodyJson.set("clusters", clustersJsonArray);
    bodyJson.set("nodeDetailsSet", nodeDetailsJsonArray);

    String url = "/api/customers/" + customer.uuid + "/universes/" + u.universeUUID;
    Result result =
        assertPlatformException(
            () -> doRequestWithAuthTokenAndBody("PUT", url, authToken, bodyJson));
    assertBadRequest(result, String.format("Forbidden ip %s for node", FORBIDDEN_IP_2));
  }

  @Test
  public void testUniverseUpdateWithAllNodesRemoved() {
    UUID fakeTaskUUID = UUID.randomUUID();
    when(mockCommissioner.submit(any(TaskType.class), any(UniverseDefinitionTaskParams.class)))
        .thenReturn(fakeTaskUUID);

    Provider p = ModelFactory.awsProvider(customer);
    String accessKeyCode = "someKeyCode";
    AccessKey.create(p.uuid, accessKeyCode, new AccessKey.KeyInfo());
    Region r = Region.create(p, "region-1", "PlacementRegion 1", "default-image");
    AvailabilityZone.createOrThrow(r, "az-1", "PlacementAZ 1", "subnet-1");
    Universe u = createUniverse(customer.getCustomerId());
    InstanceType i =
        InstanceType.upsert(p.uuid, "c3.xlarge", 10, 5.5, new InstanceType.InstanceTypeDetails());

    ObjectNode userIntentJson =
        Json.newObject()
            .put("universeName", u.name)
            .put("numNodes", 3)
            .put("instanceType", i.getInstanceTypeCode())
            .put("replicationFactor", 3)
            .put("provider", p.uuid.toString())
            .put("accessKeyCode", accessKeyCode);
    ArrayNode regionList = Json.newArray().add(r.uuid.toString());
    userIntentJson.set("regionList", regionList);
    ArrayNode nodeDetailsJsonArray = Json.newArray();
    Universe.saveDetails(
        u.universeUUID,
        universe -> {
          UniverseDefinitionTaskParams universeDetails = universe.getUniverseDetails();
          Cluster cluster = universeDetails.clusters.get(0);
          cluster.userIntent = Json.fromJson(userIntentJson, UserIntent.class);
          for (int idx = 0; idx < 3; idx++) {
            NodeDetails node = ApiUtils.getDummyNodeDetails(idx, NodeState.ToBeRemoved);
            node.placementUuid = cluster.uuid;
            nodeDetailsJsonArray.add(Json.toJson(node));
            universeDetails.nodeDetailsSet.add(node);
          }
        });

    Cluster cluster = u.getUniverseDetails().clusters.get(0);
    ObjectNode clusterJson = Json.newObject();
    clusterJson.set("userIntent", userIntentJson);
    clusterJson.set("uuid", Json.toJson(cluster.uuid));
    ArrayNode clustersJsonArray = Json.newArray().add(clusterJson);

    ObjectNode bodyJson = Json.newObject();
    bodyJson.set("clusters", clustersJsonArray);
    bodyJson.set("nodeDetailsSet", nodeDetailsJsonArray);

    String url = "/api/customers/" + customer.uuid + "/universes/" + u.universeUUID;
    Result result =
        assertPlatformException(
            () -> doRequestWithAuthTokenAndBody("PUT", url, authToken, bodyJson));
    assertBadRequest(result, "All nodes cannot be removed for cluster " + cluster.uuid);
  }

  @Test
  public void testUniverseUpdateWithMissingNodes() {
    UUID fakeTaskUUID = UUID.randomUUID();
    when(mockCommissioner.submit(any(TaskType.class), any(UniverseDefinitionTaskParams.class)))
        .thenReturn(fakeTaskUUID);

    Provider p = ModelFactory.awsProvider(customer);
    String accessKeyCode = "someKeyCode";
    AccessKey.create(p.uuid, accessKeyCode, new AccessKey.KeyInfo());
    Region r = Region.create(p, "region-1", "PlacementRegion 1", "default-image");
    AvailabilityZone.createOrThrow(r, "az-1", "PlacementAZ 1", "subnet-1");
    Universe u = createUniverse(customer.getCustomerId());
    InstanceType i =
        InstanceType.upsert(p.uuid, "c3.xlarge", 10, 5.5, new InstanceType.InstanceTypeDetails());
    ObjectNode userIntentJson =
        Json.newObject()
            .put("universeName", u.name)
            .put("numNodes", 3)
            .put("instanceType", i.getInstanceTypeCode())
            .put("replicationFactor", 3)
            .put("provider", p.uuid.toString())
            .put("accessKeyCode", accessKeyCode);

    ArrayNode regionList = Json.newArray().add(r.uuid.toString());
    userIntentJson.set("regionList", regionList);

    ArrayNode nodeDetailsJsonArray = Json.newArray();
    Universe.saveDetails(
        u.universeUUID,
        universe -> {
          UniverseDefinitionTaskParams universeDetails = universe.getUniverseDetails();
          Cluster cluster = universeDetails.clusters.get(0);
          cluster.userIntent = Json.fromJson(userIntentJson, UserIntent.class);
          for (int idx = 0; idx < 3; idx++) {
            NodeDetails node = ApiUtils.getDummyNodeDetails(idx, NodeState.Live);
            node.placementUuid = cluster.uuid;
            if (idx > 0) {
              // Exclude the first node in the payload.
              nodeDetailsJsonArray.add(Json.toJson(node));
            }
            universeDetails.nodeDetailsSet.add(node);
          }
        });
    UniverseDefinitionTaskParams universeDetails = u.getUniverseDetails();
    Cluster cluster = universeDetails.clusters.get(0);
    ObjectNode clusterJson = Json.newObject();
    clusterJson.set("userIntent", userIntentJson);
    clusterJson.set("uuid", Json.toJson(cluster.uuid));
    ArrayNode clustersJsonArray = Json.newArray().add(clusterJson);

    ObjectNode bodyJson = Json.newObject();
    bodyJson.set("clusters", clustersJsonArray);
    bodyJson.set("nodeDetailsSet", nodeDetailsJsonArray);
    String url = "/api/customers/" + customer.uuid + "/universes/" + u.universeUUID;
    Result result =
        assertPlatformException(
            () -> doRequestWithAuthTokenAndBody("PUT", url, authToken, bodyJson));
    assertBadRequest(result, "Node host-n0 is missing");
  }

  @Test
  public void testUniverseUpdateWithUnknownNodes() {
    UUID fakeTaskUUID = UUID.randomUUID();
    when(mockCommissioner.submit(any(TaskType.class), any(UniverseDefinitionTaskParams.class)))
        .thenReturn(fakeTaskUUID);

    Provider p = ModelFactory.awsProvider(customer);
    String accessKeyCode = "someKeyCode";
    AccessKey.create(p.uuid, accessKeyCode, new AccessKey.KeyInfo());
    Region r = Region.create(p, "region-1", "PlacementRegion 1", "default-image");
    AvailabilityZone.createOrThrow(r, "az-1", "PlacementAZ 1", "subnet-1");
    Universe u = createUniverse(customer.getCustomerId());
    InstanceType i =
        InstanceType.upsert(p.uuid, "c3.xlarge", 10, 5.5, new InstanceType.InstanceTypeDetails());
    ObjectNode userIntentJson =
        Json.newObject()
            .put("universeName", u.name)
            .put("numNodes", 3)
            .put("instanceType", i.getInstanceTypeCode())
            .put("replicationFactor", 3)
            .put("provider", p.uuid.toString())
            .put("accessKeyCode", accessKeyCode);

    ArrayNode regionList = Json.newArray().add(r.uuid.toString());
    userIntentJson.set("regionList", regionList);

    ArrayNode nodeDetailsJsonArray = Json.newArray();
    u =
        Universe.saveDetails(
            u.universeUUID,
            universe -> {
              UniverseDefinitionTaskParams universeDetails = universe.getUniverseDetails();
              Cluster cluster = universeDetails.clusters.get(0);
              cluster.userIntent = Json.fromJson(userIntentJson, UserIntent.class);
              for (int idx = 0; idx < 3; idx++) {
                NodeDetails node = ApiUtils.getDummyNodeDetails(idx, NodeState.Live);
                node.placementUuid = cluster.uuid;
                nodeDetailsJsonArray.add(Json.toJson(node));
                if (idx > 0) {
                  // Exclude the first node in the universe.
                  universeDetails.nodeDetailsSet.add(node);
                }
              }
            });
    UniverseDefinitionTaskParams universeDetails = u.getUniverseDetails();
    Cluster cluster = universeDetails.clusters.get(0);
    ObjectNode clusterJson = Json.newObject();
    clusterJson.set("userIntent", userIntentJson);
    clusterJson.set("uuid", Json.toJson(cluster.uuid));
    ArrayNode clustersJsonArray = Json.newArray().add(clusterJson);

    ObjectNode bodyJson = Json.newObject();
    bodyJson.set("clusters", clustersJsonArray);
    bodyJson.set("nodeDetailsSet", nodeDetailsJsonArray);
    String url = "/api/customers/" + customer.uuid + "/universes/" + u.universeUUID;
    Result result =
        assertPlatformException(
            () -> doRequestWithAuthTokenAndBody("PUT", url, authToken, bodyJson));
    assertBadRequest(result, "Unknown nodes host-n0");
  }

  @Test
  public void testUniverseUpdateWithNodeNameInToBeAdded() {
    UUID fakeTaskUUID = UUID.randomUUID();
    when(mockCommissioner.submit(any(TaskType.class), any(UniverseDefinitionTaskParams.class)))
        .thenReturn(fakeTaskUUID);

    Provider p = ModelFactory.awsProvider(customer);
    String accessKeyCode = "someKeyCode";
    AccessKey.create(p.uuid, accessKeyCode, new AccessKey.KeyInfo());
    Region r = Region.create(p, "region-1", "PlacementRegion 1", "default-image");
    AvailabilityZone.createOrThrow(r, "az-1", "PlacementAZ 1", "subnet-1");
    Universe u = createUniverse(customer.getCustomerId());
    InstanceType i =
        InstanceType.upsert(p.uuid, "c3.xlarge", 10, 5.5, new InstanceType.InstanceTypeDetails());

    ObjectNode userIntentJson =
        Json.newObject()
            .put("universeName", u.name)
            .put("numNodes", 3)
            .put("instanceType", i.getInstanceTypeCode())
            .put("replicationFactor", 3)
            .put("provider", p.uuid.toString())
            .put("accessKeyCode", accessKeyCode);
    ArrayNode regionList = Json.newArray().add(r.uuid.toString());
    userIntentJson.set("regionList", regionList);

    Cluster cluster = u.getUniverseDetails().clusters.get(0);
    ObjectNode clusterJson = Json.newObject();
    clusterJson.set("userIntent", userIntentJson);
    clusterJson.set("uuid", Json.toJson(cluster.uuid));
    ArrayNode clustersJsonArray = Json.newArray().add(clusterJson);

    ArrayNode nodeDetailsJsonArray = Json.newArray();
    for (int idx = 0; idx < 3; idx++) {
      NodeDetails node = ApiUtils.getDummyNodeDetails(idx, NodeState.Live);
      node.placementUuid = cluster.uuid;
      if (idx == 0) {
        node.state = NodeState.ToBeAdded;
      }
      nodeDetailsJsonArray.add(Json.toJson(node));
    }

    ObjectNode bodyJson = Json.newObject();
    bodyJson.set("clusters", clustersJsonArray);
    bodyJson.set("nodeDetailsSet", nodeDetailsJsonArray);

    String url = "/api/customers/" + customer.uuid + "/universes/" + u.universeUUID;
    Result result =
        assertPlatformException(
            () -> doRequestWithAuthTokenAndBody("PUT", url, authToken, bodyJson));
    assertBadRequest(result, "Node name host-n0 cannot be present");
  }

  @Test
  public void testUniverseUpdateWithValidParams() {
    UUID fakeTaskUUID = UUID.randomUUID();
    when(mockCommissioner.submit(any(TaskType.class), any(UniverseDefinitionTaskParams.class)))
        .thenReturn(fakeTaskUUID);

    Provider p = ModelFactory.awsProvider(customer);
    String accessKeyCode = "someKeyCode";
    AccessKey.create(p.uuid, accessKeyCode, new AccessKey.KeyInfo());
    Region r = Region.create(p, "region-1", "PlacementRegion 1", "default-image");
    AvailabilityZone.createOrThrow(r, "az-1", "PlacementAZ 1", "subnet-1");
    AvailabilityZone.createOrThrow(r, "az-2", "PlacementAZ 2", "subnet-2");
    AvailabilityZone.createOrThrow(r, "az-3", "PlacementAZ 3", "subnet-3");
    Universe u = createUniverse(customer.getCustomerId());
    InstanceType i =
        InstanceType.upsert(p.uuid, "c3.xlarge", 10, 5.5, new InstanceType.InstanceTypeDetails());

    ObjectNode userIntentJson =
        Json.newObject()
            .put("universeName", u.name)
            .put("instanceType", i.getInstanceTypeCode())
            .put("replicationFactor", 3)
            .put("numNodes", 3)
            .put("provider", p.uuid.toString())
            .put("accessKeyCode", accessKeyCode);
    ArrayNode regionList = Json.newArray().add(r.uuid.toString());
    userIntentJson.set("regionList", regionList);

    ArrayNode nodeDetailsJsonArray = Json.newArray();
    u =
        Universe.saveDetails(
            u.universeUUID,
            universe -> {
              UniverseDefinitionTaskParams universeDetails = universe.getUniverseDetails();
              Cluster cluster = universeDetails.clusters.get(0);
              cluster.userIntent = Json.fromJson(userIntentJson, UserIntent.class);
              for (int idx = 0; idx < 3; idx++) {
                NodeDetails node = ApiUtils.getDummyNodeDetails(idx, NodeState.Live);
                node.placementUuid = cluster.uuid;
                nodeDetailsJsonArray.add(Json.toJson(node));
                universeDetails.nodeDetailsSet.add(node);
              }
            });
    UniverseDefinitionTaskParams universeDetails = u.getUniverseDetails();
    Cluster cluster = universeDetails.clusters.get(0);
    ObjectNode clusterJson = Json.newObject();
    clusterJson.set("userIntent", userIntentJson);
    clusterJson.set("uuid", Json.toJson(cluster.uuid));
    ArrayNode clustersJsonArray = Json.newArray().add(clusterJson);

    ObjectNode bodyJson = Json.newObject();
    bodyJson.set("clusters", clustersJsonArray);
    bodyJson.set("nodeDetailsSet", nodeDetailsJsonArray);

    String url = "/api/customers/" + customer.uuid + "/universes/" + u.universeUUID;
    Result result = doRequestWithAuthTokenAndBody("PUT", url, authToken, bodyJson);
    assertOk(result);
    JsonNode json = Json.parse(contentAsString(result));
    assertValue(json, "universeUUID", u.universeUUID.toString());
    assertNotNull(json.get("universeDetails"));

    CustomerTask th = CustomerTask.find.query().where().eq("task_uuid", fakeTaskUUID).findOne();
    assertNotNull(th);
    assertThat(th.getCustomerUUID(), allOf(notNullValue(), equalTo(customer.uuid)));
    assertThat(th.getTargetName(), allOf(notNullValue(), equalTo("Test Universe")));
    assertThat(th.getType(), allOf(notNullValue(), equalTo(CustomerTask.TaskType.Update)));
    assertAuditEntry(1, customer.uuid);
  }

  @Test
  public void testVMImageUpgradeWithUnsupportedProvider() {
    when(mockRuntimeConfig.getBoolean("yb.cloud.enabled")).thenReturn(true);

    UUID fakeTaskUUID = UUID.randomUUID();
    UUID uUUID = createUniverse(customer.getCustomerId()).universeUUID;
    Universe.saveDetails(uUUID, ApiUtils.mockUniverseUpdater());

    Provider p = ModelFactory.onpremProvider(customer);
    ObjectNode bodyJson = setupVMImageUpgradeParams(uUUID, p, "type.small");

    String url = "/api/customers/" + customer.uuid + "/universes/" + uUUID + "/upgrade";
    Result result =
        assertPlatformException(
            () -> doRequestWithAuthTokenAndBody("POST", url, authToken, bodyJson));

    assertBadRequest(result, "VM image upgrade is only supported for AWS / GCP");
    assertNull(CustomerTask.find.query().where().eq("task_uuid", fakeTaskUUID).findOne());
    assertAuditEntry(0, customer.uuid);
  }

  @Test
  public void testVMImageUpgradeWithEphemerals() {
    when(mockRuntimeConfig.getBoolean("yb.cloud.enabled")).thenReturn(true);

    UUID fakeTaskUUID = UUID.randomUUID();
    UUID uUUID = createUniverse(customer.getCustomerId()).universeUUID;
    Universe.saveDetails(uUUID, ApiUtils.mockUniverseUpdater());

    Provider p = ModelFactory.awsProvider(customer);
    ObjectNode bodyJson = setupVMImageUpgradeParams(uUUID, p, "i3.xlarge");

    String url = "/api/customers/" + customer.uuid + "/universes/" + uUUID + "/upgrade";
    Result result =
        assertPlatformException(
            () -> doRequestWithAuthTokenAndBody("POST", url, authToken, bodyJson));

    assertBadRequest(result, "Cannot upgrade a universe with ephemeral storage");
    assertNull(CustomerTask.find.query().where().eq("task_uuid", fakeTaskUUID).findOne());
    assertAuditEntry(0, customer.uuid);
  }

  @Test
  public void testVMImageUpgradeWithNoImage() {
    when(mockRuntimeConfig.getBoolean("yb.cloud.enabled")).thenReturn(true);

    UUID fakeTaskUUID = UUID.randomUUID();
    UUID uUUID = createUniverse(customer.getCustomerId()).universeUUID;
    Universe.saveDetails(uUUID, ApiUtils.mockUniverseUpdater());

    Provider p = ModelFactory.awsProvider(customer);
    ObjectNode bodyJson = setupVMImageUpgradeParams(uUUID, p, "c5.xlarge");

    String url = "/api/customers/" + customer.uuid + "/universes/" + uUUID + "/upgrade";
    Result result =
        assertPlatformException(
            () -> doRequestWithAuthTokenAndBody("POST", url, authToken, bodyJson));

    assertBadRequest(result, "machineImages param is required for taskType: VMImage");
    assertNull(CustomerTask.find.query().where().eq("task_uuid", fakeTaskUUID).findOne());
    assertAuditEntry(0, customer.uuid);
  }

  @Test
  public void testVMImageUpgradeValidParams() {
    when(mockRuntimeConfig.getBoolean("yb.cloud.enabled")).thenReturn(true);

    UUID fakeTaskUUID = UUID.randomUUID();
    when(mockCommissioner.submit(any(TaskType.class), any(UniverseDefinitionTaskParams.class)))
        .thenReturn(fakeTaskUUID);

    Universe u = createUniverse(customer.getCustomerId());
    Provider p = ModelFactory.awsProvider(customer);
    ObjectNode bodyJson = setupVMImageUpgradeParams(u.universeUUID, p, "c5.xlarge");
    ObjectNode images = Json.newObject();
    UUID r = UUID.randomUUID();
    images.put(r.toString(), "image-" + r.toString());
    bodyJson.set("machineImages", images);
    String url = "/api/customers/" + customer.uuid + "/universes/" + u.universeUUID + "/upgrade";
    Result result = doRequestWithAuthTokenAndBody("POST", url, authToken, bodyJson);

    verify(mockCommissioner).submit(eq(TaskType.UpgradeUniverse), any(UniverseTaskParams.class));

    assertOk(result);
    JsonNode json = Json.parse(contentAsString(result));
    assertValue(json, "taskUUID", fakeTaskUUID.toString());

    CustomerTask th = CustomerTask.find.query().where().eq("task_uuid", fakeTaskUUID).findOne();
    assertNotNull(th);
    assertThat(th.getCustomerUUID(), allOf(notNullValue(), equalTo(customer.uuid)));
    assertThat(th.getTargetName(), allOf(notNullValue(), equalTo("Test Universe")));
    assertThat(th.getType(), allOf(notNullValue(), equalTo(CustomerTask.TaskType.UpgradeVMImage)));
    assertAuditEntry(1, customer.uuid);
  }

  @Test
  public void testUniverseUpgradeWithEmptyParams() {
    UUID fakeTaskUUID = UUID.randomUUID();
    UUID uUUID = createUniverse(customer.getCustomerId()).universeUUID;
    Universe.saveDetails(uUUID, ApiUtils.mockUniverseUpdater());

    String url = "/api/customers/" + customer.uuid + "/universes/" + uUUID + "/upgrade";
    Result result =
        assertPlatformException(
            () -> doRequestWithAuthTokenAndBody("POST", url, authToken, Json.newObject()));
    assertBadRequest(result, "clusters: This field is required");
    assertNull(CustomerTask.find.query().where().eq("task_uuid", fakeTaskUUID).findOne());
    assertAuditEntry(0, customer.uuid);
  }

  @Test
  @Parameters({
    "Rolling",
    "Non-Rolling",
    "Non-Restart",
  })
  public void testUniverseUpgradeWithNodesInTransit(String upgradeOption) {
    UUID fakeTaskUUID = UUID.randomUUID();
    when(mockCommissioner.submit(any(TaskType.class), any(UniverseDefinitionTaskParams.class)))
        .thenReturn(fakeTaskUUID);
    UUID uUUID = createUniverse(customer.getCustomerId()).universeUUID;
    Universe.saveDetails(uUUID, ApiUtils.mockUniverseUpdater());

    setInTransitNode(uUUID);

    ObjectNode bodyJson = getValidPayload(uUUID, upgradeOption);
    String url = "/api/customers/" + customer.uuid + "/universes/" + uUUID + "/upgrade";
    if (upgradeOption.equals("Rolling")) {
      Result result =
          assertPlatformException(
              () -> doRequestWithAuthTokenAndBody("POST", url, authToken, bodyJson));
      assertBadRequest(result, "as it has nodes in one of");
      assertNull(CustomerTask.find.query().where().eq("task_uuid", fakeTaskUUID).findOne());
      assertAuditEntry(0, customer.uuid);
    } else {
      Result result = doRequestWithAuthTokenAndBody("POST", url, authToken, bodyJson);
      assertOk(result);
      JsonNode json = Json.parse(contentAsString(result));
      assertValue(json, "taskUUID", fakeTaskUUID.toString());
      assertAuditEntry(1, customer.uuid);
    }
  }

  ObjectNode getValidPayload(UUID univUUID, String upgradeOption) {
    ObjectNode bodyJson =
        Json.newObject()
            .put("universeUUID", univUUID.toString())
            .put("taskType", "Software")
            .put("upgradeOption", upgradeOption)
            .put("ybSoftwareVersion", "0.0.1");
    ObjectNode userIntentJson =
        Json.newObject()
            .put("universeName", "Single UserUniverse")
            .put("ybSoftwareVersion", "0.0.1");
    ArrayNode clustersJsonArray =
        Json.newArray().add(Json.newObject().set("userIntent", userIntentJson));
    bodyJson.set("clusters", clustersJsonArray);
    return bodyJson;
  }

  protected ObjectNode setupVMImageUpgradeParams(UUID uUUID, Provider p, String instanceType) {
    when(mockRuntimeConfig.getBoolean("yb.cloud.enabled")).thenReturn(true);

    ObjectNode bodyJson = getValidPayload(uUUID, "Rolling");
    bodyJson.put("taskType", UpgradeTaskType.VMImage.toString());
    InstanceType i =
        InstanceType.upsert(p.uuid, instanceType, 10, 5.5, new InstanceType.InstanceTypeDetails());
    ObjectNode userIntentJson =
        Json.newObject()
            .put("numNodes", 3)
            .put("instanceType", i.getInstanceTypeCode())
            .put("replicationFactor", 3)
            .put("providerType", p.code)
            .put("provider", p.uuid.toString());
    ArrayNode clustersJsonArray =
        Json.newArray().add(Json.newObject().set("userIntent", userIntentJson));
    bodyJson.set("clusters", clustersJsonArray);

    return bodyJson;
  }

  @Test
  public void testUniverseSoftwareUpgradeValidParams() {
    UUID fakeTaskUUID = UUID.randomUUID();
    when(mockCommissioner.submit(any(TaskType.class), any(UniverseDefinitionTaskParams.class)))
        .thenReturn(fakeTaskUUID);
    Universe u = createUniverse(customer.getCustomerId());

    ObjectNode bodyJson = getValidPayload(u.universeUUID, "Rolling");
    String url = "/api/customers/" + customer.uuid + "/universes/" + u.universeUUID + "/upgrade";
    Result result = doRequestWithAuthTokenAndBody("POST", url, authToken, bodyJson);

    verify(mockCommissioner).submit(eq(TaskType.UpgradeUniverse), any(UniverseTaskParams.class));

    assertOk(result);
    JsonNode json = Json.parse(contentAsString(result));
    assertValue(json, "taskUUID", fakeTaskUUID.toString());

    CustomerTask th = CustomerTask.find.query().where().eq("task_uuid", fakeTaskUUID).findOne();
    assertNotNull(th);
    assertThat(th.getCustomerUUID(), allOf(notNullValue(), equalTo(customer.uuid)));
    assertThat(th.getTargetName(), allOf(notNullValue(), equalTo("Test Universe")));
    assertThat(th.getType(), allOf(notNullValue(), equalTo(CustomerTask.TaskType.UpgradeSoftware)));
    assertAuditEntry(1, customer.uuid);
  }

  @Test
  public void testUniverseRollingRestartValidParams() {
    UUID fakeTaskUUID = UUID.randomUUID();
    when(mockCommissioner.submit(any(TaskType.class), any(UniverseDefinitionTaskParams.class)))
        .thenReturn(fakeTaskUUID);
    Universe u = createUniverse(customer.getCustomerId());

    ObjectNode bodyJson =
        Json.newObject()
            .put("universeUUID", u.universeUUID.toString())
            .put("taskType", "Restart")
            .put("upgradeOption", "Rolling");
    ObjectNode userIntentJson = Json.newObject().put("universeName", "Single UserUniverse");
    ArrayNode clustersJsonArray =
        Json.newArray().add(Json.newObject().set("userIntent", userIntentJson));
    bodyJson.set("clusters", clustersJsonArray);
    String url = "/api/customers/" + customer.uuid + "/universes/" + u.universeUUID + "/upgrade";
    Result result = doRequestWithAuthTokenAndBody("POST", url, authToken, bodyJson);

    verify(mockCommissioner).submit(eq(TaskType.UpgradeUniverse), any(UniverseTaskParams.class));

    assertOk(result);
    JsonNode json = Json.parse(contentAsString(result));
    assertValue(json, "taskUUID", fakeTaskUUID.toString());

    CustomerTask th = CustomerTask.find.query().where().eq("task_uuid", fakeTaskUUID).findOne();
    assertNotNull(th);
    assertThat(th.getCustomerUUID(), allOf(notNullValue(), equalTo(customer.uuid)));
    assertThat(th.getTargetName(), allOf(notNullValue(), equalTo("Test Universe")));
    assertThat(th.getType(), allOf(notNullValue(), equalTo(CustomerTask.TaskType.Restart)));
    assertAuditEntry(1, customer.uuid);
  }

  @Test
  public void testUniverseRollingRestartNonRolling() {
    UUID fakeTaskUUID = UUID.randomUUID();
    when(mockCommissioner.submit(any(TaskType.class), any(UniverseDefinitionTaskParams.class)))
        .thenReturn(fakeTaskUUID);
    Universe u = createUniverse(customer.getCustomerId());

    ObjectNode bodyJson =
        Json.newObject()
            .put("universeUUID", u.universeUUID.toString())
            .put("taskType", "Restart")
            .put("upgradeOption", "Non-Rolling");
    ObjectNode userIntentJson = Json.newObject().put("universeName", "Single UserUniverse");
    ArrayNode clustersJsonArray =
        Json.newArray().add(Json.newObject().set("userIntent", userIntentJson));
    bodyJson.set("clusters", clustersJsonArray);
    String url = "/api/customers/" + customer.uuid + "/universes/" + u.universeUUID + "/upgrade";
    Result result =
        assertPlatformException(
            () -> doRequestWithAuthTokenAndBody("POST", url, authToken, bodyJson));

    assertBadRequest(result, "Rolling restart has to be a ROLLING UPGRADE.");
    assertAuditEntry(0, customer.uuid);
  }

  @Test
  public void testUniverseSoftwareUpgradeWithInvalidParams() {
    Universe u = createUniverse(customer.getCustomerId());

    ObjectNode bodyJson =
        Json.newObject().put("universeUUID", u.universeUUID.toString()).put("taskType", "Software");
    ObjectNode userIntentJson = Json.newObject().put("universeName", "Single UserUniverse");
    ArrayNode clustersJsonArray =
        Json.newArray().add(Json.newObject().set("userIntent", userIntentJson));
    bodyJson.set("clusters", clustersJsonArray);

    String url = "/api/customers/" + customer.uuid + "/universes/" + u.universeUUID + "/upgrade";
    Result result =
        assertPlatformException(
            () -> doRequestWithAuthTokenAndBody("POST", url, authToken, bodyJson));

    assertBadRequest(result, "ybSoftwareVersion param is required for taskType: Software");
    assertAuditEntry(0, customer.uuid);
  }

  @Test
  public void testUniverseGFlagsUpgradeValidParams() {
    UUID fakeTaskUUID = UUID.randomUUID();
    when(mockCommissioner.submit(any(TaskType.class), any(UniverseDefinitionTaskParams.class)))
        .thenReturn(fakeTaskUUID);
    Universe u = createUniverse(customer.getCustomerId());

    ObjectNode bodyJson =
        Json.newObject().put("universeUUID", u.universeUUID.toString()).put("taskType", "GFlags");
    ObjectNode userIntentJson = Json.newObject().put("universeName", "Single UserUniverse");
    ArrayNode clustersJsonArray =
        Json.newArray().add(Json.newObject().set("userIntent", userIntentJson));
    bodyJson.set("clusters", clustersJsonArray);

    JsonNode masterGFlags = Json.parse("[{ \"name\": \"master-flag\", \"value\": \"123\"}]");
    JsonNode tserverGFlags = Json.parse("[{ \"name\": \"tserver-flag\", \"value\": \"456\"}]");
    userIntentJson.set("masterGFlags", masterGFlags);
    userIntentJson.set("tserverGFlags", tserverGFlags);

    String url = "/api/customers/" + customer.uuid + "/universes/" + u.universeUUID + "/upgrade";
    Result result = doRequestWithAuthTokenAndBody("POST", url, authToken, bodyJson);

    assertOk(result);
    JsonNode json = Json.parse(contentAsString(result));
    assertValue(json, "taskUUID", fakeTaskUUID.toString());
    verify(mockCommissioner).submit(eq(TaskType.UpgradeUniverse), any(UniverseTaskParams.class));

    CustomerTask th = CustomerTask.find.query().where().eq("task_uuid", fakeTaskUUID).findOne();
    assertNotNull(th);
    assertThat(th.getCustomerUUID(), allOf(notNullValue(), equalTo(customer.uuid)));
    assertThat(th.getTargetName(), allOf(notNullValue(), equalTo("Test Universe")));
    assertThat(th.getType(), allOf(notNullValue(), equalTo(CustomerTask.TaskType.UpgradeGflags)));
    assertAuditEntry(1, customer.uuid);
  }

  @Test
  public void testUniverseGFlagsUpgradeWithTrimParams() {
    UUID fakeTaskUUID = UUID.randomUUID();
    when(mockCommissioner.submit(any(TaskType.class), any(UniverseDefinitionTaskParams.class)))
        .thenReturn(fakeTaskUUID);
    Universe u = createUniverse(customer.getCustomerId());

    ObjectNode bodyJson =
        Json.newObject().put("universeUUID", u.universeUUID.toString()).put("taskType", "GFlags");
    ObjectNode userIntentJson = Json.newObject().put("universeName", "Single UserUniverse");
    ArrayNode clustersJsonArray =
        Json.newArray().add(Json.newObject().set("userIntent", userIntentJson));
    bodyJson.set("clusters", clustersJsonArray);

    JsonNode masterGFlags = Json.parse("[{ \"name\": \" master-flag \", \"value\": \" 123 \"}]");
    JsonNode tserverGFlags = Json.parse("[{ \"name\": \" tserver-flag \", \"value\": \" 456 \"}]");
    userIntentJson.set("masterGFlags", masterGFlags);
    userIntentJson.set("tserverGFlags", tserverGFlags);

    String url = "/api/customers/" + customer.uuid + "/universes/" + u.universeUUID + "/upgrade";
    Result result = doRequestWithAuthTokenAndBody("POST", url, authToken, bodyJson);

    assertOk(result);
    JsonNode json = Json.parse(contentAsString(result));
    assertValue(json, "taskUUID", fakeTaskUUID.toString());
    verify(mockCommissioner).submit(eq(TaskType.UpgradeUniverse), any(UniverseTaskParams.class));

    CustomerTask th = CustomerTask.find.query().where().eq("task_uuid", fakeTaskUUID).findOne();
    assertNotNull(th);
    assertThat(th.getCustomerUUID(), allOf(notNullValue(), equalTo(customer.uuid)));
    assertThat(th.getTargetName(), allOf(notNullValue(), equalTo("Test Universe")));
    assertThat(th.getType(), allOf(notNullValue(), equalTo(CustomerTask.TaskType.UpgradeGflags)));
    assertAuditEntry(1, customer.uuid);
  }

  @Test
  public void testUniverseGFlagsUpgradeWithInvalidParams() {
    Universe u = createUniverse(customer.getCustomerId());

    ObjectNode bodyJson =
        Json.newObject().put("universeUUID", u.universeUUID.toString()).put("taskType", "GFlags");
    ObjectNode userIntentJson = Json.newObject().put("universeName", "Test Universe");
    userIntentJson.set("masterGFlags", Json.parse("[\"gflag1\", \"123\"]"));
    ArrayNode clustersJsonArray =
        Json.newArray().add(Json.newObject().set("userIntent", userIntentJson));
    bodyJson.set("clusters", clustersJsonArray);

    String url = "/api/customers/" + customer.uuid + "/universes/" + u.universeUUID + "/upgrade";
    Result result =
        assertPlatformException(
            () -> doRequestWithAuthTokenAndBody("POST", url, authToken, bodyJson));

    assertBadRequest(result, "Neither master nor tserver gflags changed.");
    assertAuditEntry(0, customer.uuid);
  }

  @Test
  public void testUniverseGFlagsUpgradeWithSameGFlags() {
    Universe u = createUniverse(customer.getCustomerId());

    Universe.UniverseUpdater updater =
        universe -> {
          UniverseDefinitionTaskParams universeDetails = universe.getUniverseDetails();
          UniverseDefinitionTaskParams.UserIntent userIntent =
              universeDetails.getPrimaryCluster().userIntent;
          userIntent.masterGFlags = ImmutableMap.of("master-flag", "123");
          userIntent.tserverGFlags = ImmutableMap.of("tserver-flag", "456");
          universe.setUniverseDetails(universeDetails);
        };
    Universe.saveDetails(u.universeUUID, updater);

    ObjectNode bodyJson =
        Json.newObject()
            .put("universeUUID", u.universeUUID.toString())
            .put("taskType", "GFlags")
            .put("upgradeOption", "Non-Rolling");
    ObjectNode userIntentJson = Json.newObject().put("universeName", u.name);
    ArrayNode clustersJsonArray =
        Json.newArray().add(Json.newObject().set("userIntent", userIntentJson));
    bodyJson.set("clusters", clustersJsonArray);

    JsonNode masterGFlags = Json.parse("[{ \"name\": \"master-flag\", \"value\": \"123\"}]");
    JsonNode tserverGFlags = Json.parse("[{ \"name\": \"tserver-flag\", \"value\": \"456\"}]");
    userIntentJson.set("masterGFlags", masterGFlags);
    userIntentJson.set("tserverGFlags", tserverGFlags);

    String url = "/api/customers/" + customer.uuid + "/universes/" + u.universeUUID + "/upgrade";
    Result result =
        assertPlatformException(
            () -> doRequestWithAuthTokenAndBody("POST", url, authToken, bodyJson));

    assertBadRequest(result, "Neither master nor tserver gflags changed");
    assertAuditEntry(0, customer.uuid);
  }

  @Test
  public void testUniverseGFlagsUpgradeWithMissingGflags() {
    Universe u = createUniverse(customer.getCustomerId());

    ObjectNode bodyJsonMissingGFlags =
        Json.newObject().put("universeUUID", u.universeUUID.toString()).put("taskType", "GFlags");
    ObjectNode userIntentJson = Json.newObject().put("universeName", "Single UserUniverse");
    ArrayNode clustersJsonArray =
        Json.newArray().add(Json.newObject().set("userIntent", userIntentJson));
    bodyJsonMissingGFlags.set("clusters", clustersJsonArray);

    String url = "/api/customers/" + customer.uuid + "/universes/" + u.universeUUID + "/upgrade";
    Result result =
        assertPlatformException(
            () -> doRequestWithAuthTokenAndBody("POST", url, authToken, bodyJsonMissingGFlags));

    assertBadRequest(result, "Neither master nor tserver gflags changed.");
    assertAuditEntry(0, customer.uuid);
  }

  @Test
  public void testUniverseGFlagsUpgradeWithMalformedTServerFlags() {
    Universe u = createUniverse(customer.getCustomerId());

    ObjectNode bodyJson =
        Json.newObject().put("universeUUID", u.universeUUID.toString()).put("taskType", "GFlags");
    ObjectNode userIntentJson =
        Json.newObject().put("universeName", "Single UserUniverse").put("tserverGFlags", "abcd");
    ArrayNode clustersJsonArray =
        Json.newArray().add(Json.newObject().set("userIntent", userIntentJson));
    bodyJson.set("clusters", clustersJsonArray);

    String url = "/api/customers/" + customer.uuid + "/universes/" + u.universeUUID + "/upgrade";
    Result result =
        assertPlatformException(
            () -> doRequestWithAuthTokenAndBody("POST", url, authToken, bodyJson));

    assertBadRequest(result, "Neither master nor tserver gflags changed.");
    assertAuditEntry(0, customer.uuid);
  }

  @Test
  public void testUniverseGFlagsUpgradeWithMalformedMasterGFlags() {
    Universe u = createUniverse(customer.getCustomerId());

    ObjectNode bodyJson =
        Json.newObject().put("universeUUID", u.universeUUID.toString()).put("taskType", "GFlags");
    ObjectNode userIntentJson =
        Json.newObject().put("universeName", "Single UserUniverse").put("masterGFlags", "abcd");
    ArrayNode clustersJsonArray =
        Json.newArray().add(Json.newObject().set("userIntent", userIntentJson));
    bodyJson.set("clusters", clustersJsonArray);

    String url = "/api/customers/" + customer.uuid + "/universes/" + u.universeUUID + "/upgrade";
    Result result =
        assertPlatformException(
            () -> doRequestWithAuthTokenAndBody("POST", url, authToken, bodyJson));

    assertBadRequest(result, "Neither master nor tserver gflags changed.");
    assertAuditEntry(0, customer.uuid);
  }

  @Test
  public void testUniverseNonRollingGFlagsUpgrade() {
    UUID fakeTaskUUID = UUID.randomUUID();
    when(mockCommissioner.submit(any(TaskType.class), any(UniverseDefinitionTaskParams.class)))
        .thenReturn(fakeTaskUUID);
    Universe u = createUniverse(customer.getCustomerId());

    ObjectNode bodyJson =
        Json.newObject()
            .put("universeUUID", u.universeUUID.toString())
            .put("taskType", "GFlags")
            .put("upgradeOption", "Non-Rolling");
    ObjectNode userIntentJson = Json.newObject().put("universeName", "Single UserUniverse");
    ArrayNode clustersJsonArray =
        Json.newArray().add(Json.newObject().set("userIntent", userIntentJson));
    bodyJson.set("clusters", clustersJsonArray);

    JsonNode masterGFlags = Json.parse("[{ \"name\": \"master-flag\", \"value\": \"123\"}]");
    JsonNode tserverGFlags = Json.parse("[{ \"name\": \"tserver-flag\", \"value\": \"456\"}]");
    userIntentJson.set("masterGFlags", masterGFlags);
    userIntentJson.set("tserverGFlags", tserverGFlags);

    String url = "/api/customers/" + customer.uuid + "/universes/" + u.universeUUID + "/upgrade";
    Result result = doRequestWithAuthTokenAndBody("POST", url, authToken, bodyJson);

    ArgumentCaptor<UniverseTaskParams> taskParams =
        ArgumentCaptor.forClass(UniverseTaskParams.class);
    assertOk(result);
    JsonNode json = Json.parse(contentAsString(result));
    assertValue(json, "taskUUID", fakeTaskUUID.toString());
    verify(mockCommissioner).submit(eq(TaskType.UpgradeUniverse), taskParams.capture());
    UpgradeParams taskParam = (UpgradeParams) taskParams.getValue();
    assertEquals(taskParam.upgradeOption, UpgradeParams.UpgradeOption.NON_ROLLING_UPGRADE);
    assertEquals(taskParam.masterGFlags, ImmutableMap.of("master-flag", "123"));
    assertEquals(taskParam.tserverGFlags, ImmutableMap.of("tserver-flag", "456"));
    UniverseDefinitionTaskParams.UserIntent primaryClusterIntent =
        taskParam.getPrimaryCluster().userIntent;
    assertEquals(primaryClusterIntent.masterGFlags, taskParam.masterGFlags);
    assertEquals(primaryClusterIntent.tserverGFlags, taskParam.tserverGFlags);
    assertAuditEntry(1, customer.uuid);
  }

  @Test
  public void testUniverseNonRollingSoftwareUpgrade() {
    UUID fakeTaskUUID = UUID.randomUUID();
    when(mockCommissioner.submit(any(TaskType.class), any(UniverseDefinitionTaskParams.class)))
        .thenReturn(fakeTaskUUID);
    Universe u = createUniverse(customer.getCustomerId());

    ObjectNode bodyJson =
        Json.newObject()
            .put("universeUUID", u.universeUUID.toString())
            .put("taskType", "Software")
            .put("upgradeOption", "Non-Rolling")
            .put("ybSoftwareVersion", "new-version");
    ObjectNode userIntentJson =
        Json.newObject()
            .put("universeName", "Single UserUniverse")
            .put("ybSoftwareVersion", "new-version");
    ArrayNode clustersJsonArray =
        Json.newArray().add(Json.newObject().set("userIntent", userIntentJson));
    bodyJson.set("clusters", clustersJsonArray);

    String url = "/api/customers/" + customer.uuid + "/universes/" + u.universeUUID + "/upgrade";
    Result result = doRequestWithAuthTokenAndBody("POST", url, authToken, bodyJson);

    ArgumentCaptor<UniverseTaskParams> taskParams =
        ArgumentCaptor.forClass(UniverseTaskParams.class);
    assertOk(result);
    JsonNode json = Json.parse(contentAsString(result));
    assertValue(json, "taskUUID", fakeTaskUUID.toString());
    verify(mockCommissioner).submit(eq(TaskType.UpgradeUniverse), taskParams.capture());
    UpgradeParams taskParam = (UpgradeParams) taskParams.getValue();
    assertEquals(taskParam.upgradeOption, UpgradeParams.UpgradeOption.NON_ROLLING_UPGRADE);
    assertEquals("new-version", taskParam.ybSoftwareVersion);
    assertAuditEntry(1, customer.uuid);
  }

  private ObjectNode getResizeNodeValidPayload(Universe u, Provider p) {
    UUID fakeClusterUUID = UUID.randomUUID();
    ObjectNode bodyJson =
        Json.newObject()
            .put("universeUUID", u.universeUUID.toString())
            .put("taskType", UpgradeTaskType.ResizeNode.toString())
            .put("upgradeOption", "Rolling");

    ObjectNode deviceInfoJson = Json.newObject().put("volumeSize", 600);

    ObjectNode userIntentJson = Json.newObject().put("instanceType", "test-instance-type");
    userIntentJson.set("deviceInfo", deviceInfoJson);

    if (p != null) {
      userIntentJson.put("providerType", p.code).put("provider", p.uuid.toString());
    }

    ObjectNode primaryCluster =
        Json.newObject().put("uuid", fakeClusterUUID.toString()).put("clusterType", "PRIMARY");
    primaryCluster.set("userIntent", userIntentJson);

    ArrayNode clustersJsonArray = Json.newArray().add(primaryCluster);
    bodyJson.set("clusters", clustersJsonArray);

    return bodyJson;
  }

  @Test
  public void testResizeNodeWithUnsupportedProvider() {
    when(mockRuntimeConfig.getBoolean("yb.cloud.enabled")).thenReturn(true);

    UUID fakeTaskUUID = UUID.randomUUID();
    when(mockCommissioner.submit(any(TaskType.class), any(UniverseDefinitionTaskParams.class)))
        .thenReturn(fakeTaskUUID);

    Universe u = createUniverse(customer.getCustomerId());

    Provider p = ModelFactory.newProvider(customer, Common.CloudType.other);
    Universe.UniverseUpdater updater =
        universe -> {
          UniverseDefinitionTaskParams universeDetails = universe.getUniverseDetails();
          UniverseDefinitionTaskParams.UserIntent userIntent =
              universeDetails.getPrimaryCluster().userIntent;
          userIntent.providerType = Common.CloudType.other;
          userIntent.provider = p.uuid.toString();
          universe.setUniverseDetails(universeDetails);
        };
    Universe.saveDetails(u.universeUUID, updater);

    ObjectNode bodyJson = getResizeNodeValidPayload(u, p);

    String url = "/api/customers/" + customer.uuid + "/universes/" + u.universeUUID + "/upgrade";
    Result result =
        assertThrows(
                PlatformServiceException.class,
                () -> doRequestWithAuthTokenAndBody("POST", url, authToken, bodyJson))
            .getResult();

    assertBadRequest(result, "Smart resizing is only supported for AWS / GCP, It is: " + p.code);

    assertNull(CustomerTask.find.query().where().eq("task_uuid", fakeTaskUUID).findOne());
    assertAuditEntry(0, customer.uuid);
  }

  @Test
  public void testResizeNodeValidParams() {
    when(mockRuntimeConfig.getBoolean("yb.cloud.enabled")).thenReturn(true);

    UUID fakeTaskUUID = UUID.randomUUID();
    when(mockCommissioner.submit(any(TaskType.class), any(UniverseDefinitionTaskParams.class)))
        .thenReturn(fakeTaskUUID);

    Universe u = createUniverse(customer.getCustomerId());

    ObjectNode bodyJson = getResizeNodeValidPayload(u, null);

    String url = "/api/customers/" + customer.uuid + "/universes/" + u.universeUUID + "/upgrade";
    Result result = doRequestWithAuthTokenAndBody("POST", url, authToken, bodyJson);

    verify(mockCommissioner).submit(eq(TaskType.UpgradeUniverse), any(UniverseTaskParams.class));

    assertOk(result);
    JsonNode json = Json.parse(contentAsString(result));
    assertValue(json, "taskUUID", fakeTaskUUID.toString());

    CustomerTask th = CustomerTask.find.query().where().eq("task_uuid", fakeTaskUUID).findOne();
    assertNotNull(th);
    assertThat(th.getCustomerUUID(), allOf(notNullValue(), equalTo(customer.uuid)));
    assertThat(th.getTargetName(), allOf(notNullValue(), equalTo("Test Universe")));
    assertThat(th.getType(), allOf(notNullValue(), equalTo(CustomerTask.TaskType.ResizeNode)));
    assertAuditEntry(1, customer.uuid);
  }

  @Test
  public void testExpandDiskSizeFailureInvalidSize() {
    Universe u = createUniverse(customer.getCustomerId());
    setupDiskUpdateTest(100, "c4.xlarge", PublicCloudConstants.StorageType.GP2, u);
    u = Universe.getOrBadRequest(u.universeUUID);

    ObjectNode bodyJson = (ObjectNode) Json.toJson(u.getUniverseDetails());
    bodyJson.put("size", 50);

    String url =
        "/api/customers/" + customer.uuid + "/universes/" + u.universeUUID + "/disk_update";
    Result result =
        assertPlatformException(
            () -> doRequestWithAuthTokenAndBody("POST", url, authToken, bodyJson));
    assertBadRequest(result, "Size can only be increased.");
  }

  @Test
  public void testExpandDiskSizeFailureInvalidStorage() {
    Universe u = createUniverse("Test universe", customer.getCustomerId(), Common.CloudType.gcp);
    setupDiskUpdateTest(100, "c4.xlarge", PublicCloudConstants.StorageType.Scratch, u);
    u = Universe.getOrBadRequest(u.universeUUID);

    ObjectNode bodyJson = (ObjectNode) Json.toJson(u.getUniverseDetails());
    bodyJson.put("size", 150);

    String url =
        "/api/customers/" + customer.uuid + "/universes/" + u.universeUUID + "/disk_update";
    Result result =
        assertPlatformException(
            () -> doRequestWithAuthTokenAndBody("POST", url, authToken, bodyJson));
    assertBadRequest(result, "Cannot modify instance volumes.");
  }

  @Test
  public void testExpandDiskSizeFailureInvalidInstance() {
    Universe u = createUniverse(customer.getCustomerId());
    setupDiskUpdateTest(100, "i3.xlarge", PublicCloudConstants.StorageType.GP2, u);
    u = Universe.getOrBadRequest(u.universeUUID);

    ObjectNode bodyJson = (ObjectNode) Json.toJson(u.getUniverseDetails());
    bodyJson.put("size", 150);

    String url =
        "/api/customers/" + customer.uuid + "/universes/" + u.universeUUID + "/disk_update";
    Result result =
        assertPlatformException(
            () -> doRequestWithAuthTokenAndBody("POST", url, authToken, bodyJson));
    assertBadRequest(result, "Cannot modify instance volumes.");
  }

  @Test
  public void testExpandDiskSizeSuccess() {
    UUID fakeTaskUUID = UUID.randomUUID();
    when(mockCommissioner.submit(any(TaskType.class), any(UniverseDefinitionTaskParams.class)))
        .thenReturn(fakeTaskUUID);
    Universe u = createUniverse(customer.getCustomerId());
    setupDiskUpdateTest(100, "c4.xlarge", PublicCloudConstants.StorageType.GP2, u);
    u = Universe.getOrBadRequest(u.universeUUID);

    ObjectNode bodyJson = (ObjectNode) Json.toJson(u.getUniverseDetails());
    bodyJson.put("size", 150);

    String url =
        "/api/customers/" + customer.uuid + "/universes/" + u.universeUUID + "/disk_update";
    Result result = doRequestWithAuthTokenAndBody("POST", url, authToken, bodyJson);
    assertOk(result);
    ArgumentCaptor<UniverseTaskParams> argCaptor =
        ArgumentCaptor.forClass(UniverseTaskParams.class);
    verify(mockCommissioner).submit(eq(TaskType.UpdateDiskSize), argCaptor.capture());
    assertAuditEntry(1, customer.uuid);
  }

  @Test
  public void testGetUpdateOptionsCreate() throws IOException {
    testGetAvailableOptions(x -> {}, CREATE, UniverseDefinitionTaskParams.UpdateOptions.UPDATE);
  }

  @Test
  public void testGetUpdateOptionsEditEmpty() throws IOException {
    testGetAvailableOptions(x -> {}, EDIT);
  }

  @Test
  public void testGetUpdateOptionsEditModifyTags() throws IOException {
    testGetAvailableOptions(
        u -> u.getPrimaryCluster().userIntent.instanceTags.put("aa", "bb"),
        EDIT,
        UniverseDefinitionTaskParams.UpdateOptions.UPDATE);
  }

  @Test
  public void testGetUpdateOptionsEditAddNode() throws IOException {
    testGetAvailableOptions(
        x -> {
          NodeDetails node = new NodeDetails();
          node.state = NodeState.ToBeAdded;
          node.placementUuid = x.nodeDetailsSet.iterator().next().placementUuid;
          x.nodeDetailsSet.add(node);
        },
        EDIT,
        UniverseDefinitionTaskParams.UpdateOptions.UPDATE);
  }

  @Test
  public void testGetUpdateOptionsEditSmartResizeNonRestart() throws IOException {
    testGetAvailableOptions(
        x -> x.getPrimaryCluster().userIntent.deviceInfo.volumeSize += 50,
        EDIT,
        UniverseDefinitionTaskParams.UpdateOptions.SMART_RESIZE_NON_RESTART);
  }

  @Test
  public void testGetUpdateOptionsEditSmartResizeAndFullMove() throws IOException {
    testGetAvailableOptions(
        u -> {
          List<NodeDetails> nodesToAdd = new ArrayList<>();
          u.nodeDetailsSet.forEach(
              node -> {
                node.state = NodeState.ToBeRemoved;
                NodeDetails newNode = new NodeDetails();
                newNode.state = NodeState.ToBeAdded;
                newNode.placementUuid = node.placementUuid;
                nodesToAdd.add(newNode);
              });
          u.nodeDetailsSet.addAll(nodesToAdd);
          u.getPrimaryCluster().userIntent.deviceInfo.volumeSize += 50;
          u.getPrimaryCluster().userIntent.instanceType = "c3.large";
        },
        EDIT,
        UniverseDefinitionTaskParams.UpdateOptions.SMART_RESIZE,
        UniverseDefinitionTaskParams.UpdateOptions.FULL_MOVE);
  }

  @Test
  public void testGetUpdateOptionsEditFullMove() throws IOException {
    testGetAvailableOptions(
        u -> {
          List<NodeDetails> nodesToAdd = new ArrayList<>();
          u.nodeDetailsSet.forEach(
              node -> {
                node.state = NodeState.ToBeRemoved;
                NodeDetails newNode = new NodeDetails();
                newNode.state = NodeState.ToBeAdded;
                newNode.placementUuid = node.placementUuid;
                nodesToAdd.add(newNode);
              });
          u.nodeDetailsSet.addAll(nodesToAdd);
          u.getPrimaryCluster().userIntent.deviceInfo.volumeSize += 50;
          // change placement => SR is not available.
          PlacementInfo.PlacementAZ az =
              u.getPrimaryCluster().placementInfo.azStream().findFirst().get();
          az.isAffinitized = !az.isAffinitized;
        },
        EDIT,
        UniverseDefinitionTaskParams.UpdateOptions.FULL_MOVE);
  }

  private void testGetAvailableOptions(
      Consumer<UniverseDefinitionTaskParams> mutator,
      UniverseConfigureTaskParams.ClusterOperationType operationType,
      UniverseDefinitionTaskParams.UpdateOptions... expectedOptions)
      throws IOException {
    Universe u = createUniverse(customer.getCustomerId());
    Provider provider = awsProvider(customer);
    Region region = Region.create(provider, "test-region", "Region 1", "yb-image-1");
    AvailabilityZone.createOrThrow(region, "az-1", "az-1", "subnet-1");
    // create default universe
    UserIntent userIntent = new UserIntent();
    userIntent.numNodes = 3;
    userIntent.provider = provider.uuid.toString();
    userIntent.providerType = Common.CloudType.valueOf(provider.code);
    userIntent.ybSoftwareVersion = "yb-version";
    userIntent.accessKeyCode = "demo-access";
    userIntent.instanceType = "c5.large";
    userIntent.replicationFactor = 3;
    userIntent.regionList = ImmutableList.of(region.uuid);
    userIntent.deviceInfo = new DeviceInfo();
    userIntent.deviceInfo.volumeSize = 100;
    Universe.saveDetails(u.universeUUID, ApiUtils.mockUniverseUpdater(userIntent, true));
    u = Universe.getOrBadRequest(u.universeUUID);
    UniverseDefinitionTaskParams udtp = u.getUniverseDetails();
    mutator.accept(udtp);
    udtp.universeUUID = u.universeUUID;

    ObjectNode bodyJson = (ObjectNode) Json.toJson(udtp);
    if (udtp.getPrimaryCluster().userIntent.instanceTags.size() > 0) {
      ArrayNode clusters = (ArrayNode) bodyJson.get("clusters");
      ObjectNode cluster = (ObjectNode) clusters.get(0);
      ArrayNode instanceTags = Json.newArray();
      udtp.getPrimaryCluster()
          .userIntent
          .instanceTags
          .entrySet()
          .forEach(
              entry -> {
                instanceTags.add(
                    Json.newObject().put("name", entry.getKey()).put("value", entry.getValue()));
              });
      ObjectNode userIntentNode = (ObjectNode) cluster.get("userIntent");
      userIntentNode.replace("instanceTags", instanceTags);
    }
    InstanceType.upsert(
        provider.uuid,
        udtp.getPrimaryCluster().userIntent.instanceType,
        10,
        5.5,
        new InstanceType.InstanceTypeDetails());
    bodyJson.put("currentClusterType", "PRIMARY");
    bodyJson.put("clusterOperation", operationType.name());

    Result result =
        doRequestWithAuthTokenAndBody(
            "POST",
            "/api/customers/" + customer.uuid + "/universe_update_options",
            authToken,
            bodyJson);
    assertEquals(
        Arrays.asList(expectedOptions).stream().map(o -> o.name()).collect(Collectors.toSet()),
        new HashSet<>(parseArrayOfStrings(result)));
  }

  private List<String> parseArrayOfStrings(Result result) throws IOException {
    ObjectMapper mapper = new ObjectMapper();
    String[] strings =
        mapper.readValue(
            contentAsString(result),
            TypeFactory.defaultInstance().constructArrayType(String.class));
    return Arrays.asList(strings);
  }

  private void setupDiskUpdateTest(
      int diskSize, String instanceType, PublicCloudConstants.StorageType storageType, Universe u) {

    Universe.UniverseUpdater updater =
        universe -> {
          UniverseDefinitionTaskParams universeDetails = new UniverseDefinitionTaskParams();
          UniverseDefinitionTaskParams.UserIntent userIntent =
              new UniverseDefinitionTaskParams.UserIntent();
          userIntent.instanceType = instanceType;
          userIntent.providerType = storageType.getCloudType();
          DeviceInfo di = new DeviceInfo();
          di.volumeSize = diskSize;
          di.numVolumes = 2;
          di.storageType = storageType;
          userIntent.deviceInfo = di;
          universeDetails.upsertPrimaryCluster(userIntent, null);
          universe.setUniverseDetails(universeDetails);
        };
    // Save the updates to the universe.
    Universe.saveDetails(u.universeUUID, updater);
  }
}
