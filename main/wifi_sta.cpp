#include "wifi_sta.hpp"

#include <cstring>

#include <esp_event.h>
#include <esp_log.h>
#include <esp_netif.h>
#include <esp_wifi.h>
#include <freertos/FreeRTOS.h>
#include <freertos/event_groups.h>

namespace alc {

namespace {
constexpr const char* M_TAG { "wifi" };
constexpr uint8_t M_MAX_RETRIES { 10 };
}  // namespace

WifiSta& WifiSta::Instance()
{
  static WifiSta instance;
  return instance;
}

int WifiSta::Init()
{
  esp_err_t err { esp_netif_init() };
  if (err != ESP_OK) {
    ESP_LOGE(M_TAG, "esp_netif_init failed: %d!", err);
    return err;
  }

  err = esp_event_loop_create_default();
  if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
    ESP_LOGE(M_TAG, "event_loop_create_default failed: %d!", err);
    return err;
  }

  esp_netif_create_default_wifi_sta();

  wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
  err = esp_wifi_init(&cfg);
  if (err != ESP_OK) {
    ESP_LOGE(M_TAG, "esp_wifi_init failed: %d!", err);
    return err;
  }

  err = esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                            &eventHandler, this, nullptr);
  if (err != ESP_OK) {
    ESP_LOGE(M_TAG, "wifi event register failed: %d!", err);
    return err;
  }

  err = esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                            &eventHandler, this, nullptr);
  if (err != ESP_OK) {
    ESP_LOGE(M_TAG, "ip event register failed: %d!", err);
    return err;
  }

  return ESP_OK;
}

int WifiSta::Connect(const char* ssid, const char* password)
{
  wifi_config_t cfg {};
  std::strncpy(reinterpret_cast<char*>(cfg.sta.ssid), ssid, sizeof(cfg.sta.ssid) - 1);
  std::strncpy(reinterpret_cast<char*>(cfg.sta.password), password, sizeof(cfg.sta.password) - 1);
  cfg.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
  cfg.sta.pmf_cfg.capable = true;
  cfg.sta.pmf_cfg.required = false;

  esp_err_t err { esp_wifi_set_mode(WIFI_MODE_STA) };
  if (err != ESP_OK) { return err; }

  err = esp_wifi_set_config(WIFI_IF_STA, &cfg);
  if (err != ESP_OK) { return err; }

  err = esp_wifi_start();
  if (err != ESP_OK) {
    ESP_LOGE(M_TAG, "esp_wifi_start failed: %d!", err);
    return err;
  }

  ESP_LOGI(M_TAG, "Connecting to SSID '%s'", ssid);
  return ESP_OK;
}

void WifiSta::eventHandler(void* arg, const char* base, int32_t id, void* data)
{
  WifiSta* self { static_cast<WifiSta*>(arg) };

  if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
    esp_wifi_connect();
    return;
  }

  if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
    self->m_connected = false;
    if (self->m_cb) { self->m_cb(false); }
    if (self->m_retryCount < M_MAX_RETRIES) {
      ++self->m_retryCount;
      ESP_LOGW(M_TAG, "Disconnected, retry %u/%u", self->m_retryCount, M_MAX_RETRIES);
      esp_wifi_connect();
    } else {
      ESP_LOGE(M_TAG, "Max retries reached — giving up!");
    }
    return;
  }

  if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
    auto* event { static_cast<ip_event_got_ip_t*>(data) };
    ESP_LOGI(M_TAG, "Got IP: " IPSTR, IP2STR(&event->ip_info.ip));
    self->m_retryCount = 0;
    self->m_connected = true;
    if (self->m_cb) { self->m_cb(true); }
    return;
  }
}

}  // namespace alc
