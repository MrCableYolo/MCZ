// net_mqtt.cpp — WiFi + MQTT + Home Assistant discovery.
// Device identity = oven BLE MAC (unique across multiple ovens); display name incl.
// serial number. Both are only known after the BLE connect -> topics/discovery are
// built lazily once the identity is known.
#include "net_mqtt.h"

#if USE_MQTT
#include <Arduino.h>
#include <WiFi.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include <Preferences.h>   // one-time discovery-migration flag (see publishDeviceDiscovery)
#include "config.h"
#include "appconfig.h"
#include "oven.h"
#include "net_telnet.h"
#define Serial g_telnetSerial   // mirror this file's logs to the WiFi Telnet console too

static WiFiClient   wifiClient;
static PubSubClient mqtt(wifiClient);

static String T_BASE, T_STATE, T_AVAIL, T_SET_TEMP, T_SET_POWER, T_SET_MODE, T_SET_ONOFF, T_SET_FAN, T_SET_SILENT;
static String g_id, g_devName;

static bool     g_idReady     = false;
static String   g_discSerial;                  // serial number that was in the last discovery
static uint32_t g_lastPubSeq  = 0xFFFFFFFF;
static uint32_t g_lastPubMs   = 0;
static bool     g_lastAvailBle= false;
static const uint32_t HEARTBEAT_MS = 30000;

// Hydro-only settings exposed as HA number entities (read/write, °C). One table drives
// discovery, the state JSON and the command handling. object_id = state-JSON key = set/<id> topic.
struct HydroNum { const char* oid; const char* name; uint16_t reg; float mn, mx, step; bool needsPuffer; };
static const HydroNum HYDRO_NUMS[] = {
  {"hyst_amb_neg", "Ambient hysteresis -",        REG_HYST_AMB_NEG, 0, 20, 0.1f, false},
  {"hyst_amb_pos", "Ambient hysteresis +",        REG_HYST_AMB_POS, 0, 20, 0.1f, false},
  {"ss_neg",       "Start/Stop hysteresis -",     REG_HYST_SS_NEG,  0, 20, 0.1f, false},
  {"ss_pos",       "Start/Stop hysteresis +",     REG_HYST_SS_POS,  0, 20, 0.1f, false},
  {"pump_min_on",  "Circulation pump min. temp.", REG_PUMP_MIN_ON, 20, 90, 1.0f, false},
  {"water_set",    "Water temperature setpoint",  REG_SET_PUFFER,  30, 85, 1.0f, true },  // set_puffer
};
static const int HYDRO_NUM_CNT = sizeof(HYDRO_NUMS)/sizeof(HYDRO_NUMS[0]);
static bool hydroNumAvail(const HydroNum& h){ return !h.needsPuffer || g_caps.puffer; }
static float hydroVal(uint16_t reg){    // current OvenState value for a hydro-param register
  switch(reg){
    case REG_HYST_AMB_NEG: return g_oven.hystAmbNeg;
    case REG_HYST_AMB_POS: return g_oven.hystAmbPos;
    case REG_HYST_SS_NEG:  return g_oven.hystSSNeg;
    case REG_HYST_SS_POS:  return g_oven.hystSSPos;
    case REG_PUMP_MIN_ON:  return g_oven.pumpMinOn;
    case REG_SET_PUFFER:   return g_oven.pufferSet;
  } return NAN;
}

// ---- Identity / Topics ----------------------------------------------------
static void buildDevName(){
  // Name uses the same MAC as MQTT topic/ID (g_id) -> immediately correct, no waiting for serial.
  g_devName = String(DEVICE_NAME) + " (" + g_id + ")";
}
static bool buildIdentity(){
  String cfg = DEVICE_ID;                       // optional override from config.h
  String id  = cfg.length() ? cfg : g_ovenMac;  // otherwise: oven BLE MAC
  if (id.length() == 0) return false;           // no identity yet (BLE not connected)
  g_id        = id;
  T_BASE      = String("mcz/") + g_id;
  T_STATE     = T_BASE + "/state";
  T_AVAIL     = T_BASE + "/availability";
  T_SET_TEMP  = T_BASE + "/set/temp";
  T_SET_POWER = T_BASE + "/set/power";
  T_SET_MODE  = T_BASE + "/set/mode";
  T_SET_ONOFF = T_BASE + "/set/onoff";
  T_SET_FAN   = T_BASE + "/set/fan";
  T_SET_SILENT= T_BASE + "/set/silent";
  buildDevName();
  return true;
}

