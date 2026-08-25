#include "wifi/HomeAssistant.h"

#ifdef HOME_ASSISTANT

#if defined(ESP32)
  #include <WiFi.h>
#elif defined(ESP8266)
  #include <ESP8266WiFi.h>
#endif
#include <StreamUtils.h>
#include <version.h>

// Heat pump values are passed through to Home Assistant verbatim for fan and
// vane, they are already readable. Only power/mode need merging, because HA
// folds "off" into the mode list while the heat pump keeps them separate.
static const char *HA_MODES[] = {"off", "heat", "cool", "dry", "fan_only", "auto"};
static const char *HA_FAN_MODES[] = {"AUTO", "QUIET", "1", "2", "3", "4"};
static const char *HA_SWING_MODES[] = {"AUTO", "1", "2", "3", "4", "5", "SWING"};
static const char *HA_SWING_H_MODES[] = {"<<", "<", "|", ">", ">>", "<>", "SWING"};

// Lowercases and strips anything that isn't safe in an MQTT topic, collapsing
// runs of separators into single underscores: "Living Room AC" -> "living_room_ac"
static void sanitizeNodeId(const char *in, char *out, size_t outLen) {
  size_t j = 0;
  bool separator = false;
  for (size_t i = 0; in[i] != 0 && j < outLen - 1; i++) {
    char c = in[i];
    if (c >= 'A' && c <= 'Z') {
      c = c - 'A' + 'a';
    }
    if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) {
      out[j++] = c;
      separator = false;
    } else if (!separator && j > 0) {
      out[j++] = '_';
      separator = true;
    }
  }
  while (j > 0 && out[j - 1] == '_') {
    j--;
  }
  out[j] = 0;
}

CHomeAssistant::CHomeAssistant(PubSubClient *mqtt, ISensorProvider *sensorProvider)
: mqtt(mqtt), sensorProvider(sensorProvider),
  tsSettingsPublished(0), tsStatusPublished(0), tsLastStatePublish(0),
  tsLastChangeCheck(0) {
  nodeId[0] = 0;
  refreshNodeId();
}

bool CHomeAssistant::refreshNodeId() {
  char previous[HA_NODE_ID_LEN];
  strncpy(previous, nodeId, sizeof(previous));
  previous[sizeof(previous) - 1] = 0;

  sanitizeNodeId(configuration.name, nodeId, sizeof(nodeId));
  if (strlen(nodeId) == 0) {
    // Unnamed device, fall back to something unique rather than an empty topic
    snprintf(nodeId, sizeof(nodeId), "mhvac_%u", CONFIG_getDeviceId());
  }

  buildTopics();
  return strcmp(previous, nodeId) != 0;
}

void CHomeAssistant::buildTopics() {
  snprintf(baseTopic, sizeof(baseTopic), "%s/%s", configuration.mqttTopic, nodeId);
  snprintf(availabilityTopic, sizeof(availabilityTopic), "%s/availability", baseTopic);
  snprintf(stateTopic, sizeof(stateTopic), "%s/state", baseTopic);
  snprintf(commandTopicFilter, sizeof(commandTopicFilter), "%s/cmd/#", baseTopic);
}

void CHomeAssistant::discoveryTopic(char *out, size_t outLen, const char *component,
                                    const char *node, const char *objectId) {
  snprintf(out, outLen, "%s/%s/%s/%s/config", HA_DISCOVERY_PREFIX, component, node, objectId);
}

// Streams the document straight onto the socket. PubSubClient's default 256
// byte buffer can't hold a discovery payload, and growing it would cost RAM
// permanently, so publish in chunks the way postSensorUpdate does.
void CHomeAssistant::publishJson(const char *topic, JsonDocument &doc, bool retain) {
  size_t len = measureJson(doc);
  if (!mqtt->beginPublish(topic, len, retain)) {
    Log.warningln("Failed to begin publish of %u bytes to '%s'", len, topic);
    return;
  }
  BufferingPrint buffered(*mqtt, 32);
  serializeJson(doc, buffered);
  buffered.flush();
  if (!mqtt->endPublish()) {
    Log.warningln("Failed to finish publish to '%s'", topic);
    return;
  }
  Log.verboseln("Published %u bytes to '%s'", len, topic);
}

