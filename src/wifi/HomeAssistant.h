#pragma once

#include "Configuration.h"

#ifdef HOME_ASSISTANT

#include <PubSubClient.h>
#include <ArduinoJson.h>

#include "wifi/SensorProvider.h"

// Home Assistant MQTT discovery for the `climate` component.
//
// Each unit publishes under a node id derived from the user configurable device
// name, so several ACs can share one MQTT topic prefix:
//
//   <mqttTopic>/<nodeId>/state          retained climate + sensor state (JSON)
//   <mqttTopic>/<nodeId>/availability   LWT backed online/offline
//   <mqttTopic>/<nodeId>/cmd/#          commands from Home Assistant
//
// Entity unique ids are anchored to the chip id rather than the name, so
// renaming a unit keeps its history in the HA entity registry.
class CHomeAssistant {

public:
  CHomeAssistant(PubSubClient *mqtt, ISensorProvider *sensorProvider);

  // Recomputes the node id and topics from configuration.name.
  // Returns true when the node id changed.
  bool refreshNodeId();

  const char *getNodeId() { return nodeId; }
  const char *getBaseTopic() { return baseTopic; }
  const char *getAvailabilityTopic() { return availabilityTopic; }

  // Called right after the MQTT session comes up: announces availability,
  // subscribes to the command topics and (re)publishes discovery.
  void onMQTTConnected();

  // Retracts the retained discovery configs for a node id we no longer use.
  void clearDiscovery(const char *staleNodeId);

  void publishState(bool force);

  // Returns true when the topic belonged to this device's command tree.
  bool handleCommand(const char *topic, const uint8_t *payload, unsigned int length);

  // Publishes state when the heat pump reports a change.
  void loop();

private:
  PubSubClient *mqtt;
  ISensorProvider *sensorProvider;

  char nodeId[HA_NODE_ID_LEN];
  char baseTopic[HA_TOPIC_LEN];
  char availabilityTopic[HA_TOPIC_LEN];
  char stateTopic[HA_TOPIC_LEN];
  char commandTopicFilter[HA_TOPIC_LEN];

  unsigned long tsSettingsPublished;
  unsigned long tsStatusPublished;
  unsigned long tsLastStatePublish;
  unsigned long tsLastChangeCheck;

  void buildTopics();
  void publishJson(const char *topic, JsonDocument &doc, bool retain);
  void publishDiscovery();
  void publishClimateDiscovery();
  void publishSensorDiscovery(const char *objectId, const char *name,
                              const char *valueTemplate, const char *deviceClass,
                              const char *unit, const char *stateClass,
                              bool diagnostic);
  void addDeviceBlock(JsonDocument &doc, bool full);
  void discoveryTopic(char *out, size_t outLen, const char *component,
                      const char *node, const char *objectId);

  bool applyACChange(const char *field, const char *value);
};

#endif // HOME_ASSISTANT