// ---- Helpers --------------------------------------------------------------
static void publishJson(const String& topic, JsonDocument& doc, bool retain){
  // static, NOT a stack local: the device-based discovery payload (one message describing
  // every entity) can run several KB, well beyond what's safe to put on the loop task's stack.
  static char buf[8192];
  size_t need = measureJson(doc);
  if (need >= sizeof(buf))
    Serial.printf("!! MQTT payload too large for %s (%u B needed, buffer=%u) - truncated!\n",
                  topic.c_str(), (unsigned)need, (unsigned)sizeof(buf));
  size_t n = serializeJson(doc, buf, sizeof(buf));
  bool ok = mqtt.publish(topic.c_str(), (const uint8_t*)buf, n, retain);
  if (!ok){ mqtt.loop(); delay(10); ok = mqtt.publish(topic.c_str(), (const uint8_t*)buf, n, retain); }
  if (!ok) Serial.printf("!! MQTT publish failed (%s, %u B) - buffer/network?\n", topic.c_str(), (unsigned)n);
  mqtt.loop(); delay(2);
}

// ---- HA MQTT discovery: single "device discovery" message (HA 2024.9+) -----------------
// Instead of one discovery payload PER entity (~20 separate retained messages, each repeating
// the full device{} block), everything is described once under homeassistant/device/<id>/config,
// with entities as sub-objects under "components". device{}/origin{} are written ONCE, and
// state_topic/availability_topic are declared ONCE at the root and inherited by every component
// that doesn't override them (see "Supported shared options" in the HA MQTT discovery docs).
// unique_id per entity is kept IDENTICAL to the old per-entity scheme on purpose, so existing
// HA history/statistics/dashboard references survive the switch unaffected.
static String oldDiscoTopic(const char* component, const char* objectId){       // legacy topic (pre-migration)
  return String(HA_DISCOVERY_PREFIX) + "/" + component + "/" + g_id + "_" + objectId + "/config";
}
static String deviceDiscoTopic(){
  return String(HA_DISCOVERY_PREFIX) + "/device/" + g_id + "/config";
}
// Every (component, object_id) pair ever published under the old per-entity discovery scheme.
// Used once to cleanly migrate to device-based discovery (see migrateOldDiscovery() below).
struct OldEnt { const char* comp; const char* oid; };
static const OldEnt OLD_ENTITIES[] = {
  {"climate","climate"}, {"number","power"}, {"select","fan"}, {"switch","silent"},
  {"sensor","boiler_temp"}, {"sensor","puffer_temp"},
  {"number","hyst_amb_neg"}, {"number","hyst_amb_pos"}, {"number","ss_neg"}, {"number","ss_pos"},
  {"number","pump_min_on"}, {"number","water_set"},
  {"sensor","fumes"}, {"sensor","board"}, {"sensor","room"}, {"sensor","fan_room"}, {"sensor","fan_comb"},
  {"sensor","active"}, {"sensor","phase"}, {"sensor","ignitions"},
  {"sensor","worktime"}, {"sensor","ptime1"}, {"sensor","ptime2"}, {"sensor","ptime3"},
  {"sensor","ptime4"}, {"sensor","ptime5"},
  {"sensor","alarm_last"}, {"sensor","alarm_hist"},
};
static const int OLD_ENTITIES_CNT = sizeof(OLD_ENTITIES)/sizeof(OLD_ENTITIES[0]);

