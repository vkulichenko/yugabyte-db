// Copyright (c) YugaByte, Inc.

package com.yugabyte.yw.common;

import static com.yugabyte.yw.models.helpers.CommonUtils.nowMinusWithoutMillis;
import static com.yugabyte.yw.models.helpers.CommonUtils.nowPlusWithoutMillis;
import static com.yugabyte.yw.models.helpers.CommonUtils.nowWithoutMillis;

import com.fasterxml.jackson.databind.JsonNode;
import com.fasterxml.jackson.databind.node.ObjectNode;
import com.google.common.collect.ImmutableMap;
import com.google.common.collect.ImmutableSet;
import com.yugabyte.yw.commissioner.Common;
import com.yugabyte.yw.commissioner.Common.CloudType;
import com.yugabyte.yw.common.TableSpaceStructures.PlacementBlock;
import com.yugabyte.yw.common.TableSpaceStructures.TableSpaceInfo;
import com.yugabyte.yw.common.alerts.AlertChannelEmailParams;
import com.yugabyte.yw.common.alerts.AlertChannelParams;
import com.yugabyte.yw.common.alerts.AlertChannelSlackParams;
import com.yugabyte.yw.common.certmgmt.CertConfigType;
import com.yugabyte.yw.common.kms.EncryptionAtRestManager;
import com.yugabyte.yw.common.kms.services.EncryptionAtRestService;
import com.yugabyte.yw.common.metrics.MetricLabelsBuilder;
import com.yugabyte.yw.forms.AlertingData;
import com.yugabyte.yw.forms.BackupTableParams;
import com.yugabyte.yw.forms.CreateTablespaceParams;
import com.yugabyte.yw.forms.UniverseDefinitionTaskParams;
import com.yugabyte.yw.forms.UniverseDefinitionTaskParams.Cluster;
import com.yugabyte.yw.forms.UniverseDefinitionTaskParams.UserIntent;
import com.yugabyte.yw.forms.filters.AlertConfigurationApiFilter;
import com.yugabyte.yw.models.Alert;
import com.yugabyte.yw.models.AlertChannel;
import com.yugabyte.yw.models.AlertConfiguration;
import com.yugabyte.yw.models.AlertConfigurationTarget;
import com.yugabyte.yw.models.AlertConfigurationThreshold;
import com.yugabyte.yw.models.AlertDefinition;
import com.yugabyte.yw.models.AlertDestination;
import com.yugabyte.yw.models.AlertLabel;
import com.yugabyte.yw.models.AvailabilityZone;
import com.yugabyte.yw.models.Backup;
import com.yugabyte.yw.models.CertificateInfo;
import com.yugabyte.yw.models.Customer;
import com.yugabyte.yw.models.KmsConfig;
import com.yugabyte.yw.models.MaintenanceWindow;
import com.yugabyte.yw.models.Provider;
import com.yugabyte.yw.models.Region;
import com.yugabyte.yw.models.Schedule;
import com.yugabyte.yw.models.Universe;
import com.yugabyte.yw.models.Universe.UniverseUpdater;
import com.yugabyte.yw.models.Users;
import com.yugabyte.yw.models.Users.Role;
import com.yugabyte.yw.models.common.Condition;
import com.yugabyte.yw.models.common.Unit;
import com.yugabyte.yw.models.configs.CustomerConfig;
import com.yugabyte.yw.models.helpers.NodeDetails;
import com.yugabyte.yw.models.helpers.PlacementInfo;
import com.yugabyte.yw.models.helpers.PlacementInfo.PlacementAZ;
import com.yugabyte.yw.models.helpers.PlacementInfo.PlacementCloud;
import com.yugabyte.yw.models.helpers.PlacementInfo.PlacementRegion;
import com.yugabyte.yw.models.helpers.TaskType;
import java.io.IOException;
import java.security.NoSuchAlgorithmException;
import java.time.temporal.ChronoUnit;
import java.util.ArrayList;
import java.util.Collections;
import java.util.Date;
import java.util.HashSet;
import java.util.List;
import java.util.Map;
import java.util.Optional;
import java.util.Set;
import java.util.UUID;
import java.util.function.Consumer;
import java.util.stream.Collectors;
import net.logstash.logback.encoder.org.apache.commons.lang3.StringUtils;
import play.libs.Json;

public class ModelFactory {

  /*
   * Customer creation helpers.
   */

