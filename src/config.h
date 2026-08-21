// config_example.h — Template. Copy this file to src/config.h and enter your
// values. src/config.h is in .gitignore (contains secrets) and is NOT checked in.
//
//   cp src/config_example.h src/config.h
//
// Note: These values are only DEFAULTS / first boot. At runtime, WiFi, MQTT broker
// and target oven are stored in NVS (via the display system menu) and take precedence.
#pragma once

// ---- WiFi ----
// ---- WiFi ----
#define WIFI_SSID      ""
#define WIFI_PASSWORD  ""

// ---- MQTT broker ----
#define MQTT_HOST      "192.168"   // IP or hostname of the broker
#define MQTT_PORT      1883
#define MQTT_USER      ""               // leave empty if no login
#define MQTT_PASSWORD  ""

// ---- Device/topic identity ----
// DEVICE_ID determines topic prefix mcz/<id>/ and HA unique_id/identifiers.
// EMPTY "" = automatically use the oven BLE MAC (recommended: unique per oven, no typing).
// Only set a fixed string if you deliberately want to assign the ID yourself.
#define DEVICE_ID      ""
#define DEVICE_NAME    "MCZ Maestro+ Pelletstove"

// First-boot target oven (important with multiple ovens in range).
// Empty "" = first available oven named MCZ_EP...; otherwise the oven BLE MAC (with or without ':').
// At runtime this is overridden by the NVS value — switch ovens without reflashing via
// serial 'scan' + 'target <mac>' (or 'target none'), or Display -> Find stove.
#define TARGET_MAC     ""

// ---- Oven clock timezone (NTP -> oven RTC via BLE) ----
// The ESP fetches time from NTP (needs internet on the ESP's WiFi) and sets the oven clock.
// POSIX TZ string with automatic daylight-saving. Pick the one for your country:
//   Germany / Italy (CET/CEST): "CET-1CEST,M3.5.0,M10.5.0/3"
//   United Kingdom  (GMT/BST):  "GMT0BST,M3.5.0/1,M10.5.0"
#define OVEN_TZ        "CET-1CEST,M3.5.0,M10.5.0/3"

// ---- Home Assistant MQTT discovery ----
// Discovery prefix of Home Assistant (default: "homeassistant").
#define HA_DISCOVERY_PREFIX "homeassistant"
