It's a fork from https://github.com/foyewmaddeeb/mcz-maestro-ble
##

The goal of this project is to provide local access to your oven without relying on the MCZ cloud. 
Home Assistant integration is included — all entities are discovered automatically, so no separate integration is required.


Summary of All Changes
1. Platform Migration: ESP32Dev → ESP32-C3 (CYD removed)
Removed all CYD-specific code: display_cyd.cpp (TFT_eSPI/LVGL/touch driver), fan_icon.h, include/lv_conf.h
platformio.ini rewritten to a single esp32-c3-devkitm-1 environment (NimBLE, ArduinoJson, PubSubClient kept)
Removed touch-calibration/language/screen-timeout fields from AppConfig (no longer relevant, headless build)
display_none.cpp and display.h cleaned up as the sole (no-op) display backend
2. Home Assistant MQTT Discovery Fixes
select.fan: guarded against fan_level values outside 1..fanLevels (e.g. 0 during "Loading") → renders as unknown instead of triggering "Invalid option"
number.power: same guard for values outside 1..5
switch.silent: fixed 'dict object' has no attribute 'silent' warning by defaulting missing key to false
climate preset mode: renamed preset_mode_state_template → preset_mode_value_template (correct HA field name); also filters out invalid mode_name values (e.g. "?") instead of forwarding them
3. Firmware Data-Integrity Fixes (main.cpp)
Counter sanity filter (acceptCounter()): rejects decreasing or implausibly large jumps in worktime_min / time_power_X (32-bit values assembled from two 16-bit reads — a torn/corrupted read could otherwise produce garbage of tens of millions of minutes, breaking HA's total_increasing statistics)
Root-cause fix for stale BLE responses: Modbus read responses are now validated against the expected register count (g_lastReadCount) before being applied — previously a late/mismatched response could be misapplied under the wrong register base, explaining most of the earlier garbage values (mode:14, power:14, setpoint:2522.1, etc.)
4. NTP → Oven Clock Sync Reliability
The oven clock write (Modbus Fn 0x10) previously had no confirmation — it was assumed successful as soon as sent
Added a read-back verification right after writing: compares the oven's actual date/time against what was sent
On mismatch/failure: retries every 60 seconds instead of waiting 24 hours; logs a clear success/failure message
5. WiFi Telnet Console
New net_telnet.h/.cpp: a TelnetSerial class that mirrors all Serial output/input to a WiFi Telnet client (port 23), in addition to USB
Applied via a scoped #define Serial g_telnetSerial in main.cpp, net_mqtt.cpp, chrono.cpp, appconfig.cpp — no changes needed to existing Serial.print/printf calls
One client at a time; second connection attempts are rejected
Board's WiFi IP is now logged on connect (previously never printed)


## Disabling the Cloud Connection

With this solution in place, you can disable the oven's Wi-Fi connection entirely. If you prefer to block the connection at your router instead, note that the oven tries to reach the hostname *m.maestro.mcz.it*.
## Telnet 

List of available serial/telnet commands (from `handleLine` in `main.cpp`):

| Command | Description |
|---|---|
| `help` | Shows the list of commands |
| `status` | Displays the current oven status |
| `poll` | Forces a read of register 0x02BC (33 values) |
| `on` / `off` | Turns the oven on / off |
| `temp <c>` | Sets target temperature (°C) |
| `power <1-5>` | Sets power level |
| `mode <0-4>` | Sets mode (0=Manual, 1=Auto, 2=Overnight, 3=Comfort, 4=Turbo) |
| `fan <auto\|1-5>` | Sets fan speed |
| `silent on` / `silent off` | Enables/disables silent mode |
| `settime` | Sends current time (NTP) to the oven |
| `getchrono` | Reads and displays chrono/scheduling registers |
| `alarms` | Shows alarm history |
| `log on` / `log off` | Enables/disables raw frame logging |
| `scan` | Scans ~8s for nearby MCZ_EP ovens over BLE |
| `target <mac\|none>` | Sets (or clears) the targeted MAC address, then restarts |
| `target` (alone) | Shows the currently configured target |
| `r <regHex> <count>` | Raw Modbus register read |
| `w <regHex> <valHex>` | Raw Modbus register write |
| `wm <regHex> <v0> <v1>...` | Multi-register Modbus write (function 0x10) |
| `ctr <hex>` | Manually sets the frame counter |

##

The ESP can be powered directly from the motherboard's USB port.
<img width="961" height="899" alt="IMG_6411" src="https://github.com/user-attachments/assets/bf8e8dd3-f099-4723-a655-501891867545" />

