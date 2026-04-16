#include "secrets_manager.hpp"

#include <cctype>
#include <cstdio>
#include <cstring>

#include <cJSON.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <mbedtls/sha256.h>
#include <nvs.h>
#include <nvs_flash.h>

#include "mqtt_client.hpp"

namespace alc {

namespace {
constexpr const char* M_TAG { "secrets" };
constexpr std::size_t M_EXPECTED_SECRET_HEX_LEN { 64 };
constexpr std::size_t M_SERIAL_MAX_LEN { sizeof(BleDeviceSecret::serial) - 1 };
}  // namespace

SecretsManager& SecretsManager::Instance()
{
  static SecretsManager instance;
  return instance;
}

int SecretsManager::Init()
{
  int result { loadFromNvs() };
  if (result == 0) {
    m_ready = (m_deviceCount > 0);
    ESP_LOGI(M_TAG, "Loaded from NVS: version=%u count=%u ready=%d",
             m_version, static_cast<unsigned>(m_deviceCount), m_ready);
    deriveAllAddresses();
  } else {
    ESP_LOGI(M_TAG, "No cached secrets — will request on connect");
  }
  return 0;
}

void SecretsManager::OnMqttConnected()
{
  auto& mqtt { MqttClient::Instance() };
  mqtt.Subscribe("secrets/ver", 1);
  mqtt.Subscribe("secrets/res", 1);
  sendRequest();
}

void SecretsManager::OnMqttMessage(const char* topic, std::size_t topicLen,
                                   const char* data, std::size_t dataLen)
{
  // Match on the trailing component of the topic so we don't need the full
  // alc/<serial>/ prefix built up here.
  auto endsWith = [&](const char* suffix) {
    std::size_t n { std::strlen(suffix) };
    return topicLen >= n && std::strncmp(topic + topicLen - n, suffix, n) == 0;
  };

  if (endsWith("/secrets/ver")) {
    handleVer(data, dataLen);
  } else if (endsWith("/secrets/res")) {
    handleRes(data, dataLen);
  }
}

bool SecretsManager::handleVer(const char* data, std::size_t dataLen)
{
  cJSON* root { cJSON_ParseWithLength(data, dataLen) };
  if (root == nullptr) {
    ESP_LOGE(M_TAG, "ver: JSON parse failed!");
    return false;
  }

  cJSON* verItem { cJSON_GetObjectItem(root, "version") };
  if (!cJSON_IsNumber(verItem)) {
    ESP_LOGE(M_TAG, "ver: missing 'version' field!");
    cJSON_Delete(root);
    return false;
  }

  uint8_t serverVersion { static_cast<uint8_t>(verItem->valueint) };
  cJSON_Delete(root);

  ESP_LOGI(M_TAG, "ver: local=%u server=%u", m_version, serverVersion);

  if (serverVersion != m_version) {
    sendRequest();
  }
  return true;
}

bool SecretsManager::handleRes(const char* data, std::size_t dataLen)
{
  cJSON* root { cJSON_ParseWithLength(data, dataLen) };
  if (root == nullptr) {
    ESP_LOGE(M_TAG, "res: JSON parse failed!");
    return false;
  }

  cJSON* verItem { cJSON_GetObjectItem(root, "version") };
  cJSON* devItem { cJSON_GetObjectItem(root, "devices") };
  if (!cJSON_IsNumber(verItem) || !cJSON_IsArray(devItem)) {
    ESP_LOGE(M_TAG, "res: missing 'version' or 'devices'!");
    cJSON_Delete(root);
    return false;
  }

  uint8_t newVersion { static_cast<uint8_t>(verItem->valueint) };
  std::size_t count { 0 };
  BleDeviceSecret parsed[M_MAX_DEVICES] {};

  int arraySize { cJSON_GetArraySize(devItem) };
  for (int i = 0; i < arraySize && count < M_MAX_DEVICES; ++i) {
    cJSON* entry { cJSON_GetArrayItem(devItem, i) };
    cJSON* serial { cJSON_GetObjectItem(entry, "serial") };
    cJSON* secret { cJSON_GetObjectItem(entry, "secret") };
    if (!cJSON_IsString(serial) || !cJSON_IsString(secret)) {
      ESP_LOGW(M_TAG, "res: device[%d] malformed — skipped", i);
      continue;
    }

    std::size_t serialLen { std::strlen(serial->valuestring) };
    std::size_t secretLen { std::strlen(secret->valuestring) };
    if (serialLen > M_SERIAL_MAX_LEN || secretLen != M_EXPECTED_SECRET_HEX_LEN) {
      ESP_LOGW(M_TAG, "res: device[%d] '%s' wrong lengths (serial=%u secret=%u)!",
               i, serial->valuestring,
               static_cast<unsigned>(serialLen), static_cast<unsigned>(secretLen));
      continue;
    }

    std::strncpy(parsed[count].serial, serial->valuestring, M_SERIAL_MAX_LEN);
    parsed[count].serial[M_SERIAL_MAX_LEN] = '\0';

    if (!hexDecode(secret->valuestring, secretLen, parsed[count].secret,
                   sizeof(parsed[count].secret))) {
      ESP_LOGW(M_TAG, "res: device[%d] '%s' bad hex!", i, serial->valuestring);
      continue;
    }

    ++count;
  }

  cJSON_Delete(root);

  // Full snapshot replaces the local table (per brief — not a delta).
  std::memcpy(m_devices, parsed, sizeof(m_devices));
  m_deviceCount = count;
  m_version = newVersion;
  m_ready = true;

  ESP_LOGI(M_TAG, "res: applied version=%u count=%u", m_version,
           static_cast<unsigned>(m_deviceCount));

  deriveAllAddresses();

  if (m_tableChangedCb != nullptr) { m_tableChangedCb(); }

  int err { saveToNvs() };
  if (err != 0) {
    ESP_LOGE(M_TAG, "NVS save failed: %d!", err);
  }
  return true;
}

int SecretsManager::sendRequest()
{
  int64_t nowMs { esp_timer_get_time() / 1000 };
  if (m_lastRequestMs != 0 && (nowMs - m_lastRequestMs) < M_MIN_REQ_INTERVAL_MS) {
    ESP_LOGD(M_TAG, "req rate-limited (%lld ms since last)",
             static_cast<long long>(nowMs - m_lastRequestMs));
    return 0;
  }
  m_lastRequestMs = nowMs;

  char payload[32];
  std::snprintf(payload, sizeof(payload), "{\"version\":%u}", m_version);

  ESP_LOGI(M_TAG, "req -> %s", payload);
  return MqttClient::Instance().Publish("secrets/req", payload, 1, false);
}

int SecretsManager::loadFromNvs()
{
  nvs_handle_t handle {};
  esp_err_t err { nvs_open(M_NVS_NAMESPACE, NVS_READWRITE, &handle) };
  if (err != ESP_OK) { return err; }

  uint8_t version { 0 };
  err = nvs_get_u8(handle, M_NVS_KEY_VERSION, &version);
  if (err != ESP_OK) {
    nvs_close(handle);
    return err;
  }

  uint8_t count { 0 };
  err = nvs_get_u8(handle, M_NVS_KEY_COUNT, &count);
  if (err != ESP_OK || count > M_MAX_DEVICES) {
    nvs_close(handle);
    return err == ESP_OK ? ESP_ERR_INVALID_SIZE : err;
  }

  std::size_t blobLen { sizeof(BleDeviceSecret) * count };
  if (blobLen > 0) {
    err = nvs_get_blob(handle, M_NVS_KEY_DEVICES, m_devices, &blobLen);
    if (err != ESP_OK) {
      nvs_close(handle);
      return err;
    }
  }

  m_version = version;
  m_deviceCount = count;
  nvs_close(handle);
  return 0;
}

int SecretsManager::saveToNvs()
{
  nvs_handle_t handle {};
  esp_err_t err { nvs_open(M_NVS_NAMESPACE, NVS_READWRITE, &handle) };
  if (err != ESP_OK) { return err; }

  err = nvs_set_u8(handle, M_NVS_KEY_VERSION, m_version);
  if (err != ESP_OK) { nvs_close(handle); return err; }

  err = nvs_set_u8(handle, M_NVS_KEY_COUNT, static_cast<uint8_t>(m_deviceCount));
  if (err != ESP_OK) { nvs_close(handle); return err; }

  if (m_deviceCount > 0) {
    err = nvs_set_blob(handle, M_NVS_KEY_DEVICES, m_devices,
                       sizeof(BleDeviceSecret) * m_deviceCount);
    if (err != ESP_OK) { nvs_close(handle); return err; }
  } else {
    // Empty device list — explicitly clear any stale blob so stale secrets
    // from a previous group assignment can't linger.
    nvs_erase_key(handle, M_NVS_KEY_DEVICES);
  }

  err = nvs_commit(handle);
  nvs_close(handle);
  return err;
}

int SecretsManager::DeriveAddress(const uint8_t* secret, BleAddress* out)
{
  if (secret == nullptr || out == nullptr) { return -1; }

  uint8_t hash[32];
  int err { mbedtls_sha256(secret, 32, hash, 0) };  // 0 = SHA-256, not SHA-224.
  if (err != 0) { return err; }

  // Little-endian layout: hash[0] is LSB (printed last), hash[5] is MSB.
  for (int i = 0; i < 6; ++i) {
    out->addr[i] = hash[i];
  }
  // Static-random marker: top two bits of the MSB must be 0b11.
  out->addr[5] = (out->addr[5] & 0x3F) | 0xC0;
  return 0;
}

void SecretsManager::deriveAllAddresses()
{
  for (std::size_t i = 0; i < m_deviceCount; ++i) {
    if (DeriveAddress(m_devices[i].secret, &m_addresses[i]) != 0) {
      ESP_LOGE(M_TAG, "deriveAddress failed for %s!", m_devices[i].serial);
      // Zero the address so downstream code can't accidentally use stale bytes.
      std::memset(&m_addresses[i], 0, sizeof(m_addresses[i]));
      continue;
    }
    const uint8_t* a { m_addresses[i].addr };
    ESP_LOGI(M_TAG, "  %s -> %02X:%02X:%02X:%02X:%02X:%02X (static-random)",
             m_devices[i].serial, a[5], a[4], a[3], a[2], a[1], a[0]);
  }
}

bool SecretsManager::hexDecode(const char* hex, std::size_t hexLen, uint8_t* out, std::size_t outLen)
{
  if (hexLen != outLen * 2) { return false; }

  auto nibble = [](char c) -> int {
    if (c >= '0' && c <= '9') { return c - '0'; }
    if (c >= 'a' && c <= 'f') { return c - 'a' + 10; }
    if (c >= 'A' && c <= 'F') { return c - 'A' + 10; }
    return -1;
  };

  for (std::size_t i = 0; i < outLen; ++i) {
    int hi { nibble(hex[2 * i]) };
    int lo { nibble(hex[2 * i + 1]) };
    if (hi < 0 || lo < 0) { return false; }
    out[i] = static_cast<uint8_t>((hi << 4) | lo);
  }
  return true;
}

}  // namespace alc
