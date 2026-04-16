#pragma once

// BLE peripheral secrets provisioning over MQTT.
// Spec: ~/nordic/ncs/v3.2.4/alc_hub/docs/hub_ble_secrets_brief.md
//
// Flow on MQTT connect:
//   1. Subscribe to alc/<hub>/secrets/ver and alc/<hub>/secrets/res (QoS 1).
//   2. Publish alc/<hub>/secrets/req with {"version": <stored>}.
//   3. On secrets/ver: if != stored, re-publish req.
//   4. On secrets/res: replace table, persist version + devices in NVS.

#include <cstddef>
#include <cstdint>

namespace alc {

struct BleDeviceSecret
{
  char serial[16];      // "XXX-NNNN-NNNN"
  uint8_t secret[32];   // 256-bit key (hex-decoded from payload).
};

class SecretsManager
{
public:
  static constexpr std::size_t M_MAX_DEVICES { 16 };

  static SecretsManager& Instance();

  int Init();

  // Called by the MQTT wiring on connect/message.
  void OnMqttConnected();
  void OnMqttMessage(const char* topic, std::size_t topicLen,
                     const char* data, std::size_t dataLen);

  bool IsReady() const { return m_ready; }
  uint8_t GetVersion() const { return m_version; }
  std::size_t GetDeviceCount() const { return m_deviceCount; }
  const BleDeviceSecret* GetDevice(std::size_t i) const
  {
    return (i < m_deviceCount) ? &m_devices[i] : nullptr;
  }

private:
  static constexpr const char* M_NVS_NAMESPACE { "alc" };
  static constexpr const char* M_NVS_KEY_VERSION { "sec_ver" };
  static constexpr const char* M_NVS_KEY_DEVICES { "sec_devs" };
  static constexpr const char* M_NVS_KEY_COUNT { "sec_cnt" };
  static constexpr uint32_t M_MIN_REQ_INTERVAL_MS { 30000 };

  SecretsManager() = default;

  int loadFromNvs();
  int saveToNvs();
  bool handleVer(const char* data, std::size_t dataLen);
  bool handleRes(const char* data, std::size_t dataLen);
  int sendRequest();
  static bool hexDecode(const char* hex, std::size_t hexLen, uint8_t* out, std::size_t outLen);

  BleDeviceSecret m_devices[M_MAX_DEVICES] {};
  std::size_t m_deviceCount { 0 };
  uint8_t m_version { 0 };
  bool m_ready { false };
  int64_t m_lastRequestMs { 0 };
};

}  // namespace alc