  public static Customer testCustomer() {
    return testCustomer("test@customer.com");
  }

  public static Customer testCustomer(String name) {
    return testCustomer("tc", name);
  }

  public static Customer testCustomer(String code, String name) {
    return Customer.create(code, name);
  }

  /*
   * Users creation helpers.
   */
  public static Users testUser(Customer customer) {
    return testUser(customer, "test@customer.com");
  }

  public static Users testUser(Customer customer, String email) {
    return testUser(customer, email, Role.Admin);
  }

  public static Users testUser(Customer customer, Role role) {
    return testUser(customer, "test@customer.com", role);
  }

  public static Users testUser(Customer customer, String email, Role role) {
    return Users.create(email, "password", role, customer.uuid, false);
  }

  /*
   * Provider creation helpers.
   */

  public static Provider awsProvider(Customer customer) {
    return Provider.create(customer.uuid, Common.CloudType.aws, "Amazon");
  }

  public static Provider gcpProvider(Customer customer) {
    return Provider.create(customer.uuid, Common.CloudType.gcp, "Google");
  }

  public static Provider azuProvider(Customer customer) {
    return Provider.create(customer.uuid, Common.CloudType.azu, "Azure");
  }

  public static Provider onpremProvider(Customer customer) {
    return Provider.create(customer.uuid, Common.CloudType.onprem, "OnPrem");
  }

  public static Provider kubernetesProvider(Customer customer) {
    return Provider.create(customer.uuid, Common.CloudType.kubernetes, "Kubernetes");
  }

  public static Provider newProvider(Customer customer, Common.CloudType cloud) {
    return Provider.create(customer.uuid, cloud, cloud.toString());
  }

  public static Provider newProvider(Customer customer, Common.CloudType cloud, String name) {
    return Provider.create(customer.uuid, cloud, name);
  }

  public static Provider newProvider(
      Customer customer, Common.CloudType cloud, Map<String, String> config) {
    return Provider.create(customer.uuid, cloud, cloud.toString(), config);
  }

  /*
   * Universe creation helpers.
   */

  public static Universe createUniverse() {
    return createUniverse("Test Universe", 1L);
  }

  public static Universe createUniverse(String universeName) {
    return createUniverse(universeName, 1L);
  }

  public static Universe createUniverse(long customerId) {
    return createUniverse("Test Universe", customerId);
  }

  public static Universe createUniverse(long customerId, UUID rootCA) {
    return createUniverse(
        "Test Universe", UUID.randomUUID(), customerId, Common.CloudType.aws, null, rootCA);
  }

  public static Universe createUniverse(String universeName, long customerId) {
    return createUniverse(universeName, UUID.randomUUID(), customerId, Common.CloudType.aws);
  }

  public static Universe createUniverse(String universeName, UUID universeUUID) {
    return createUniverse(universeName, universeUUID, 1L, Common.CloudType.aws);
  }

  public static Universe createUniverse(
      String universeName, long customerId, Common.CloudType cloudType) {
    return createUniverse(universeName, UUID.randomUUID(), customerId, cloudType);
  }

  public static Universe createUniverse(
      String universeName, UUID universeUUID, long customerId, Common.CloudType cloudType) {
    return createUniverse(universeName, universeUUID, customerId, cloudType, null);
  }

  public static Universe createUniverse(
      String universeName,
      UUID universeUUID,
      long customerId,
      Common.CloudType cloudType,
      PlacementInfo pi) {
    return createUniverse(universeName, universeUUID, customerId, cloudType, pi, null);
  }

  public static Universe createUniverse(
      String universeName,
      UUID universeUUID,
      long customerId,
      Common.CloudType cloudType,
      PlacementInfo pi,
      UUID rootCA) {
    return createUniverse(universeName, universeUUID, customerId, cloudType, pi, rootCA, false);
  }