// One-time (per device lifetime, tracked in NVS) migration handshake, per the official HA
// procedure: tell HA to unload each old single-component entity via {"migrate_discovery":true},
// then (right after this returns) the new device-based config gets published, then the old
// topics are cleared. Doing this only once (not on every reconnect) avoids needless MQTT churn.
static bool discoveryMigratedFlag(){
  Preferences p; p.begin("mcz", true); bool m = p.getBool("disc_v2", false); p.end(); return m;
}
static void setDiscoveryMigratedFlag(){
  Preferences p; p.begin("mcz", false); p.putBool("disc_v2", true); p.end();
}
static void migrateOldDiscovery(){
  Serial.println(">> One-time migration to HA device-based discovery...");
  for (int i=0;i<OLD_ENTITIES_CNT;i++)
    mqtt.publish(oldDiscoTopic(OLD_ENTITIES[i].comp, OLD_ENTITIES[i].oid).c_str(),
                 "{\"migrate_discovery\": true}", true);
  mqtt.loop(); delay(200);
}
static void cleanupOldDiscovery(){
  for (int i=0;i<OLD_ENTITIES_CNT;i++)
    mqtt.publish(oldDiscoTopic(OLD_ENTITIES[i].comp, OLD_ENTITIES[i].oid).c_str(), "", true);
  mqtt.loop();
}

// Add one "sensor" component under cmps[objectId]. state_topic/availability_topic are inherited
// from the discovery root -> not repeated here.
static void addSensorCmp(JsonObject cmps, const char* objectId, const char* name, const char* valTpl,
                         const char* unit, const char* devClass, const char* stateClass){
  JsonObject c = cmps[objectId].to<JsonObject>();
  c["p"]      = "sensor";
  c["name"]   = name;
  c["uniq_id"]= g_id + "_" + objectId;
  c["val_tpl"]= valTpl;
  if (unit && *unit)             c["unit_of_meas"] = unit;
  if (devClass && *devClass)     c["dev_cla"] = devClass;
  if (stateClass && *stateClass) c["stat_cla"] = stateClass;
}

