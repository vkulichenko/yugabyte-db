// Copyright (c) YugaByte, Inc.
package com.yugabyte.yw.commissioner.tasks;

import com.yugabyte.yw.commissioner.BaseTaskDependencies;
import com.yugabyte.yw.commissioner.UserTaskDetails;
import com.yugabyte.yw.common.Util;
import com.yugabyte.yw.forms.BackupRequestParams;
import com.yugabyte.yw.forms.BackupTableParams;
import com.yugabyte.yw.forms.RestoreBackupParams;
import com.yugabyte.yw.models.Backup;
import com.yugabyte.yw.models.Backup.BackupCategory;
import com.yugabyte.yw.models.Customer;
import com.yugabyte.yw.models.Universe;
import com.yugabyte.yw.models.XClusterConfig;
import com.yugabyte.yw.models.XClusterConfig.XClusterConfigStatusType;
import java.io.File;
import java.util.ArrayList;
import java.util.Collections;
import java.util.List;
import java.util.Map;
import java.util.Optional;
import java.util.Set;
import java.util.concurrent.TimeUnit;
import java.util.stream.Collectors;
import javax.inject.Inject;
import lombok.extern.slf4j.Slf4j;
import org.yb.CommonTypes;
import org.yb.client.ListTablesResponse;
import org.yb.client.YBClient;
import org.yb.master.MasterDdlOuterClass;
import org.yb.master.MasterTypes;

@Slf4j
public class CreateXClusterConfig extends XClusterConfigTaskBase {

  public static final long TIME_BEFORE_DELETE_BACKUP_MS = TimeUnit.DAYS.toMillis(1);

  @Inject
  protected CreateXClusterConfig(BaseTaskDependencies baseTaskDependencies) {
    super(baseTaskDependencies);
  }

