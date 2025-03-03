---
title: Install YugabyteDB software
headerTitle: Install software
linkTitle: 2. Install software
description: Download and install YugabyteDB software to each node
menu:
  preview:
    identifier: deploy-manual-deployment-install-software
    parent: deploy-manual-deployment
    weight: 612
type: docs
---

Install YugabyteDB on each of the nodes using the steps shown below.

## Download

Download the YugabyteDB binary package as described in the [Quick Start](../../../quick-start/).

Copy the YugabyteDB package into each instance and then run the following commands.

```sh
$ tar xvfz yugabyte-<version>-<os>.tar.gz && cd yugabyte-<version>/
```

## Configure

- Run the **post_install.sh** script to make some final updates to the installed software.

```sh
$ ./bin/post_install.sh
```