static void publishDeviceDiscovery(){
  buildDevName();                 // always build display name from current serial number
  JsonDocument d;
  { // ---- shared device{} + origin{} (written ONCE, not per entity) ----
    JsonObject dev = d["dev"].to<JsonObject>();
    dev["ids"].to<JsonArray>().add(g_id);
    dev["name"] = g_devName;
    dev["mf"]   = "MCZ";
    String model = "Maestro";
    if (g_caps.detected){
      if (g_caps.bancaDati.length()) model += " " + g_caps.bancaDati;
      model += g_caps.hydro ? " (Hydro)" : " (Air)";
    }
    dev["mdl"] = model;
    if (g_ovenSerial.length()) dev["sn"] = g_ovenSerial;
    JsonObject o = d["o"].to<JsonObject>();
    o["name"] = "MCZ Maestro Bridge";
    o["sw"]   = "1.0";
  }
  d["avty_t"] = T_AVAIL;   // shared availability_topic: every component below inherits this
  d["stat_t"] = T_STATE;   // shared state_topic: every component below inherits this
  JsonObject cmps = d["cmps"].to<JsonObject>();

  { // climate
    // NOTE: unlike sensor/number/select/switch (whose schema field is literally named
    // "state_topic", auto-inherited from the shared "stat_t" set above), climate's readable
    // aspects live under DIFFERENTLY-NAMED topic keys (current_temperature_topic,
    // temperature_state_topic, mode_state_topic, preset_mode_state_topic). Those are NOT
    // covered by the shared root option and must stay explicit here, even in device-based
    // discovery, or HA has no idea where to read them from.
    JsonObject c = cmps["climate"].to<JsonObject>();
    c["p"] = "climate"; c["name"] = "Stove"; c["uniq_id"] = g_id + "_climate";
    c["curr_temp_t"]    = T_STATE;
    c["curr_temp_tpl"]  = "{{ value_json.room }}";
    c["temp_cmd_t"]     = T_SET_TEMP;
    c["temp_stat_t"]    = T_STATE;
    c["temp_stat_tpl"]  = "{{ value_json.setpoint }}";
    c["min_temp"] = TEMP_MIN_C; c["max_temp"] = TEMP_MAX_C; c["temp_step"] = 0.5;
    c["temp_unit"] = "C";
    JsonArray modes = c["modes"].to<JsonArray>(); modes.add("off"); modes.add("heat");
    c["mode_cmd_t"]   = T_SET_ONOFF;
    c["mode_stat_t"]  = T_STATE;
    c["mode_stat_tpl"]= "{{ 'heat' if value_json.running else 'off' }}";
    JsonArray pm = c["pr_modes"].to<JsonArray>();
    pm.add("Comfort"); pm.add("Overnight"); pm.add("Turbo"); pm.add("Auto"); pm.add("Manual");
    c["pr_mode_cmd_t"]  = T_SET_MODE;
    c["pr_mode_stat_t"] = T_STATE;
    // mode_name can be "?" (raw register value outside the known 0..4 enum, e.g. stale data
    // while Off/disconnected) -> only forward it if it's one of the declared preset_modes,
    // else '' (unknown), instead of forwarding "?" (HA would otherwise log "not a valid preset mode").
    c["pr_mode_val_tpl"] =
      "{% set v = value_json.mode_name %}"
      "{{ v if v in ['Comfort','Overnight','Turbo','Auto','Manual'] else '' }}";
  }
  { // number: power
    JsonObject c = cmps["power"].to<JsonObject>();
    c["p"] = "number"; c["name"] = "Power"; c["uniq_id"] = g_id + "_power";
    c["cmd_t"] = T_SET_POWER;
    // Register can report values outside 1..5 (e.g. stale/garbage while the stove is Off /
    // BLE not connected) -> map anything outside range to '' (unknown) instead of forwarding
    // an invalid value (HA would otherwise log "Invalid value ... range 1.0 - 5.0" repeatedly).
    c["val_tpl"] = "{% set v = value_json.power | int(-1) %}{{ v if (v >= 1 and v <= 5) else '' }}";
    c["min"] = 1; c["max"] = 5; c["step"] = 1; c["mode"] = "slider";
  }
  // Fan/Silent only exist if the stove actually has a controllable fan (capability scan).
  int fanLevels = g_caps.detected ? g_caps.fanLevels : 5;
  bool hasFan   = !g_caps.detected || g_caps.fanCount > 0;   // before detection: assume yes
  if (hasFan){ // select: fan level (Auto + 1..fanLevels, per detected hardware)
    JsonObject c = cmps["fan"].to<JsonObject>();
    c["p"] = "select"; c["name"] = "Fan"; c["uniq_id"] = g_id + "_fan";
    c["cmd_t"] = T_SET_FAN;
    // Live level 0x0324 (1..N) -> option; Auto is not distinguishable from the actual value.
    // Register can briefly report 0 (fan not yet spinning, e.g. during "Loading") which is
    // NOT a valid select option -> map anything outside 1..fanLevels to '' (unknown) instead
    // of forwarding an invalid value (HA would otherwise log "Invalid option" repeatedly).
    c["val_tpl"] = String("{% set v = value_json.fan_level | int(-1) %}{{ v if (v >= 1 and v <= ")
                   + fanLevels + ") else '' }}";
    JsonArray op = c["ops"].to<JsonArray>();
    op.add("Auto");
    for (int i=1;i<=fanLevels;i++){ char b[4]; snprintf(b,sizeof(b),"%d",i); op.add(b); }
  }
  if (hasFan){ // switch: silent mode
    JsonObject c = cmps["silent"].to<JsonObject>();
    c["p"] = "switch"; c["name"] = "Silent"; c["uniq_id"] = g_id + "_silent";
    c["cmd_t"] = T_SET_SILENT;
    // 'silent' key can be absent from the payload while flags is unknown (e.g. stove Off) ->
    // default(false) avoids the "'dict object' has no attribute 'silent'" template warning.
    c["val_tpl"] = "{{ 'ON' if value_json.silent | default(false) else 'OFF' }}";
    c["pl_on"] = "on"; c["pl_off"] = "off";
    c["stat_on"] = "ON"; c["stat_off"] = "OFF";
    c["ic"] = "mdi:volume-off";
  }
  // Hydro-only sensors: published solely when the capability scan found the circuit.
  if (g_caps.hydro)  addSensorCmp(cmps, "boiler_temp", "Boiler temperature", "{{ value_json.boiler_temp }}", "°C", "temperature", "measurement");
  if (g_caps.puffer) addSensorCmp(cmps, "puffer_temp", "Buffer temperature", "{{ value_json.puffer_temp }}", "°C", "temperature", "measurement");
  if (g_caps.hydro) for (int i=0;i<HYDRO_NUM_CNT;i++){   // hydro settings as writable number entities
    const HydroNum& h = HYDRO_NUMS[i];
    if (!hydroNumAvail(h)) continue;                     // e.g. water setpoint only if a puffer exists
    JsonObject c = cmps[h.oid].to<JsonObject>();
    c["p"] = "number"; c["name"] = h.name; c["uniq_id"] = g_id + "_" + h.oid;
    c["cmd_t"] = T_BASE + "/set/" + h.oid;
    c["val_tpl"] = String("{{ value_json.") + h.oid + " }}";
    c["min"] = h.mn; c["max"] = h.mx; c["step"] = h.step;
    c["unit_of_meas"] = "°C"; c["mode"] = "box"; c["ent_cat"] = "config";
  }
  addSensorCmp(cmps, "fumes", "Flue gas temperature", "{{ value_json.fumes }}",  "°C", "temperature", "measurement");
  addSensorCmp(cmps, "board", "Control board temperature", "{{ value_json.board }}", "°C", "temperature", "measurement");
  addSensorCmp(cmps, "room",  "Room temperature",     "{{ value_json.room }}",   "°C", "temperature", "measurement");
  addSensorCmp(cmps, "fan_room", "Flue gas fan",  "{{ value_json.fan_room }}", "rpm", "", "measurement");
  addSensorCmp(cmps, "fan_comb", "Combustion fan", "{{ value_json.fan_comb }}", "rpm", "", "measurement");
  addSensorCmp(cmps, "active", "Active",             "{{ value_json.active }}", "", "", "measurement");
  addSensorCmp(cmps, "phase", "Phase", "{{ value_json.phase_name }}", "", "", "");
  addSensorCmp(cmps, "ignitions", "Ignitions", "{{ value_json.ignitions }}", "", "", "total_increasing");
  addSensorCmp(cmps, "worktime_h", "Total working time", "{{ value_json.worktime_h }}", "h", "", "total_increasing");
  // Explicit object_id keeps the Home Assistant entity IDs stable and avoids _2 suffixes.
  cmps["worktime_h"]["obj_id"] = "total_working_time";
  for (int i=1;i<=5;i++){
    char oid[12], name[20], tpl[40], objId[20];
    snprintf(oid,   sizeof(oid),   "ptime%d_h", i);
    snprintf(name,  sizeof(name),  "Time power %d", i);
    snprintf(tpl,   sizeof(tpl),   "{{ value_json.pt%d_h }}", i);
    addSensorCmp(cmps, oid, name, tpl, "h", "", "total_increasing");
    snprintf(objId, sizeof(objId), "time_power_%d", i);
    cmps[oid]["obj_id"] = objId;
  }
  // Alarm log (all ovens): last alarm + history (raw Axx codes; meanings are model-specific)
  { JsonObject c = cmps["alarm_last"].to<JsonObject>();
    c["p"]="sensor"; c["name"]="Last alarm"; c["uniq_id"]=g_id+"_alarm_last";
    c["val_tpl"]="{{ value_json.alarm_last }}"; c["ic"]="mdi:alert-circle-outline";
  }
  { JsonObject c = cmps["alarm_hist"].to<JsonObject>();
    c["p"]="sensor"; c["name"]="Alarm history"; c["uniq_id"]=g_id+"_alarm_hist";
    c["val_tpl"]="{{ value_json.alarm_hist }}";
    c["json_attr_t"]=T_STATE;
    c["json_attr_tpl"]="{{ {'codes': value_json.alarm_codes} | tojson }}";
    c["ic"]="mdi:history";
  }

  if (!discoveryMigratedFlag()){          // one-time: unload legacy per-entity discovery cleanly
    migrateOldDiscovery();
    publishJson(deviceDiscoTopic(), d, true);
    delay(200);
    cleanupOldDiscovery();
    setDiscoveryMigratedFlag();
    Serial.println(">> Migration to device-based discovery done.");
  } else {
    publishJson(deviceDiscoTopic(), d, true);
  }
}

