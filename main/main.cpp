#include <cstdio>

#include <esp_log.h>
#include <esp_wifi.h>
#include <nvs_flash.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "mqtt_client.hpp"
#include "time_sync.hpp"
#include "wifi_sta.hpp"

namespace {
constexpr const char* M_TAG { "app" };
constexpr uint32_t M_HEARTBEAT_PERIOD_MS { 60000 };
}

static void publishHeartbeat(uint32_t uptimeSec)
{
  auto& mqtt { alc::MqttClient::Instance() };
  if (!mqtt.IsConnected()) { return; }

  char ts[32];
  alc::TimeSync::Instance().GetIsoTimestamp(ts, sizeof(ts));

  int8_t rssi { 0 };
  wifi_ap_record_t ap {};
  if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
    rssi = ap.rssi;
  }

  char payload[128];
  std::snprintf(payload, sizeof(payload),
                "{\"ts\":\"%s\",\"uptime_s\":%lu,\"rssi\":%d}",
                ts, static_cast<unsigned long>(uptimeSec), rssi);

  mqtt.Publish("heartbeat", payload, 1, false);
  ESP_LOGI(M_TAG, "heartbeat -> %s", payload);
}

// C-linkage entry point required by ESP-IDF.
extern "C" void app_main()
{
  ESP_LOGI(M_TAG, "ALC Hub ESP boot — serial=%s", CONFIG_ALC_HUB_SERIAL);

  // NVS — required by WiFi driver for credential storage.
  esp_err_t err { nvs_flash_init() };
  if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
    nvs_flash_erase();
    err = nvs_flash_init();
  }
  ESP_ERROR_CHECK(err);

  auto& wifi { alc::WifiSta::Instance() };
  auto& mqtt { alc::MqttClient::Instance() };
  auto& timeSync { alc::TimeSync::Instance() };

  wifi.SetCallback([](bool connected) {
    if (connected) {
      ESP_LOGI(M_TAG, "WiFi up — starting SNTP + MQTT");
      alc::TimeSync::Instance().Start();
      alc::MqttClient::Instance().Start();
    }
  });

  // WiFi init must run first — it brings up esp_netif and the LwIP tcpip thread,
  // which SNTP's operating-mode / server APIs require.
  ESP_ERROR_CHECK(wifi.Init());
  timeSync.Init();
  ESP_ERROR_CHECK(mqtt.Init(CONFIG_ALC_MQTT_BROKER_URI,
                            CONFIG_ALC_HUB_SERIAL,
                            CONFIG_ALC_HUB_SECRET));

  ESP_ERROR_CHECK(wifi.Connect(CONFIG_ALC_WIFI_SSID, CONFIG_ALC_WIFI_PASSWORD));

  uint32_t uptimeSec { 0 };
  while (true) {
    vTaskDelay(pdMS_TO_TICKS(M_HEARTBEAT_PERIOD_MS));
    uptimeSec += M_HEARTBEAT_PERIOD_MS / 1000;
    publishHeartbeat(uptimeSec);
  }
}