  public static Universe createUniverse(
      String universeName,
      UUID universeUUID,
      long customerId,
      Common.CloudType cloudType,
      PlacementInfo pi,
      UUID rootCA,
      boolean enableYbc) {
    Customer c = Customer.get(customerId);
    // Custom setup a default AWS provider, can be overridden later.
    List<Provider> providerList = Provider.get(c.uuid, cloudType);
    Provider p = providerList.isEmpty() ? newProvider(c, cloudType) : providerList.get(0);

    UniverseDefinitionTaskParams.UserIntent userIntent =
        new UniverseDefinitionTaskParams.UserIntent();
    userIntent.universeName = universeName;
    userIntent.provider = p.uuid.toString();
    userIntent.providerType = cloudType;
    UniverseDefinitionTaskParams params = new UniverseDefinitionTaskParams();
    params.universeUUID = universeUUID;
    params.nodeDetailsSet = new HashSet<>();
    params.nodePrefix = universeName;
    params.rootCA = rootCA;
    params.enableYbc = enableYbc;
    params.upsertPrimaryCluster(userIntent, pi);
    return Universe.create(params, customerId);
  }

  public static CustomerConfig createS3StorageConfig(Customer customer, String configName) {
    JsonNode formData = getS3ConfigFormData(configName);
    return CustomerConfig.createWithFormData(customer.uuid, formData);
  }

  public static JsonNode getS3ConfigFormData(String configName) {
    return Json.parse(
        "{\"configName\": \""
            + configName
            + "\", \"name\": \"S3\","
            + " \"type\": \"STORAGE\", \"data\": {\"BACKUP_LOCATION\": \"s3://foo\","
            + " \"AWS_ACCESS_KEY_ID\": \"A-KEY\", \"AWS_SECRET_ACCESS_KEY\": \"A-SECRET\"}}");
  }

  public static CustomerConfig createS3StorageConfigWithIAM(Customer customer, String configName) {
    JsonNode formData =
        Json.parse(
            "{\"configName\": \""
                + configName
                + "\", \"name\": \"S3\","
                + " \"type\": \"STORAGE\", \"data\": {\"BACKUP_LOCATION\": \"s3://foo\","
                + " \"IAM_INSTANCE_PROFILE\": \"true\"}}");
    return CustomerConfig.createWithFormData(customer.uuid, formData);
  }

  public static CustomerConfig createNfsStorageConfig(Customer customer, String configName) {
    JsonNode formData =
        Json.parse(
            "{\"configName\": \""
                + configName
                + "\", \"name\": \"NFS\","
                + " \"type\": \"STORAGE\", \"data\": {\"BACKUP_LOCATION\": \"/foo/bar\"}}");
    return CustomerConfig.createWithFormData(customer.uuid, formData);
  }

  public static CustomerConfig createGcsStorageConfig(Customer customer, String configName) {
    JsonNode formData =
        Json.parse(
            "{\"configName\": \""
                + configName
                + "\", \"name\": \"GCS\","
                + " \"type\": \"STORAGE\", \"data\": {\"BACKUP_LOCATION\": \"gs://foo\","
                + " \"GCS_CREDENTIALS_JSON\": \"G-CREDS\"}}");
    return CustomerConfig.createWithFormData(customer.uuid, formData);
  }

  public static CustomerConfig createAZStorageConfig(Customer customer, String configName) {
    JsonNode formData =
        Json.parse(
            "{\"configName\": \""
                + configName
                + "\", \"name\": \"AZ\","
                + " \"type\": \"STORAGE\","
                + " \"data\":"
                + " {\"BACKUP_LOCATION\": \"https://foo.blob.core.windows.net/azurecontainer\","
                + " \"AZURE_STORAGE_SAS_TOKEN\": \"AZ-TOKEN\"}}");
    return CustomerConfig.createWithFormData(customer.uuid, formData);
  }

  public static Backup createBackup(UUID customerUUID, UUID universeUUID, UUID configUUID) {
    BackupTableParams params = new BackupTableParams();
    params.storageConfigUUID = configUUID;
    params.universeUUID = universeUUID;
    params.setKeyspace("foo");
    params.setTableName("bar");
    params.tableUUID = UUID.randomUUID();
    params.actionType = BackupTableParams.ActionType.CREATE;
    return Backup.create(customerUUID, params);
  }

  public static Backup restoreBackup(UUID customerUUID, UUID universeUUID, UUID configUUID) {
    BackupTableParams params = new BackupTableParams();
    params.storageConfigUUID = configUUID;
    params.universeUUID = universeUUID;
    params.setKeyspace("foo");
    params.setTableName("bar");
    params.tableUUID = UUID.randomUUID();
    params.actionType = BackupTableParams.ActionType.RESTORE;
    return Backup.create(customerUUID, params);
  }