static void publishState(){
  JsonDocument d;
  if (!isnan(g_oven.roomC))     d["room"]     = roundf(g_oven.roomC*10)/10.0;
  if (!isnan(g_oven.setpointC)) d["setpoint"] = roundf(g_oven.setpointC*10)/10.0;
  if (!isnan(g_oven.boardC))    d["board"]    = roundf(g_oven.boardC*10)/10.0;
  if (!isnan(g_oven.fumesC))    d["fumes"]    = roundf(g_oven.fumesC*10)/10.0;
  if (g_oven.power>=0)          d["power"]    = g_oven.power;
  if (g_oven.mode>=0){ d["mode"] = g_oven.mode; d["mode_name"] = modeName(g_oven.mode); }
  if (g_oven.phase>=0){ d["phase"] = g_oven.phase; d["running"] = ovenRunning(); }
  if (g_oven.state>=0){
    d["state"] = g_oven.state;                 // raw fine-phase code (0x0320)
    const char* sn = stateName(g_oven.state);
    if (sn[0]) d["phase_name"] = sn;
    else { char b[12]; snprintf(b,sizeof(b),"0x%04X",(unsigned)g_oven.state); d["phase_name"] = b; }
  }
  if (g_oven.fanLevel>=0)      d["fan_level"] = g_oven.fanLevel;
  if (g_oven.fanRoom>=0)       d["fan_room"]  = g_oven.fanRoom;   // flue gas fan RPM
  if (g_oven.fanComb>=0)       d["fan_comb"]  = g_oven.fanComb;   // combustion fan RPM
  if (g_oven.active>=0)        d["active"]    = g_oven.active;    // app value "active"
  if (g_oven.flags>=0){ d["chrono"] = (g_oven.flags>>6)&1; d["silent"] = (g_oven.flags>>5)&1; }
  if (g_oven.ignitions>=0)   d["ignitions"]    = g_oven.ignitions;
  // The oven registers are 32-bit minute counters. Publish hours to Home Assistant.
  // Example: 239453 min -> 3990.88 h.
  if (g_oven.worktimeMinutes>=0) d["worktime_h"] = roundf(g_oven.worktimeMinutes/60.0f*100.0f)/100.0f;
  for (int i=0;i<5;i++) if (g_oven.powerTimeMinutes[i]>=0){
    char k[8]; snprintf(k,sizeof(k),"pt%d_h",i+1);
    d[k] = roundf(g_oven.powerTimeMinutes[i]/60.0f*100.0f)/100.0f;
  }
  d["ble"] = g_oven.bleOnline;
  d["seq"] = g_oven.seq;
  if (g_ovenSerial.length()) d["serial"] = g_ovenSerial;
  if (g_oven.alarmHist[0] >= 0){                 // alarm log (raw Axx codes, newest first)
    char la[8];
    if (g_oven.alarmHist[0] > 0) snprintf(la,sizeof(la),"A%d",g_oven.alarmHist[0]); else strncpy(la,"none",sizeof(la));
    d["alarm_last"] = la;
    String hist; JsonArray codes = d["alarm_codes"].to<JsonArray>();
    for (int k=0;k<10 && g_oven.alarmHist[k]>=0;k++){
      if (g_oven.alarmHist[k] <= 0) continue;
      if (hist.length()) hist += ",";
      hist += "A"; hist += g_oven.alarmHist[k];
      codes.add(g_oven.alarmHist[k]);
    }
    d["alarm_hist"] = hist;
  }
  if (g_caps.detected){                          // detected hardware capabilities
    d["hydro"]      = g_caps.hydro;
    d["fan_count"]  = g_caps.fanCount;
    d["fan_levels"] = g_caps.fanLevels;
    d["boiler"]     = g_caps.boiler;
    d["puffer"]     = g_caps.puffer;
    if (g_caps.bancaDati.length()) d["model"] = g_caps.bancaDati;
    // Hydro live temperatures (only meaningful / published on water-heating stoves)
    if (g_caps.hydro  && !isnan(g_oven.boilerC)) d["boiler_temp"] = roundf(g_oven.boilerC*10)/10.0;
    if (g_caps.puffer && !isnan(g_oven.pufferC)) d["puffer_temp"] = roundf(g_oven.pufferC*10)/10.0;
    if (g_caps.hydro) for (int i=0;i<HYDRO_NUM_CNT;i++){   // hydro settings
      float v = hydroVal(HYDRO_NUMS[i].reg);
      if (!isnan(v)) d[HYDRO_NUMS[i].oid] = roundf(v*10)/10.0;
    }
  }
  publishJson(T_STATE, d, true);
}

