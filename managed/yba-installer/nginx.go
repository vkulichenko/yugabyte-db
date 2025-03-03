/*
 * Copyright (c) YugaByte, Inc.
 */

package main

import (
    "fmt"
    "os"
)

// Component 4: Nginx
type Nginx struct {
    Name               string
    ConfFileLocation   string
    Mode               string
    ServerName         string
    ServerKeyLocation  string
    ServerCertLocation string
}

// Method of the Component
// Interface are implemented by
// the Nginx struct and customizable
// for each specific service.

func (ngi Nginx) SetUpPrereqs() {
    arg1 := []string{"epel-release"}
    YumInstall(arg1)
    arg2 := []string{"nginx"}
    YumInstall(arg2)
}

func (ngi Nginx) Install() {
    if ngi.Mode == "https" {
        configureNginxConfHTTPS(ngi.ServerKeyLocation, ngi.ServerCertLocation,
        ngi.ConfFileLocation)
    }
    certTLSstorage()
}

func (ngi Nginx) Start() {
    command1 := "systemctl"
    arg1 := []string{"daemon-reload"}
    ExecuteBashCommand(command1, arg1)

    command2 := "systemctl"
    arg2 := []string{"start", "nginx"}
    ExecuteBashCommand(command2, arg2)

    command3 := "systemctl"
    arg3 := []string{"status", "nginx"}
    ExecuteBashCommand(command3, arg3)
}

func (ngi Nginx) Stop() {
    command1 := "systemctl"
    arg1 := []string{"stop", "nginx"}
    ExecuteBashCommand(command1, arg1)
}

func (ngi Nginx) Restart() {
    command1 := "systemctl"
    arg1 := []string{"restart", "nginx"}
    ExecuteBashCommand(command1, arg1)
}

func (ngi Nginx) GetConfFileLocation() string {
    return ngi.ConfFileLocation
}

// Per current cleanup.sh script.
func (ngi Nginx) Uninstall() {
    ngi.Stop()
    os.RemoveAll("/opt/yugabyte")
}

func configureNginxConfHTTPS(server_cert_location string,
 server_key_location string, confFileLoc string) {

    os.MkdirAll("/opt/yugabyte/certs", os.ModePerm)
    fmt.Println("/opt/yugabyte/certs directory successfully created.")
    destination_tls := "/opt/yugabyte/certs"
    CopyFileGolang(server_key_location, destination_tls)
    CopyFileGolang(server_cert_location, destination_tls)

    command1 := "chown"
    arg1 := []string{"yugabyte:yugabyte", "/opt/yugabyte/certs/server.key"}
    ExecuteBashCommand(command1, arg1)

    command2 := "chown"
    arg2 := []string{"yugabyte:yugabyte", "/opt/yugabyte/certs/server.crt"}
    ExecuteBashCommand(command2, arg2)
}

func certTLSstorage() {

    os.MkdirAll("/opt/yugaware", os.ModePerm)
    fmt.Println("/opt/yugaware directory successfully created.")
    command1 := "chown"
    arg1 := []string{"yugabyte:yugabyte", "-R", "/opt/yugaware"}
    ExecuteBashCommand(command1, arg1)

}