  @Override
  public void run() {
    log.info("Running {}", getName());

    XClusterConfig xClusterConfig = getXClusterConfigFromTaskParams();
    Universe sourceUniverse = Universe.getOrBadRequest(xClusterConfig.sourceUniverseUUID);
    Universe targetUniverse = Universe.getOrBadRequest(xClusterConfig.targetUniverseUUID);
    try {
      // Lock the source universe.
      lockUniverseForUpdate(sourceUniverse.universeUUID, sourceUniverse.version);
      try {
        // Lock the target universe.
        lockUniverseForUpdate(targetUniverse.universeUUID, targetUniverse.version);

        if (xClusterConfig.status != XClusterConfigStatusType.Init) {
          throw new RuntimeException(
              String.format(
                  "XClusterConfig(%s) must be in `Init` state to create replication for",
                  xClusterConfig.uuid));
        }
        if (xClusterConfig.getTables().size() < 1) {
          throw new RuntimeException(
              "At least one table must be selected to set up replication for");
        }

        createXClusterConfigSetStatusTask(XClusterConfigStatusType.Updating)
            .setSubTaskGroupType(UserTaskDetails.SubTaskGroupType.ConfigureUniverse);

        // Ensure the user table selection respects our constraints.
        Map<String, List<MasterDdlOuterClass.ListTablesResponsePB.TableInfo>>
            requestedNamespaceTablesInfoMap = checkTables();
        // At least one entry exists in requestedNamespaceTablesInfoMap and each list in any entry
        // has at least one TableInfo object.
        CommonTypes.TableType tableType =
            requestedNamespaceTablesInfoMap
                .entrySet()
                .stream()
                .findAny()
                .get()
                .getValue()
                .get(0)
                .getTableType();

        // Support mismatched TLS root certificates.
        Optional<File> sourceCertificate =
            getSourceCertificateIfNecessary(sourceUniverse, targetUniverse);
        sourceCertificate.ifPresent(
            cert ->
                createSetupSourceCertificateTask(
                    targetUniverse, xClusterConfig.getReplicationGroupName(), cert));

        checkBootstrapRequired(getTableIdsNeedBootstrap());

        requestedNamespaceTablesInfoMap.forEach(
            (namespaceId, tablesInfoList) -> {
              Set<String> tableIdsInNamespace = getTableIds(tablesInfoList);
              // If at least one YSQL table needs bootstrap, it must be done for all tables in that
              // keyspace.
              if (tableType == CommonTypes.TableType.PGSQL_TABLE_TYPE
                  && !getTablesNeedBootstrap(tableIdsInNamespace).isEmpty()) {
                xClusterConfig.setNeedBootstrapForTables(
                    tableIdsInNamespace, true /* needBootstrap */);
              }
            });

        // Replication for tables that do NOT need bootstrapping.
        Set<String> tableIdsNotNeedBootstrap = getTableIdsNotNeedBootstrap();
        if (!tableIdsNotNeedBootstrap.isEmpty()) {
          // Set up the replication config.
          createXClusterConfigSetupTask(tableIdsNotNeedBootstrap)
              .setSubTaskGroupType(UserTaskDetails.SubTaskGroupType.ConfigureUniverse);
        }

        // Add the subtasks to set up replication for tables that need bootstrapping.
        addSubtasksForTablesNeedBootstrap(targetUniverse, requestedNamespaceTablesInfoMap);

        createXClusterConfigSetStatusTask(XClusterConfigStatusType.Running)
            .setSubTaskGroupType(UserTaskDetails.SubTaskGroupType.ConfigureUniverse);

        createMarkUniverseUpdateSuccessTasks(targetUniverse.universeUUID)
            .setSubTaskGroupType(UserTaskDetails.SubTaskGroupType.ConfigureUniverse);

        createMarkUniverseUpdateSuccessTasks(sourceUniverse.universeUUID)
            .setSubTaskGroupType(UserTaskDetails.SubTaskGroupType.ConfigureUniverse);

        getRunnableTask().runSubTasks();
      } catch (Exception e) {
        log.error("{} hit error : {}", getName(), e.getMessage());
        throw new RuntimeException(e);
      } finally {
        // Unlock the target universe.
        unlockUniverseForUpdate(targetUniverse.universeUUID);
      }
    } catch (Exception e) {
      log.error("{} hit error : {}", getName(), e.getMessage());
      setXClusterConfigStatus(XClusterConfigStatusType.Failed);
      throw new RuntimeException(e);
    } finally {
      // Unlock the source universe.
      unlockUniverseForUpdate(sourceUniverse.universeUUID);
    }

    log.info("Completed {}", getName());
  }