void CHomeAssistant::addDeviceBlock(JsonDocument &doc, bool full) {
  JsonObject device = doc["device"].to<JsonObject>();
  char identifier[64];
  snprintf(identifier, sizeof(identifier), "mhvac_%u", CONFIG_getDeviceId());
  device["identifiers"].to<JsonArray>().add(identifier);

  if (full) {
    device["name"] = configuration.name;
    device["manufacturer"] = "Mitsubishi Electric";
    device["model"] = "Heat Pump (CN105)";
    device["sw_version"] = VERSION;
    device["configuration_url"] = String("http://") + WiFi.localIP().toString();
  }
}

void CHomeAssistant::onMQTTConnected() {
  if (!strlen(configuration.mqttTopic)) {
    return;
  }

  refreshNodeId();

  mqtt->publish(availabilityTopic, "online", true);

  bool r = mqtt->subscribe(commandTopicFilter);
  Log.noticeln("Subscribed to Home Assistant commands on '%s' success = %T", commandTopicFilter, r);

  publishDiscovery();
  publishState(true);
}

void CHomeAssistant::publishDiscovery() {
  publishClimateDiscovery();
  publishSensorDiscovery("room_temperature", "Room temperature",
                         "{{ value_json.current_temperature }}", "temperature",
                         "\xC2\xB0" "C", "measurement", false);
  publishSensorDiscovery("wifi_signal", "WiFi signal",
                         "{{ value_json.wifi_rssi }}", "signal_strength",
                         "dBm", "measurement", true);
  publishSensorDiscovery("uptime", "Uptime",
                         "{{ value_json.uptime }}", "duration",
                         "s", NULL, true);
}