  public static Backup createExpiredBackupWithScheduleUUID(
      UUID customerUUID, UUID universeUUID, UUID configUUID, UUID scheduleUUID) {
    BackupTableParams params = new BackupTableParams();
    params.storageConfigUUID = configUUID;
    params.universeUUID = universeUUID;
    params.setKeyspace("foo");
    params.setTableName("bar");
    params.tableUUID = UUID.randomUUID();
    params.scheduleUUID = scheduleUUID;
    params.timeBeforeDelete = -100L;
    return Backup.create(customerUUID, params);
  }

  public static Backup createBackupWithExpiry(
      UUID customerUUID, UUID universeUUID, UUID configUUID) {
    BackupTableParams params = new BackupTableParams();
    params.storageConfigUUID = configUUID;
    params.universeUUID = universeUUID;
    params.setKeyspace("foo");
    params.setTableName("bar");
    params.tableUUID = UUID.randomUUID();
    params.timeBeforeDelete = -100L;
    return Backup.create(customerUUID, params);
  }

  public static Schedule createScheduleBackup(
      UUID customerUUID, UUID universeUUID, UUID configUUID) {
    BackupTableParams params = new BackupTableParams();
    params.storageConfigUUID = configUUID;
    params.universeUUID = universeUUID;
    params.setKeyspace("foo");
    params.setTableName("bar");
    params.tableUUID = UUID.randomUUID();
    return Schedule.create(customerUUID, params, TaskType.BackupUniverse, 1000, null);
  }

  public static CustomerConfig setCallhomeLevel(Customer customer, String level) {
    return CustomerConfig.createCallHomeConfig(customer.uuid, level);
  }

  public static CustomerConfig createAlertConfig(
      Customer customer, String alertingEmail, boolean sendAlertsToYb, boolean reportOnlyErrors) {
    AlertingData data = new AlertingData();
    data.sendAlertsToYb = sendAlertsToYb;
    data.alertingEmail = alertingEmail;
    data.reportOnlyErrors = reportOnlyErrors;

    CustomerConfig config = CustomerConfig.createAlertConfig(customer.uuid, Json.toJson(data));
    config.save();
    return config;
  }

  public static AlertConfiguration createAlertConfiguration(
      Customer customer, Universe universe, Consumer<AlertConfiguration> modifier) {
    AlertConfiguration configuration =
        new AlertConfiguration()
            .setName("alertConfiguration")
            .setDescription("alertConfiguration description")
            .setCustomerUUID(customer.getUuid())
            .setTargetType(AlertConfiguration.TargetType.UNIVERSE)
            .setCreateTime(nowWithoutMillis())
            .setThresholds(
                ImmutableMap.of(
                    AlertConfiguration.Severity.SEVERE,
                    new AlertConfigurationThreshold()
                        .setCondition(Condition.GREATER_THAN)
                        .setThreshold(1D)))
            .setThresholdUnit(Unit.PERCENT)
            .setDefaultDestination(true)
            .generateUUID();
    if (universe != null) {
      configuration
          .setTarget(
              new AlertConfigurationTarget().setUuids(ImmutableSet.of(universe.getUniverseUUID())))
          .setTemplate(AlertTemplate.MEMORY_CONSUMPTION);
    } else {
      configuration
          .setTarget(new AlertConfigurationTarget().setAll(true))
          .setTemplate(AlertTemplate.BACKUP_FAILURE);
    }
    modifier.accept(configuration);
    configuration.save();
    return configuration;
  }

  public static AlertConfiguration createAlertConfiguration(Customer customer, Universe universe) {
    return createAlertConfiguration(customer, universe, c -> {});
  }

  public static AlertDefinition createAlertDefinition(Customer customer, Universe universe) {
    AlertConfiguration configuration = createAlertConfiguration(customer, universe);
    return createAlertDefinition(customer, universe, configuration);
  }

  public static AlertDefinition createAlertDefinition(
      Customer customer, Universe universe, AlertConfiguration configuration) {
    AlertDefinition alertDefinition =
        new AlertDefinition()
            .setConfigurationUUID(configuration.getUuid())
            .setCustomerUUID(customer.getUuid())
            .setQuery("query {{ query_condition }} {{ query_threshold }}")
            .generateUUID();
    if (universe != null) {
      alertDefinition.setLabels(
          MetricLabelsBuilder.create().appendSource(universe).getDefinitionLabels());
    } else {
      alertDefinition.setLabels(
          MetricLabelsBuilder.create().appendSource(customer).getDefinitionLabels());
    }
    alertDefinition.save();
    return alertDefinition;
  }