  private void addSubtasksForTablesNeedBootstrap(
      Universe targetUniverse,
      Map<String, List<MasterDdlOuterClass.ListTablesResponsePB.TableInfo>>
          requestedNamespaceTablesInfoMap) {
    XClusterConfig xClusterConfig = taskParams().xClusterConfig;

    boolean isReplicationConfigCreated = !getTablesNotNeedBootstrap().isEmpty();
    for (String namespaceId : requestedNamespaceTablesInfoMap.keySet()) {
      List<MasterDdlOuterClass.ListTablesResponsePB.TableInfo> tablesInfoList =
          requestedNamespaceTablesInfoMap.get(namespaceId);
      if (tablesInfoList.isEmpty()) {
        throw new RuntimeException(
            String.format("tablesInfoList in namespaceId %s is empty", namespaceId));
      }
      CommonTypes.TableType tableType = tablesInfoList.get(0).getTableType();
      String namespace = tablesInfoList.get(0).getNamespace().getName();
      Set<String> tableIds = getTableIds(tablesInfoList);
      Set<String> tableIdsNeedBootstrap = getTableIdsNeedBootstrap(tableIds);
      if (!tableIdsNeedBootstrap.isEmpty()) {
        // Create checkpoints for the tables.
        createBootstrapProducerTask(tableIdsNeedBootstrap)
            .setSubTaskGroupType(UserTaskDetails.SubTaskGroupType.BootstrappingProducer);

        // Backup from the source universe.
        BackupRequestParams backupRequestParams =
            getBackupRequestParams(tableIdsNeedBootstrap, tablesInfoList);
        Backup backup =
            createAllBackupSubtasks(
                backupRequestParams, UserTaskDetails.SubTaskGroupType.CreatingBackup);
        // Assign the created backup UUID for the tables in the DB.
        xClusterConfig.setBackupForTables(tableIdsNeedBootstrap, backup);

        // If the table type is YCQL, delete the tables from the target universe, because if the
        // tables exist, the restore subtask will fail.
        if (tableType == CommonTypes.TableType.YQL_TABLE_TYPE) {
          List<String> tableNamesNeedBootstrap =
              tablesInfoList
                  .stream()
                  .filter(
                      tableInfo -> tableIdsNeedBootstrap.contains(tableInfo.getId().toStringUtf8()))
                  .map(MasterDdlOuterClass.ListTablesResponsePB.TableInfo::getName)
                  .collect(Collectors.toList());
          List<String> tableNamesToDeleteOnTargetUniverse =
              getTableInfoList(targetUniverse)
                  .stream()
                  .filter(
                      tableInfo ->
                          tableNamesNeedBootstrap.contains(tableInfo.getName())
                              && tableInfo.getNamespace().getName().equals(namespace))
                  .map(MasterDdlOuterClass.ListTablesResponsePB.TableInfo::getName)
                  .collect(Collectors.toList());
          createDeleteTablesFromUniverseTask(
                  targetUniverse.universeUUID,
                  Collections.singletonMap(namespace, tableNamesToDeleteOnTargetUniverse))
              .setSubTaskGroupType(UserTaskDetails.SubTaskGroupType.RestoringBackup);
        }

        // Restore to the target universe.
        RestoreBackupParams restoreBackupParams =
            getRestoreBackupParams(backupRequestParams, backup);
        createAllRestoreSubtasks(
            restoreBackupParams,
            UserTaskDetails.SubTaskGroupType.RestoringBackup,
            backup.category.equals(BackupCategory.YB_CONTROLLER));
        // Set the restore time for the tables in the DB.
        createSetRestoreTimeTask(tableIdsNeedBootstrap)
            .setSubTaskGroupType(UserTaskDetails.SubTaskGroupType.RestoringBackup);

        if (isReplicationConfigCreated) {
          // If the xCluster config is already created, add the bootstrapped tables to the created
          // xCluster config.
          createXClusterConfigModifyTablesTask(tableIdsNeedBootstrap, null /* tableIdsToRemove */)
              .setSubTaskGroupType(UserTaskDetails.SubTaskGroupType.ConfigureUniverse);
        } else {
          // Set up the replication config.
          createXClusterConfigSetupTask(tableIdsNeedBootstrap)
              .setSubTaskGroupType(UserTaskDetails.SubTaskGroupType.ConfigureUniverse);
          isReplicationConfigCreated = true;
        }
      }
    }
  }