static void publishAvail(bool online){
  mqtt.publish(T_AVAIL.c_str(), online ? "online" : "offline", true);
}

// ---- Command callback -----------------------------------------------------
static int parseMode(const String& s){
  String t = s; t.trim(); t.toLowerCase();
  if (t=="0" || t=="manuell" || t=="manual")             return 0;
  if (t=="1" || t=="auto")                               return 1;
  if (t=="2" || t=="nacht" || t=="night" || t=="overnight") return 2;
  if (t=="3" || t=="comfort" || t=="komfort")            return 3;
  if (t=="4" || t=="turbo")                              return 4;
  return -1;
}
static void onMqtt(char* topic, byte* payload, unsigned int len){
  String t(topic), msg; msg.reserve(len);
  for (unsigned i=0;i<len;i++) msg += (char)payload[i];
  msg.trim();
  if      (t == T_SET_TEMP)  ovenSetTemp(msg.toFloat());
  else if (t == T_SET_POWER) ovenSetPower(msg.toInt());
  else if (t == T_SET_MODE)  ovenSetMode(parseMode(msg));
  else if (t == T_SET_ONOFF){ String m=msg; m.toLowerCase();
    if (m=="heat"||m=="on"||m=="1")  ovenSetOnOff(true);
    else if (m=="off"||m=="0")       ovenSetOnOff(false);
  }
  else if (t == T_SET_FAN){ String m=msg; m.toLowerCase();
    ovenSetFan(m=="auto" ? 0 : msg.toInt());        // "Auto"/"0" -> Auto, "1".."5" -> level
  }
  else if (t == T_SET_SILENT){ String m=msg; m.toLowerCase();
    if (m=="on"||m=="1"||m=="true")   ovenSetSilent(true);
    else if (m=="off"||m=="0"||m=="false") ovenSetSilent(false);
  }
  else for (int i=0;i<HYDRO_NUM_CNT;i++){                  // hydro settings (set/<oid>)
    if (t == T_BASE + "/set/" + HYDRO_NUMS[i].oid){ ovenSetTempParam(HYDRO_NUMS[i].reg, msg.toFloat()); break; }
  }
}