  public static Alert createAlert(Customer customer) {
    return createAlert(customer, null, null, null);
  }

  public static Alert createAlert(Customer customer, Universe universe) {
    return createAlert(customer, universe, null);
  }

  public static Alert createAlert(Customer customer, AlertDefinition definition) {
    return createAlert(customer, null, definition);
  }

  public static Alert createAlert(
      Customer customer, AlertDefinition definition, Consumer<Alert> modifier) {
    return createAlert(customer, null, definition, modifier);
  }

  public static Alert createAlert(
      Customer customer, Universe universe, AlertDefinition definition) {
    return createAlert(customer, universe, definition, a -> {});
  }

  public static Alert createAlert(
      Customer customer, Universe universe, AlertDefinition definition, Consumer<Alert> modifier) {
    Alert alert =
        new Alert()
            .setCustomerUUID(customer.getUuid())
            .setName("Alert 1")
            .setSourceName("Source 1")
            .setSourceUUID(
                universe == null
                    ? UUID.fromString("3ea389e6-07e7-487e-8592-c1b2a7339590")
                    : universe.getUniverseUUID())
            .setSeverity(AlertConfiguration.Severity.SEVERE)
            .setMessage("Universe on fire!")
            .generateUUID();
    if (definition == null) {
      AlertConfiguration configuration = createAlertConfiguration(customer, universe);
      definition = createAlertDefinition(customer, universe, configuration);
    }
    AlertConfiguration configuration =
        AlertConfiguration.db().find(AlertConfiguration.class, definition.getConfigurationUUID());
    alert.setConfigurationUuid(definition.getConfigurationUUID());
    alert.setConfigurationType(configuration.getTargetType());
    alert.setDefinitionUuid(definition.getUuid());
    List<AlertLabel> labels =
        definition
            .getEffectiveLabels(configuration, AlertConfiguration.Severity.SEVERE)
            .stream()
            .map(l -> new AlertLabel(l.getName(), l.getValue()))
            .collect(Collectors.toList());
    alert.setLabels(labels);
    if (modifier != null) {
      modifier.accept(alert);
    }
    alert.save();
    return alert;
  }

  public static AlertChannel createAlertChannel(
      UUID customerUUID, String name, AlertChannelParams params) {
    AlertChannel channel =
        new AlertChannel()
            .generateUUID()
            .setCustomerUUID(customerUUID)
            .setName(name)
            .setParams(params);
    channel.save();
    return channel;
  }

  public static AlertChannel createEmailChannel(UUID customerUUID, String name) {
    return createAlertChannel(customerUUID, name, createEmailChannelParams());
  }

  public static AlertChannel createSlackChannel(UUID customerUUID, String name) {
    return createAlertChannel(customerUUID, name, createSlackChannelParams());
  }

  public static AlertChannelEmailParams createEmailChannelParams() {
    AlertChannelEmailParams params = new AlertChannelEmailParams();
    params.setRecipients(Collections.singletonList("test@test.com"));
    params.setSmtpData(EmailFixtures.createSmtpData());
    return params;
  }

  public static AlertChannelSlackParams createSlackChannelParams() {
    AlertChannelSlackParams params = new AlertChannelSlackParams();
    params.setUsername("channel");
    params.setWebhookUrl("http://www.google.com");
    return params;
  }

  public static AlertDestination createAlertDestination(
      UUID customerUUID, String name, List<AlertChannel> channels) {
    AlertDestination destination =
        new AlertDestination()
            .generateUUID()
            .setCustomerUUID(customerUUID)
            .setName(name)
            .setChannelsList(channels);
    destination.save();
    return destination;
  }

  public static MaintenanceWindow createMaintenanceWindow(UUID customerUUID) {
    return createMaintenanceWindow(customerUUID, window -> {});
  }

  public static MaintenanceWindow createMaintenanceWindow(
      UUID customerUUID, Consumer<MaintenanceWindow> modifier) {
    Date startDate = nowMinusWithoutMillis(1, ChronoUnit.HOURS);
    Date endDate = nowPlusWithoutMillis(1, ChronoUnit.HOURS);
    AlertConfigurationApiFilter filter = new AlertConfigurationApiFilter();
    filter.setName("Replication Lag");
    MaintenanceWindow window =
        new MaintenanceWindow()
            .generateUUID()
            .setName("Test")
            .setDescription("Test Description")
            .setStartTime(startDate)
            .setEndTime(endDate)
            .setCustomerUUID(customerUUID)
            .setCreateTime(nowWithoutMillis())
            .setAlertConfigurationFilter(filter);
    modifier.accept(window);
    window.save();
    return window;
  }