  /**
   * It ensures that all requested tables exist on the source universe, and they have the same type.
   * Also, if the table type is YSQL and bootstrap is required, it ensures all the tables in a
   * keyspace are selected because per-table backup/restore is not supported for YSQL. In addition,
   * it ensures none of YCQL tables are index tables.
   *
   * @return A map of namespace ID to {@link MasterDdlOuterClass.ListTablesResponsePB.TableInfo}
   *     containing table info of the tables in that namespace requested to be in the xCluster
   *     config
   */
  private Map<String, List<MasterDdlOuterClass.ListTablesResponsePB.TableInfo>> checkTables() {
    XClusterConfig xClusterConfig = taskParams().xClusterConfig;
    Universe sourceUniverse = Universe.getOrBadRequest(xClusterConfig.sourceUniverseUUID);
    Set<String> tableIds = xClusterConfig.getTables();
    // Ensure at least one table exists to check.
    if (tableIds.isEmpty()) {
      throw new IllegalArgumentException(
          String.format("No table exists in the xCluster config(%s)", xClusterConfig));
    }

    List<MasterDdlOuterClass.ListTablesResponsePB.TableInfo> sourceTablesInfoList =
        getTableInfoList(sourceUniverse);
    List<MasterDdlOuterClass.ListTablesResponsePB.TableInfo> requestedTablesInfoList =
        sourceTablesInfoList
            .stream()
            .filter(tableInfo -> tableIds.contains(tableInfo.getId().toStringUtf8()))
            .collect(Collectors.toList());
    Map<String, List<MasterDdlOuterClass.ListTablesResponsePB.TableInfo>>
        requestedNamespaceTablesInfoMap =
            requestedTablesInfoList
                .stream()
                .collect(
                    Collectors.groupingBy(
                        tableInfo -> tableInfo.getNamespace().getId().toStringUtf8()));
    // All tables are found.
    if (requestedTablesInfoList.size() != tableIds.size()) {
      Set<String> foundTableIds = getTableIds(requestedTablesInfoList);
      Set<String> missingTableIds =
          tableIds
              .stream()
              .filter(tableId -> !foundTableIds.contains(tableId))
              .collect(Collectors.toSet());
      throw new IllegalArgumentException(
          String.format(
              "Some of the tables were not found on the source universe (%s): was %d, "
                  + "found %d, missing tables: %s",
              xClusterConfig.sourceUniverseUUID,
              tableIds.size(),
              requestedTablesInfoList.size(),
              missingTableIds));
    }
    // All tables have the same type.
    if (!requestedTablesInfoList
        .stream()
        .allMatch(
            tableInfo ->
                tableInfo.getTableType().equals(requestedTablesInfoList.get(0).getTableType()))) {
      throw new IllegalArgumentException(
          "At least one table has a different type from others. "
              + "All tables in an xCluster config must have the same type. Please create separate "
              + "xCluster configs for different table types.");
    }
    CommonTypes.TableType tableType = requestedTablesInfoList.get(0).getTableType();
    log.info(
        "All the requested tables in the xClusterConfig({}) are found and they have a type of {}",
        xClusterConfig,
        tableType);

    // Backup index table is not supported for YCQL.
    if (tableType == CommonTypes.TableType.YQL_TABLE_TYPE) {
      Set<String> tableIdsNeedBootstrap = getTableIdsNeedBootstrap();
      List<String> indexTablesIdWithBootstrapList =
          requestedTablesInfoList
              .stream()
              .filter(
                  tableInfo ->
                      tableInfo.hasRelationType()
                          && tableInfo.getRelationType()
                              == MasterTypes.RelationType.INDEX_TABLE_RELATION)
              .map(tableInfo -> tableInfo.getId().toStringUtf8())
              .filter(tableIdsNeedBootstrap::contains)
              .collect(Collectors.toList());
      if (!indexTablesIdWithBootstrapList.isEmpty()) {
        throw new IllegalArgumentException(
            String.format(
                "Bootstrap is not supported for YCQL index tables, but %s are index tables",
                indexTablesIdWithBootstrapList));
      }
    }

    // If table type is YSQL and bootstrap is required, all tables in the keyspace are
    // selected.
    if (tableType == CommonTypes.TableType.PGSQL_TABLE_TYPE) {
      requestedNamespaceTablesInfoMap.forEach(
          (namespaceId, tablesInfoList) -> {
            Set<String> tableIdsNeedBootstrap =
                getTableIdsNeedBootstrap(getTableIds(tablesInfoList));
            if (!tableIdsNeedBootstrap.isEmpty()) {
              Set<String> tableIdsInNamespace =
                  sourceTablesInfoList
                      .stream()
                      .filter(
                          tableInfo ->
                              tableInfo.getNamespace().getId().toStringUtf8().equals(namespaceId))
                      .map(tableInfo -> tableInfo.getId().toStringUtf8())
                      .collect(Collectors.toSet());
              Set<String> selectedTableIdsInNamespace = getTableIds(tablesInfoList);
              if (tableIdsInNamespace.size() != selectedTableIdsInNamespace.size()) {
                throw new IllegalArgumentException(
                    String.format(
                        "For YSQL tables, all the tables in a keyspace must be selected: "
                            + "selected: %s, tables in the keyspace: %s",
                        selectedTableIdsInNamespace, tableIdsInNamespace));
              }
            }
          });
    }

    return requestedNamespaceTablesInfoMap;
  }