static bool mqttConnect(){
  String cid = String("mcz-") + g_id;
  bool ok;
  if (g_cfg.mqttUser.length() > 0)
    ok = mqtt.connect(cid.c_str(), g_cfg.mqttUser.c_str(), g_cfg.mqttPass.c_str(),
                      T_AVAIL.c_str(), 0, true, "offline");
  else
    ok = mqtt.connect(cid.c_str(), nullptr, nullptr, T_AVAIL.c_str(), 0, true, "offline");
  if (!ok) return false;
  Serial.printf(">> MQTT connected (id=%s).\n", g_id.c_str());
  mqtt.subscribe(T_SET_TEMP.c_str());
  mqtt.subscribe(T_SET_POWER.c_str());
  mqtt.subscribe(T_SET_MODE.c_str());
  mqtt.subscribe(T_SET_ONOFF.c_str());
  mqtt.subscribe(T_SET_FAN.c_str());
  mqtt.subscribe(T_SET_SILENT.c_str());
  for (int i=0;i<HYDRO_NUM_CNT;i++)                 // hydro settings (harmless if not a hydro oven)
    mqtt.subscribe((T_BASE + "/set/" + HYDRO_NUMS[i].oid).c_str());
  publishDeviceDiscovery(); g_discSerial = g_ovenSerial;  // name = MAC -> immediately correct
  publishAvail(g_oven.bleOnline); g_lastAvailBle = g_oven.bleOnline;
  publishState();                     // send state IMMEDIATELY (do not wait for the next tick)
  g_lastPubSeq = g_oven.seq; g_lastPubMs = millis();
  return true;
}