  /*
   * KMS Configuration creation helpers.
   */
  public static KmsConfig createKMSConfig(
      UUID customerUUID, String keyProvider, ObjectNode authConfig) {
    return createKMSConfig(customerUUID, keyProvider, authConfig, "Test KMS Configuration");
  }

  public static KmsConfig createKMSConfig(
      UUID customerUUID, String keyProvider, ObjectNode authConfig, String configName) {
    EncryptionAtRestManager keyManager = new EncryptionAtRestManager();
    EncryptionAtRestService keyService = keyManager.getServiceInstance(keyProvider);
    return keyService.createAuthConfig(customerUUID, configName, authConfig);
  }

  /*
   * CertificateInfo creation helpers.
   */
  public static CertificateInfo createCertificateInfo(
      UUID customerUUID, String certificate, CertConfigType certType)
      throws IOException, NoSuchAlgorithmException {
    return CertificateInfo.create(
        UUID.randomUUID(), customerUUID, "test", new Date(), new Date(), "", certificate, certType);
  }

  // Create a universe from the configuration string. The configuration string
  // format is:
  //
  // config = zone1 descr; zone2 descr; ...; zoneK descr
  // zone descr = region - zone - nodes in zone - masters in zone.
  //
  // Example:
  // r1-az1-5-1;r1-az2-4-1;r1-az3-3-1;r2-az4-2-1;r2-az5-2-1;r2-az6-1-0
  //
  // Additional details:
  // - RF could be calculated as a sum of all masters across these zones;
  // - Region+zone may not have any nodes - in such case it will be created and
  // can be used in further required operations (as example, to add some nodes in
  // this region/zone);
  // - For each mentioned regions we are trying to find it at first. If the region
  // isn't found, it is created. The same is about availability zones.
  public static Universe createFromConfig(Provider provider, String univName, String config) {
    Customer customer = Customer.get(provider.customerUUID);
    Universe universe = createUniverse(univName, customer.getCustomerId());

    UUID placementUuid = universe.getUniverseDetails().getPrimaryCluster().uuid;
    PlacementCloud cloud = new PlacementCloud();
    cloud.uuid = provider.uuid;
    cloud.code = provider.code;

    PlacementInfo placementInfo = new PlacementInfo();
    placementInfo.cloudList.add(cloud);

    String[] zoneDescr = config.split(";");
    Set<NodeDetails> nodes = new HashSet<>();
    int index = 0;
    int rf = 0;
    int numNodes = 0;
    List<UUID> regionList = new ArrayList<>();

    for (String descriptor : zoneDescr) {
      String[] parts = descriptor.split("-");

      String regionCode = parts[0];
      Region region = Region.getByCode(provider, regionCode);
      if (region == null) {
        region = Region.create(provider, regionCode, regionCode, "yb-image-1");
      }

      String zone = parts[1];
      Optional<AvailabilityZone> azOpt = AvailabilityZone.maybeGetByCode(provider, zone);
      AvailabilityZone az =
          !azOpt.isPresent()
              ? AvailabilityZone.createOrThrow(region, zone, zone, "subnet-" + zone)
              : azOpt.get();

      int count = Integer.parseInt(parts[2]);
      if (count == 0) {
        // No nodes in this zone yet.
        continue;
      }

      PlacementAZ zonePlacement = getOrCreatePlacementAZ(placementInfo, region, az);
      int mastersCount = Integer.parseInt(parts[3]);
      rf += mastersCount;
      numNodes += count;
      zonePlacement.replicationFactor = mastersCount;
      for (int i = 0; i < count; i++) {
        NodeDetails node =
            ApiUtils.getDummyNodeDetails(
                index++,
                NodeDetails.NodeState.Live,
                mastersCount-- > 0,
                true,
                "aws",
                regionCode,
                az.code,
                null);
        node.placementUuid = placementUuid;
        node.azUuid = az.uuid;
        nodes.add(node);

        zonePlacement.numNodesInAZ++;
      }
      regionList.add(region.uuid);
    }

    // Update userIntent for Universe
    UserIntent userIntent = new UserIntent();
    userIntent.universeName = univName;
    userIntent.replicationFactor = rf;
    userIntent.numNodes = numNodes;
    userIntent.provider = provider.code;
    userIntent.regionList = regionList;
    userIntent.instanceType = ApiUtils.UTIL_INST_TYPE;
    userIntent.ybSoftwareVersion = "0.0.1";
    userIntent.accessKeyCode = "akc";
    userIntent.providerType = CloudType.aws;
    userIntent.preferredRegion = null;

    UniverseUpdater updater =
        u -> {
          UniverseDefinitionTaskParams universeDetails = u.getUniverseDetails();
          universeDetails.nodeDetailsSet = nodes;
          Cluster primaryCluster = universeDetails.getPrimaryCluster();
          primaryCluster.userIntent = userIntent;
          primaryCluster.placementInfo = placementInfo;
        };

    return Universe.saveDetails(universe.universeUUID, updater);
  }

