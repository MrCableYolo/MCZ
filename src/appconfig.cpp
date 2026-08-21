// appconfig.cpp — NVS-backed runtime configuration.
#include "appconfig.h"
#include <Preferences.h>
#include "config.h"      // Default/first-boot values

#ifndef TARGET_MAC
#  define TARGET_MAC ""
#endif

AppConfig g_cfg;
static const char* NS = "mcz";   // NVS namespace

void configLoad(){
  Preferences p; p.begin(NS, true);            // read-only
  g_cfg.wifiSsid  = p.getString("wifi_ssid",  WIFI_SSID);
  g_cfg.wifiPass  = p.getString("wifi_pass",  WIFI_PASSWORD);
  g_cfg.mqttHost  = p.getString("mqtt_host",  MQTT_HOST);
  g_cfg.mqttPort  = p.getUShort("mqtt_port",  MQTT_PORT);
  g_cfg.mqttUser  = p.getString("mqtt_user",  MQTT_USER);
  g_cfg.mqttPass  = p.getString("mqtt_pass",  MQTT_PASSWORD);
  g_cfg.targetMac = p.getString("target_mac", TARGET_MAC);  // runtime target from NVS (set via serial 'target <mac>')
  p.end();
  Serial.printf(">> Config loaded: wifi='%s' mqtt=%s:%u oven='%s'\n",
                g_cfg.wifiSsid.c_str(), g_cfg.mqttHost.c_str(), g_cfg.mqttPort,
                g_cfg.targetMac.length()? g_cfg.targetMac.c_str() : "(first available)");
}

void configSave(){
  Preferences p; p.begin(NS, false);           // read-write
  p.putString("wifi_ssid",  g_cfg.wifiSsid);
  p.putString("wifi_pass",  g_cfg.wifiPass);
  p.putString("mqtt_host",  g_cfg.mqttHost);
  p.putUShort("mqtt_port",  g_cfg.mqttPort);
  p.putString("mqtt_user",  g_cfg.mqttUser);
  p.putString("mqtt_pass",  g_cfg.mqttPass);
  p.putString("target_mac", g_cfg.targetMac);
  p.end();
  Serial.println(">> Config saved (NVS).");
}

void configReboot(){
  configSave();
  Serial.println(">> Restarting ...");
  delay(300);
  ESP.restart();
}
