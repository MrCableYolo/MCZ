// appconfig.h — runtime-changeable, persistent configuration (NVS/Preferences).
// Replaces the previous compile-time macros as config.h only
// provides the default/first-boot values. These values are changed via the display system
// menu and permanently stored with configReboot() (followed by a restart → clean re-init).
#pragma once
#include <Arduino.h>

struct AppConfig {
  String   wifiSsid;
  String   wifiPass;
  String   mqttHost;
  uint16_t mqttPort = 1883;
  String   mqttUser;
  String   mqttPass;
  String   targetMac;   // Oven BLE MAC (with/without ':'); empty = first available MCZ_EP
};
extern AppConfig g_cfg;

void configLoad();    // load from NVS; missing values -> defaults from config.h
void configSave();    // write g_cfg to NVS
void configReboot();  // configSave() + ESP.restart()