// ---- API ------------------------------------------------------------------
void netBegin(){
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(true);                // BLE+WiFi coexistence: modem sleep MUST be on
  WiFi.begin(g_cfg.wifiSsid.c_str(), g_cfg.wifiPass.c_str());
  Serial.printf(">> WiFi: connecting to '%s' ...\n", g_cfg.wifiSsid.c_str());
  mqtt.setServer(g_cfg.mqttHost.c_str(), g_cfg.mqttPort);
  // The single device-based discovery message (all ~20 entities in one payload) runs several
  // KB; PubSubClient's default 256 B buffer would silently drop it, so size generously.
  mqtt.setBufferSize(8192);
  mqtt.setCallback(onMqtt);
}

void netTick(){
  static uint32_t lastWifi=0, lastMqtt=0;
  static bool wifiWasUp=false;
  uint32_t now = millis();
  if (WiFi.status() != WL_CONNECTED){
    wifiWasUp = false;
    if (now - lastWifi > 5000){ lastWifi = now; WiFi.reconnect(); }
    return;
  }
  if (!wifiWasUp){    // log once per (re)connect, independent of the oven/BLE state
    wifiWasUp = true;
    Serial.printf(">> WiFi connected, IP=%s (telnet to it, port 23, for the console)\n",
                  WiFi.localIP().toString().c_str());
  }
  if (!g_idReady){                    // wait for identity (oven MAC)
    if (buildIdentity()){ g_idReady = true; Serial.printf(">> MQTT ID: %s\n", g_id.c_str()); }
    else return;
  }
  if (!mqtt.connected()){
    if (now - lastMqtt > 3000){ lastMqtt = now; mqttConnect(); }
    return;
  }
  mqtt.loop();
  if (g_oven.bleOnline != g_lastAvailBle){ publishAvail(g_oven.bleOnline); g_lastAvailBle = g_oven.bleOnline; }
  // Serial number AND the capability scan only complete after the connect -> re-publish
  // discovery once each becomes known (serial_number metadata; fan entity/level count
  // matched to the detected hardware).
  static bool g_discCaps = false;
  if (g_ovenSerial != g_discSerial || g_caps.detected != g_discCaps){
    publishDeviceDiscovery(); g_discSerial = g_ovenSerial; g_discCaps = g_caps.detected;
  }
  // Publish on change, but throttled to >=1s apart (less heap churn / MQTT load over hours);
  // plus a 30s heartbeat so HA stays fresh even without changes.
  bool changed = (g_oven.seq != g_lastPubSeq) && (now - g_lastPubMs >= 1000);
  if (changed || now - g_lastPubMs > HEARTBEAT_MS){
    publishState(); g_lastPubSeq = g_oven.seq; g_lastPubMs = now;
  }
}

bool netWifiUp(){ return WiFi.status()==WL_CONNECTED; }
bool netMqttUp(){ return mqtt.connected(); }
#else
void netBegin(){}
void netTick(){}
bool netWifiUp(){ return false; }
bool netMqttUp(){ return false; }
#endif
