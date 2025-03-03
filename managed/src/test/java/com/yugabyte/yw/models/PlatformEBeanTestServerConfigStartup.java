/*
 * Copyright 2021 YugaByte, Inc. and Contributors
 *
 * Licensed under the Polyform Free Trial License 1.0.0 (the "License"); you
 * may not use this file except in compliance with the License. You
 * may obtain a copy of the License at
 *
 * http://github.com/YugaByte/yugabyte-db/blob/master/licenses/POLYFORM-FREE-TRIAL-LICENSE-1.0.0.txt
 */

package com.yugabyte.yw.models;

import io.ebean.config.ServerConfig;
import io.ebean.event.ServerConfigStartup;

/**
 * Here we will modify EBean server config at test startup. EBeans framework will make sure that
 * this gets executed at start.
 */
public class PlatformEBeanTestServerConfigStartup implements ServerConfigStartup {
  @Override
  public void onStart(ServerConfig serverConfig) {
    serverConfig.setDatabasePlatform(new H2V2Platform());
  }
}