void CHomeAssistant::publishClimateDiscovery() {
  JsonDocument doc;

  // A null name makes Home Assistant fall back to the device name, so the
  // thermostat shows up as just "Living Room AC" rather than "... Thermostat"
  doc["name"] = (char*)NULL;

  char uniqueId[64];
  snprintf(uniqueId, sizeof(uniqueId), "mhvac_%u_climate", CONFIG_getDeviceId());
  doc["unique_id"] = uniqueId;

  doc["availability_topic"] = availabilityTopic;
  doc["payload_available"] = "online";
  doc["payload_not_available"] = "offline";

  doc["temperature_unit"] = "C";
  doc["min_temp"] = HA_MIN_TEMP;
  doc["max_temp"] = HA_MAX_TEMP;
  doc["temp_step"] = HA_TEMP_STEP;

  doc["current_temperature_topic"] = stateTopic;
  doc["current_temperature_template"] = "{{ value_json.current_temperature }}";

  doc["action_topic"] = stateTopic;
  doc["action_template"] = "{{ value_json.action }}";

  char commandTopic[HA_TOPIC_LEN];

  doc["mode_state_topic"] = stateTopic;
  doc["mode_state_template"] = "{{ value_json.mode }}";
  snprintf(commandTopic, sizeof(commandTopic), "%s/cmd/mode", baseTopic);
  doc["mode_command_topic"] = commandTopic;
  JsonArray modes = doc["modes"].to<JsonArray>();
  for (uint8_t i = 0; i < sizeof(HA_MODES) / sizeof(HA_MODES[0]); i++) {
    modes.add(HA_MODES[i]);
  }

  doc["temperature_state_topic"] = stateTopic;
  doc["temperature_state_template"] = "{{ value_json.temperature }}";
  char temperatureCommandTopic[HA_TOPIC_LEN];
  snprintf(temperatureCommandTopic, sizeof(temperatureCommandTopic), "%s/cmd/temperature", baseTopic);
  doc["temperature_command_topic"] = temperatureCommandTopic;

  doc["fan_mode_state_topic"] = stateTopic;
  doc["fan_mode_state_template"] = "{{ value_json.fan }}";
  char fanCommandTopic[HA_TOPIC_LEN];
  snprintf(fanCommandTopic, sizeof(fanCommandTopic), "%s/cmd/fan", baseTopic);
  doc["fan_mode_command_topic"] = fanCommandTopic;
  JsonArray fanModes = doc["fan_modes"].to<JsonArray>();
  for (uint8_t i = 0; i < sizeof(HA_FAN_MODES) / sizeof(HA_FAN_MODES[0]); i++) {
    fanModes.add(HA_FAN_MODES[i]);
  }

  doc["swing_mode_state_topic"] = stateTopic;
  doc["swing_mode_state_template"] = "{{ value_json.swing }}";
  char swingCommandTopic[HA_TOPIC_LEN];
  snprintf(swingCommandTopic, sizeof(swingCommandTopic), "%s/cmd/swing", baseTopic);
  doc["swing_mode_command_topic"] = swingCommandTopic;
  JsonArray swingModes = doc["swing_modes"].to<JsonArray>();
  for (uint8_t i = 0; i < sizeof(HA_SWING_MODES) / sizeof(HA_SWING_MODES[0]); i++) {
    swingModes.add(HA_SWING_MODES[i]);
  }

  // Horizontal vane, requires Home Assistant 2024.12 or newer. Older versions
  // ignore these keys and simply don't render the control.
  doc["swing_horizontal_mode_state_topic"] = stateTopic;
  doc["swing_horizontal_mode_state_template"] = "{{ value_json.swing_h }}";
  char swingHCommandTopic[HA_TOPIC_LEN];
  snprintf(swingHCommandTopic, sizeof(swingHCommandTopic), "%s/cmd/swing_h", baseTopic);
  doc["swing_horizontal_mode_command_topic"] = swingHCommandTopic;
  JsonArray swingHModes = doc["swing_horizontal_modes"].to<JsonArray>();
  for (uint8_t i = 0; i < sizeof(HA_SWING_H_MODES) / sizeof(HA_SWING_H_MODES[0]); i++) {
    swingHModes.add(HA_SWING_H_MODES[i]);
  }

  addDeviceBlock(doc, true);

  char topic[HA_TOPIC_LEN];
  discoveryTopic(topic, sizeof(topic), "climate", nodeId, "thermostat");
  publishJson(topic, doc, true);
}

void CHomeAssistant::publishSensorDiscovery(const char *objectId, const char *name,
                                            const char *valueTemplate, const char *deviceClass,
                                            const char *unit, const char *stateClass,
                                            bool diagnostic) {
  JsonDocument doc;

  doc["name"] = name;

  char uniqueId[64];
  snprintf(uniqueId, sizeof(uniqueId), "mhvac_%u_%s", CONFIG_getDeviceId(), objectId);
  doc["unique_id"] = uniqueId;

  doc["state_topic"] = stateTopic;
  doc["value_template"] = valueTemplate;
  doc["availability_topic"] = availabilityTopic;
  doc["payload_available"] = "online";
  doc["payload_not_available"] = "offline";

  if (deviceClass != NULL) {
    doc["device_class"] = deviceClass;
  }
  if (unit != NULL) {
    doc["unit_of_measurement"] = unit;
  }
  if (stateClass != NULL) {
    doc["state_class"] = stateClass;
  }
  if (diagnostic) {
    doc["entity_category"] = "diagnostic";
  }

  addDeviceBlock(doc, false);

  char topic[HA_TOPIC_LEN];
  discoveryTopic(topic, sizeof(topic), "sensor", nodeId, objectId);
  publishJson(topic, doc, true);
}

