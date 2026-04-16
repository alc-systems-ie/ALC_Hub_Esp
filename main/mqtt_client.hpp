#pragma once

#include <cstddef>
#include <cstdint>

namespace alc {

// MQTT TLS client wrapping esp-mqtt.
// Publishes {"status":"online"} on connect; LWT {"status":"offline"}.
class MqttClient
{
public:
  using ConnectedCallback = void (*)(bool connected);
  using DataCallback = void (*)(const char* topic, std::size_t topicLen,
                                const char* data, std::size_t dataLen);

  static MqttClient& Instance();

  int Init(const char* brokerUri, const char* serial, const char* secret);
  int Start();
  int Publish(const char* topicSuffix, const char* payload, int qos = 1, bool retain = false);
  int Subscribe(const char* topicSuffix, int qos = 1);

  const char* GetSerial() const { return m_serial; }
  bool IsConnected() const { return m_connected; }
  void SetCallback(ConnectedCallback cb) { m_cb = cb; }
  void SetDataCallback(DataCallback cb) { m_dataCb = cb; }

private:
  MqttClient() = default;

  static void eventHandler(void* args, const char* base, int32_t id, void* data);

  void* m_client { nullptr };  // esp_mqtt_client_handle_t
  const char* m_serial { nullptr };
  bool m_connected { false };
  ConnectedCallback m_cb { nullptr };
  DataCallback m_dataCb { nullptr };
};

}  // namespace alc
