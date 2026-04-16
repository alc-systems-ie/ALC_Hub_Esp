#include "peripheral_handler.hpp"

#include <cstdio>
#include <cstring>

#include <esp_log.h>

#include "mqtt_client.hpp"
#include "time_sync.hpp"

namespace alc {

namespace {
constexpr const char* M_TAG { "periph" };

constexpr uint8_t M_DEVICE_TYPE_FRIDGE_MASTER { 1 };
constexpr uint8_t M_DEVICE_TYPE_DRAWER_MASTER { 2 };
constexpr uint8_t M_DEVICE_TYPE_ROOM_MASTER   { 3 };
constexpr uint8_t M_DEVICE_TYPE_FLUSH_MASTER  { 4 };

// Room Master SensorPayload v1 — 9 bytes, packed little-endian.
// Mirrors room_master::SensorPayload in
// ~/nordic/ncs/v3.2.4/alc_room_master/src/ble_auth_peripheral.hpp.
struct __attribute__((packed)) RmaSensorPayloadV1
{
  uint8_t  version;          // 0x01
  uint16_t soundLevelDb;     // dB × 100
  uint16_t lightLevel;       // raw
  int16_t  tempCx100;        // °C × 100
  uint16_t humidityPctx100;  // % × 100
};
static_assert(sizeof(RmaSensorPayloadV1) == 9, "RmaSensorPayloadV1 must be 9 bytes");
}  // namespace

PeripheralHandler& PeripheralHandler::Instance()
{
  static PeripheralHandler instance;
  return instance;
}

void PeripheralHandler::Handle(const PeripheralRecord& rec)
{
  switch (rec.deviceType) {
    case M_DEVICE_TYPE_ROOM_MASTER:
      handleRoomMaster(rec);
      break;

    case M_DEVICE_TYPE_FRIDGE_MASTER:
    case M_DEVICE_TYPE_DRAWER_MASTER:
    case M_DEVICE_TYPE_FLUSH_MASTER:
      ESP_LOGW(M_TAG, "%s: deviceType=%u not implemented in v1",
               rec.serial, rec.deviceType);
      break;

    default:
      ESP_LOGW(M_TAG, "%s: unknown deviceType=%u", rec.serial, rec.deviceType);
      break;
  }
}

void PeripheralHandler::handleRoomMaster(const PeripheralRecord& rec)
{
  if (rec.dataLen != sizeof(RmaSensorPayloadV1)) {
    ESP_LOGE(M_TAG, "%s: RMA data len %u != %u!",
             rec.serial, rec.dataLen, static_cast<unsigned>(sizeof(RmaSensorPayloadV1)));
    return;
  }

  RmaSensorPayloadV1 p {};
  std::memcpy(&p, rec.data, sizeof(p));

  if (p.version != 0x01) {
    ESP_LOGW(M_TAG, "%s: RMA payload version %u (expected 1)", rec.serial, p.version);
    return;
  }

  double soundDb { p.soundLevelDb / 100.0 };
  double tempC { p.tempCx100 / 100.0 };
  double humidityPct { p.humidityPctx100 / 100.0 };
  uint16_t light { p.lightLevel };

  char ts[32];
  TimeSync::Instance().GetIsoTimestamp(ts, sizeof(ts));

  char payload[192];
  std::snprintf(payload, sizeof(payload),
                "{\"sound_db\":%.2f,\"light\":%u,\"temp_c\":%.2f,"
                "\"humidity_pct\":%.2f,\"rssi\":%d,\"timestamp\":\"%s\"}",
                soundDb, light, tempC, humidityPct, rec.rssi, ts);

  char topicSuffix[32];
  std::snprintf(topicSuffix, sizeof(topicSuffix), "%s/status", rec.serial);

  int ret { MqttClient::Instance().Publish(topicSuffix, payload, 1, false) };
  if (ret < 0) {
    ESP_LOGE(M_TAG, "RMA publish failed on alc/<hub>/%s!", topicSuffix);
  } else {
    ESP_LOGI(M_TAG, "%s -> %s", topicSuffix, payload);
  }
}

}  // namespace alc