// Retracting a retained config removes the entity from Home Assistant. Used
// when the device name, and therefore the node id, changes.
void CHomeAssistant::clearDiscovery(const char *staleNodeId) {
  if (staleNodeId == NULL || strlen(staleNodeId) == 0 || !mqtt->connected()) {
    return;
  }

  const char *components[] = {"climate", "sensor", "sensor", "sensor"};
  const char *objectIds[] = {"thermostat", "room_temperature", "wifi_signal", "uptime"};

  char topic[HA_TOPIC_LEN];
  for (uint8_t i = 0; i < 4; i++) {
    discoveryTopic(topic, sizeof(topic), components[i], staleNodeId, objectIds[i]);
    mqtt->publish(topic, (const uint8_t*)NULL, 0, true);
    Log.noticeln("Retracted stale discovery config '%s'", topic);
  }

  char staleAvailability[HA_TOPIC_LEN];
  snprintf(staleAvailability, sizeof(staleAvailability), "%s/%s/availability",
           configuration.mqttTopic, staleNodeId);
  mqtt->publish(staleAvailability, (const uint8_t*)NULL, 0, true);
}

void CHomeAssistant::publishState(bool force) {
  if (!strlen(configuration.mqttTopic) || !mqtt->connected()) {
    return;
  }

  if (!force && millis() - tsLastStatePublish < HA_STATE_MIN_INTERVAL_MS) {
    return;
  }
  tsLastStatePublish = millis();

  JsonDocument &ac = sensorProvider->getACSettings();
  JsonDocument doc;

  const char *power = ac["power"].is<const char*>() ? ac["power"].as<const char*>() : NULL;
  const char *mode = ac["mode"].is<const char*>() ? ac["mode"].as<const char*>() : NULL;
  bool off = (power == NULL) || (strcmp(power, "OFF") == 0);

  // Heat pump mode -> Home Assistant mode, with power folded in as "off"
  const char *haMode = "off";
  if (!off && mode != NULL) {
    if (strcmp(mode, "HEAT") == 0)      haMode = "heat";
    else if (strcmp(mode, "COOL") == 0) haMode = "cool";
    else if (strcmp(mode, "DRY") == 0)  haMode = "dry";
    else if (strcmp(mode, "FAN") == 0)  haMode = "fan_only";
    else if (strcmp(mode, "AUTO") == 0) haMode = "auto";
  }
  doc["mode"] = haMode;

  float target = ac["temperature"].is<float>() ? ac["temperature"].as<float>() : 0;
  float room = ac["roomTemperature"].is<float>() ? ac["roomTemperature"].as<float>() : 0;
  bool operating = ac["operating"].is<bool>() ? ac["operating"].as<bool>() : false;

  // hvac_action drives the "Heating"/"Cooling" label on the thermostat card
  const char *action = "off";
  if (!off) {
    if (!operating) {
      action = "idle";
    } else if (mode == NULL) {
      action = "idle";
    } else if (strcmp(mode, "HEAT") == 0) {
      action = "heating";
    } else if (strcmp(mode, "COOL") == 0) {
      action = "cooling";
    } else if (strcmp(mode, "DRY") == 0) {
      action = "drying";
    } else if (strcmp(mode, "FAN") == 0) {
      action = "fan";
    } else if (strcmp(mode, "AUTO") == 0) {
      // The unit doesn't report which way it's driving in AUTO, infer it
      action = room > target ? "cooling" : "heating";
    }
  }
  doc["action"] = action;

  if (target > 0) {
    doc["temperature"] = target;
  }
  if (ac["roomTemperature"].is<float>()) {
    doc["current_temperature"] = room;
  }

  if (ac["fan"].is<const char*>())      doc["fan"] = ac["fan"];
  if (ac["vane"].is<const char*>())     doc["swing"] = ac["vane"];
  if (ac["wideVane"].is<const char*>()) doc["swing_h"] = ac["wideVane"];

  doc["wifi_rssi"] = WiFi.RSSI();
  doc["uptime"] = CONFIG_getUpTime() / 1000;

  publishJson(stateTopic, doc, true);

  tsSettingsPublished = ac["tsHPSettingsUpdated"].is<unsigned long>()
    ? ac["tsHPSettingsUpdated"].as<unsigned long>() : 0;
  tsStatusPublished = ac["tsHPStatusUpdated"].is<unsigned long>()
    ? ac["tsHPStatusUpdated"].as<unsigned long>() : 0;
}