  private static PlacementRegion createRegionPlacement(Region region) {
    PlacementRegion regionPlacement;
    regionPlacement = new PlacementRegion();
    regionPlacement.code = region.code;
    regionPlacement.name = region.name;
    regionPlacement.uuid = region.uuid;
    return regionPlacement;
  }

  /*
   * Finds region placement and then AZ placement in the passed PlacementInfo. If
   * region or/and AZ placements don't exists, they are created.
   */
  public static PlacementAZ getOrCreatePlacementAZ(
      PlacementInfo pi, Region region, AvailabilityZone az) {
    PlacementAZ zonePlacement = PlacementInfoUtil.findPlacementAzByUuid(pi, az.uuid);
    if (zonePlacement == null) {
      // Need to add the zone itself.
      PlacementRegion regionPlacement = findPlacementRegionByUuid(pi, region.uuid);
      if (regionPlacement == null) {
        // The region is missed as well.
        regionPlacement = createRegionPlacement(region);
        pi.cloudList.get(0).regionList.add(regionPlacement);
      }

      zonePlacement = new PlacementAZ();
      zonePlacement.name = az.name;
      zonePlacement.uuid = az.uuid;
      regionPlacement.azList.add(zonePlacement);
    }
    return zonePlacement;
  }

  private static PlacementRegion findPlacementRegionByUuid(PlacementInfo placementInfo, UUID uuid) {
    for (PlacementCloud cloud : placementInfo.cloudList) {
      for (PlacementRegion region : cloud.regionList) {
        if (region.uuid.equals(uuid)) {
          return region;
        }
      }
    }
    return null;
  }

  /*
   * Creates list with one tablespaceInfo record according to passed parameters.
   *
   * Example 1:<br>
   *   generateTablespaceParams(universeUUID, 'aws', 3, "r1-az1-1;r1-az2-1;r1-az3-1")
   *
   * It creates a list with one TableSpaceInfo with three placement blocks. The
   * number of replicas for all placement blocks is 3, three blocks for az1, az2
   * and az3 in cloud 'aws', region 'r1' with min_num_replicas=1 in each block.
   *
   * Example 2:<br>
   *   generateTablespaceParams(universeUUID, 'aws', 3, "r1-az1-1;r1-az2-1-1;r1-az3-1-2")
   *
   * The same configuration but for az1 we have leader_preference=1 and for az2 - 2.
   */
  public static CreateTablespaceParams generateTablespaceParams(
      UUID universeUUID, String cloud, int numReplicas, String config) {
    TableSpaceInfo tsi = new TableSpaceInfo();
    tsi.name = "test_tablespace";
    tsi.numReplicas = numReplicas;
    tsi.placementBlocks = new ArrayList<>();

    if (!StringUtils.isEmpty(config)) {
      String[] placements = config.split(";");
      for (String placement : placements) {
        String[] parts = placement.split("-");
        PlacementBlock pb = new PlacementBlock();
        pb.cloud = cloud;
        pb.region = parts[0];
        pb.zone = parts[1];
        pb.minNumReplicas = Integer.valueOf(parts[2]);
        pb.leaderPreference = parts.length > 3 ? Integer.valueOf(parts[3]) : null;
        tsi.placementBlocks.add(pb);
      }
    }

    CreateTablespaceParams params = new CreateTablespaceParams();
    params.tablespaceInfos = Collections.singletonList(tsi);
    return params;
  }
}
