// net_mqtt.h — WiFi + MQTT + Home Assistant discovery for the oven remote control.
// Publishes g_oven as JSON, subscribes to set/* command topics and calls the
// command API (oven.h). With -DUSE_MQTT=0 all functions are empty stubs.
#pragma once

void netBegin();   // call in setup(): initialize WiFi + MQTT
void netTick();    // call in loop(): keep connection alive, publish on-change/heartbeat
bool netWifiUp();  // WiFi associated?
bool netMqttUp();  // MQTT broker connected?