void CHomeAssistant::loop() {
  if (!strlen(configuration.mqttTopic) || !mqtt->connected()) {
    return;
  }

  // Rebuilding the settings document is not free, so sample it on an interval
  // rather than on every pass of the main loop.
  if (millis() - tsLastChangeCheck < HA_CHANGE_POLL_MS) {
    return;
  }
  tsLastChangeCheck = millis();

  // The heat pump reports changes made at the IR remote too, so watch the
  // update timestamps and push state as soon as anything moves.
  JsonDocument &ac = sensorProvider->getACSettings();
  unsigned long tsSettings = ac["tsHPSettingsUpdated"].is<unsigned long>()
    ? ac["tsHPSettingsUpdated"].as<unsigned long>() : 0;
  unsigned long tsStatus = ac["tsHPStatusUpdated"].is<unsigned long>()
    ? ac["tsHPStatusUpdated"].as<unsigned long>() : 0;

  if (tsSettings != tsSettingsPublished || tsStatus != tsStatusPublished) {
    publishState(false);
  }
}

bool CHomeAssistant::handleCommand(const char *topic, const uint8_t *payload, unsigned int length) {
  char prefix[HA_TOPIC_LEN];
  snprintf(prefix, sizeof(prefix), "%s/cmd/", baseTopic);
  size_t prefixLen = strlen(prefix);

  if (strncmp(topic, prefix, prefixLen) != 0) {
    return false;
  }

  const char *field = topic + prefixLen;

  char value[32];
  unsigned int copyLen = length < sizeof(value) - 1 ? length : sizeof(value) - 1;
  memcpy(value, payload, copyLen);
  value[copyLen] = 0;

  Log.noticeln("Home Assistant command '%s' = '%s'", field, value);
  applyACChange(field, value);
  return true;
}

bool CHomeAssistant::applyACChange(const char *field, const char *value) {
  // setACSettings pushes every field to the unit, so start from the current
  // settings and change only what Home Assistant asked for.
  JsonDocument ac = sensorProvider->getACSettings();

  if (strcmp(field, "mode") == 0) {
    if (strcmp(value, "off") == 0) {
      ac["power"] = "OFF";
    } else {
      ac["power"] = "ON";
      if (strcmp(value, "heat") == 0)          ac["mode"] = "HEAT";
      else if (strcmp(value, "cool") == 0)     ac["mode"] = "COOL";
      else if (strcmp(value, "dry") == 0)      ac["mode"] = "DRY";
      else if (strcmp(value, "fan_only") == 0) ac["mode"] = "FAN";
      else if (strcmp(value, "auto") == 0)     ac["mode"] = "AUTO";
      else {
        Log.warningln("Unknown Home Assistant mode '%s'", value);
        return false;
      }
    }
  } else if (strcmp(field, "temperature") == 0) {
    // Discovery declares Celsius, so Home Assistant always sends Celsius here
    float t = atof(value);
    if (t < HA_MIN_TEMP || t > HA_MAX_TEMP) {
      Log.warningln("Ignoring out of range temperature '%s'", value);
      return false;
    }
    ac["temperature"] = t;
  } else if (strcmp(field, "fan") == 0) {
    ac["fan"] = value;
  } else if (strcmp(field, "swing") == 0) {
    ac["vane"] = value;
  } else if (strcmp(field, "swing_h") == 0) {
    ac["wideVane"] = value;
  } else {
    Log.warningln("Unknown Home Assistant command field '%s'", field);
    return false;
  }

  bool ok = sensorProvider->setACSettings(ac);
  if (!ok) {
    Log.warningln("Failed to apply Home Assistant command '%s'", field);
  }

  // Echo the resulting state back so HA doesn't sit on an optimistic value
  publishState(true);
  return ok;
}

#endif // HOME_ASSISTANT