  private List<MasterDdlOuterClass.ListTablesResponsePB.TableInfo> getTableInfoList(
      Universe universe) {
    List<MasterDdlOuterClass.ListTablesResponsePB.TableInfo> tableInfoList;
    String universeMasterAddresses = universe.getMasterAddresses(true /* mastersQueryable */);
    String universeCertificate = universe.getCertificateNodetoNode();
    try (YBClient client = ybService.getClient(universeMasterAddresses, universeCertificate)) {
      ListTablesResponse listTablesResponse = client.getTablesList(null, true, null);
      tableInfoList = listTablesResponse.getTableInfoList();
    } catch (Exception e) {
      throw new RuntimeException(e);
    }
    return tableInfoList;
  }

  private BackupRequestParams getBackupRequestParams(
      Set<String> tableIdsNeedBootstrap,
      List<MasterDdlOuterClass.ListTablesResponsePB.TableInfo> tablesInfoList) {
    BackupRequestParams backupRequestParams;
    if (taskParams().createFormData.bootstrapParams.backupRequestParams != null) {
      backupRequestParams =
          new BackupRequestParams(taskParams().createFormData.bootstrapParams.backupRequestParams);
    } else {
      // In case the user does not pass the backup parameters, use the default values.
      backupRequestParams = new BackupRequestParams();
      backupRequestParams.customerUUID =
          Customer.get(
                  Universe.getOrBadRequest(getXClusterConfigFromTaskParams().sourceUniverseUUID)
                      .customerId)
              .uuid;
      // Use the last storage config used for a successful backup as the default one.
      Optional<Backup> latestCompletedBackupOptional =
          Backup.fetchLatestByState(backupRequestParams.customerUUID, Backup.BackupState.Completed);
      if (!latestCompletedBackupOptional.isPresent()) {
        throw new RuntimeException(
            "bootstrapParams in XClusterConfigCreateFormData is null, and storageConfigUUID "
                + "cannot be determined based on the latest successful backup");
      }
      backupRequestParams.storageConfigUUID = latestCompletedBackupOptional.get().storageConfigUUID;
      log.info(
          "storageConfigUUID {} will be used for bootstrapping",
          backupRequestParams.storageConfigUUID);
    }
    // These parameters are pre-set. Others either come from the user, or the defaults are good.
    backupRequestParams.universeUUID = taskParams().xClusterConfig.sourceUniverseUUID;
    backupRequestParams.backupType = tablesInfoList.get(0).getTableType();
    backupRequestParams.timeBeforeDelete = TIME_BEFORE_DELETE_BACKUP_MS;
    backupRequestParams.expiryTimeUnit = com.yugabyte.yw.models.helpers.TimeUnit.MILLISECONDS;
    // Set to true because it is a beta version of bootstrapping, and we need to debug it.
    backupRequestParams.enableVerboseLogs = true;
    // Ensure keyspaceTableList is not specified by the user.
    if (backupRequestParams.keyspaceTableList != null) {
      throw new RuntimeException(
          "backupRequestParams.keyspaceTableList must be null, table selection happens "
              + "automatically");
    }
    backupRequestParams.keyspaceTableList = new ArrayList<>();
    BackupRequestParams.KeyspaceTable keyspaceTable = new BackupRequestParams.KeyspaceTable();
    keyspaceTable.keyspace = tablesInfoList.get(0).getNamespace().getName();
    if (backupRequestParams.backupType != CommonTypes.TableType.PGSQL_TABLE_TYPE) {
      List<MasterDdlOuterClass.ListTablesResponsePB.TableInfo> tablesNeedBootstrapInfoList =
          tablesInfoList
              .stream()
              .filter(tableInfo -> tableIdsNeedBootstrap.contains(tableInfo.getId().toStringUtf8()))
              .collect(Collectors.toList());
      keyspaceTable.tableNameList =
          tablesNeedBootstrapInfoList
              .stream()
              .map(MasterDdlOuterClass.ListTablesResponsePB.TableInfo::getName)
              .collect(Collectors.toList());
      keyspaceTable.tableUUIDList =
          tablesNeedBootstrapInfoList
              .stream()
              .map(tableInfo -> Util.getUUIDRepresentation(tableInfo.getId().toStringUtf8()))
              .collect(Collectors.toList());
    }
    backupRequestParams.keyspaceTableList.add(keyspaceTable);
    return backupRequestParams;
  }

