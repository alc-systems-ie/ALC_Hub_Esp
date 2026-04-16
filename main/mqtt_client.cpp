#include "mqtt_client.hpp"

#include <cstdio>
#include <cstring>

#include <esp_log.h>
#include <mqtt_client.h>

#include "certificate.h"
#include "time_sync.hpp"

namespace alc {

namespace {
constexpr const char* M_TAG { "mqtt" };
constexpr int M_TOPIC_LEN { 96 };

char s_statusTopic[M_TOPIC_LEN];
constexpr const char* M_OFFLINE_PAYLOAD { "{\"status\":\"offline\"}" };
}  // namespace

MqttClient& MqttClient::Instance()
{
  static MqttClient instance;
  return instance;
}

int MqttClient::Init(const char* brokerUri, const char* serial, const char* secret)
{
  m_serial = serial;

  std::snprintf(s_statusTopic, sizeof(s_statusTopic), "alc/%s/status", serial);

  esp_mqtt_client_config_t cfg {};
  cfg.broker.address.uri = brokerUri;
  cfg.broker.verification.certificate = ISRG_ROOT_X1_PEM;
  cfg.credentials.client_id = serial;
  cfg.credentials.username = serial;
  cfg.credentials.authentication.password = secret;

  // Last Will — broker publishes offline if we drop.
  cfg.session.last_will.topic = s_statusTopic;
  cfg.session.last_will.msg = M_OFFLINE_PAYLOAD;
  cfg.session.last_will.msg_len = std::strlen(M_OFFLINE_PAYLOAD);
  cfg.session.last_will.qos = 1;
  cfg.session.last_will.retain = 1;

  cfg.session.keepalive = 60;
  cfg.network.reconnect_timeout_ms = 10000;

  auto* handle { esp_mqtt_client_init(&cfg) };
  if (handle == nullptr) {
    ESP_LOGE(M_TAG, "esp_mqtt_client_init failed!");
    return -1;
  }
  m_client = handle;

  esp_err_t err { esp_mqtt_client_register_event(handle,
                                                 static_cast<esp_mqtt_event_id_t>(ESP_EVENT_ANY_ID),
                                                 &eventHandler, this) };
  if (err != ESP_OK) {
    ESP_LOGE(M_TAG, "register_event failed: %d!", err);
    return err;
  }

  ESP_LOGI(M_TAG, "Initialised: broker=%s client=%s", brokerUri, serial);
  return ESP_OK;
}

int MqttClient::Start()
{
  if (m_client == nullptr) { return -1; }
  return esp_mqtt_client_start(static_cast<esp_mqtt_client_handle_t>(m_client));
}

int MqttClient::Publish(const char* topicSuffix, const char* payload, int qos, bool retain)
{
  if (!m_connected || m_client == nullptr) { return -1; }

  char topic[M_TOPIC_LEN];
  std::snprintf(topic, sizeof(topic), "alc/%s/%s", m_serial, topicSuffix);

  int msgId { esp_mqtt_client_publish(static_cast<esp_mqtt_client_handle_t>(m_client),
                                      topic, payload, 0, qos, retain ? 1 : 0) };
  if (msgId < 0) {
    ESP_LOGE(M_TAG, "Publish failed on '%s'!", topic);
  }
  return msgId;
}

int MqttClient::Subscribe(const char* topicSuffix, int qos)
{
  if (!m_connected || m_client == nullptr) { return -1; }

  char topic[M_TOPIC_LEN];
  std::snprintf(topic, sizeof(topic), "alc/%s/%s", m_serial, topicSuffix);

  int msgId { esp_mqtt_client_subscribe(static_cast<esp_mqtt_client_handle_t>(m_client),
                                        topic, qos) };
  if (msgId < 0) {
    ESP_LOGE(M_TAG, "Subscribe failed on '%s'!", topic);
  } else {
    ESP_LOGI(M_TAG, "Subscribed to %s (qos=%d)", topic, qos);
  }
  return msgId;
}

void MqttClient::eventHandler(void* args, const char* base, int32_t id, void* data)
{
  auto* self { static_cast<MqttClient*>(args) };
  auto* event { static_cast<esp_mqtt_event_handle_t>(data) };

  switch (static_cast<esp_mqtt_event_id_t>(id)) {
    case MQTT_EVENT_CONNECTED: {
      ESP_LOGI(M_TAG, "CONNECTED");
      self->m_connected = true;

      // Publish retained online status with timestamp (or "no_sync" if not yet synced).
      char ts[32];
      TimeSync::Instance().GetIsoTimestamp(ts, sizeof(ts));
      char payload[96];
      std::snprintf(payload, sizeof(payload), "{\"status\":\"online\",\"ts\":\"%s\"}", ts);
      esp_mqtt_client_publish(event->client, s_statusTopic, payload, 0, 1, 1);
      ESP_LOGI(M_TAG, "Published %s -> %s", s_statusTopic, payload);

      if (self->m_cb) { self->m_cb(true); }
      break;
    }

    case MQTT_EVENT_DISCONNECTED:
      ESP_LOGW(M_TAG, "DISCONNECTED");
      self->m_connected = false;
      if (self->m_cb) { self->m_cb(false); }
      break;

    case MQTT_EVENT_DATA:
      // Single-message dispatch only — payloads larger than the rx buffer arrive
      // chunked (current_data_offset != 0 or data_len != total_data_len) and are
      // ignored here. Secrets blobs for <16 devices fit comfortably in the default
      // MQTT buffer, so this simplification is safe.
      if (event->current_data_offset == 0 && event->data_len == event->total_data_len) {
        if (self->m_dataCb != nullptr) {
          self->m_dataCb(event->topic, event->topic_len,
                         event->data, event->data_len);
        }
      } else {
        ESP_LOGW(M_TAG, "Chunked payload ignored (offset=%d len=%d total=%d)",
                 event->current_data_offset, event->data_len, event->total_data_len);
      }
      break;

    case MQTT_EVENT_ERROR:
      if (event->error_handle != nullptr) {
        ESP_LOGE(M_TAG, "ERROR type=%d tls=0x%x sock=%d!",
                 event->error_handle->error_type,
                 event->error_handle->esp_tls_last_esp_err,
                 event->error_handle->esp_transport_sock_errno);
      }
      break;

    default:
      break;
  }
}

}  // namespace alc