  private RestoreBackupParams getRestoreBackupParams(
      BackupRequestParams backupRequestParams, Backup backup) {
    RestoreBackupParams restoreTaskParams = new RestoreBackupParams();
    // For the following parameters the default values will be used:
    //    restoreTaskParams.alterLoadBalancer = true
    //    restoreTaskParams.restoreTimeStamp = null
    //    public String oldOwner = "yugabyte"
    //    public String newOwner = null
    // The following parameters are set. For others, the defaults are good.
    restoreTaskParams.customerUUID = backupRequestParams.customerUUID;
    restoreTaskParams.universeUUID = taskParams().xClusterConfig.targetUniverseUUID;
    restoreTaskParams.kmsConfigUUID = backupRequestParams.kmsConfigUUID;
    if (restoreTaskParams.kmsConfigUUID != null) {
      restoreTaskParams.actionType = RestoreBackupParams.ActionType.RESTORE;
    } else {
      restoreTaskParams.actionType = RestoreBackupParams.ActionType.RESTORE_KEYS;
    }
    restoreTaskParams.enableVerboseLogs = backupRequestParams.enableVerboseLogs;
    restoreTaskParams.storageConfigUUID = backupRequestParams.storageConfigUUID;
    restoreTaskParams.useTablespaces = backupRequestParams.useTablespaces;
    restoreTaskParams.parallelism = backupRequestParams.parallelism;
    restoreTaskParams.disableChecksum = backupRequestParams.disableChecksum;
    restoreTaskParams.category = backup.category;
    // Set storage info.
    restoreTaskParams.backupStorageInfoList = new ArrayList<>();
    RestoreBackupParams.BackupStorageInfo backupStorageInfo =
        new RestoreBackupParams.BackupStorageInfo();
    backupStorageInfo.backupType = backupRequestParams.backupType;
    List<BackupTableParams> backupList = backup.getBackupInfo().backupList;
    if (backupList == null) {
      throw new RuntimeException("backup.getBackupInfo().backupList must not be null");
    }
    if (backupList.size() != 1) {
      String errMsg =
          String.format(
              "backup.getBackupInfo().backupList must have exactly one element, had %d",
              backupList.size());
      throw new RuntimeException(errMsg);
    }
    backupStorageInfo.storageLocation = backupList.get(0).storageLocation;
    List<BackupRequestParams.KeyspaceTable> keyspaceTableList =
        backupRequestParams.keyspaceTableList;
    if (keyspaceTableList == null) {
      throw new RuntimeException("backupRequestParams.keyspaceTableList must not be null");
    }
    if (keyspaceTableList.size() != 1) {
      String errMsg =
          String.format(
              "backupRequestParams.keyspaceTableList must have exactly one element, had %d",
              keyspaceTableList.size());
      throw new RuntimeException(errMsg);
    }
    backupStorageInfo.keyspace = keyspaceTableList.get(0).keyspace;
    backupStorageInfo.sse = backupRequestParams.sse;
    restoreTaskParams.backupStorageInfoList.add(backupStorageInfo);

    return restoreTaskParams;
  }
}
