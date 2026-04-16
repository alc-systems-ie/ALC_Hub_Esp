#pragma once

// UART link to the nRF54L15-DK BLE coprocessor.
// Protocol: see docs/esp_ble_coprocessor_brief.md (in the Nordic alc_hub tree).

#include <cstddef>
#include <cstdint>

#include "uart_frame.hpp"

namespace alc {

// Parsed PeripheralData payload (docs/esp_ble_coprocessor_brief.md §8).
struct PeripheralRecord
{
  char serial[14];
  uint8_t deviceType;
  uint8_t batteryPct;
  uint8_t fwVersion[3];
  uint32_t tries;
  uint32_t updateCycleSecs;
  uint8_t capabilities;
  int8_t rssi;
  const uint8_t* data;   // Points into the decoder's frame buffer; do not retain.
  uint16_t dataLen;
};

class CoprocLink
{
public:
  static CoprocLink& Instance();

  int Init();

  // Host -> coproc.
  int SendPing();
  int SendFalTable();          // Builds from SecretsManager::Instance().
  int SendGetCoprocInfo();

  // Coproc readiness — true after CoprocReady notification received.
  bool IsReady() const { return m_coprocReady; }
  uint8_t GetProtocolVersion() const { return m_coprocProtoVersion; }

  using ReadyCallback = void (*)();
  void SetReadyCallback(ReadyCallback cb) { m_readyCb = cb; }

private:
  CoprocLink() = default;

  static void rxTaskEntry(void* arg);
  static void frameCallback(uint8_t type, const uint8_t* payload,
                            uint16_t payloadLen, void* userCtx);

  int send(uint8_t type, const uint8_t* payload, uint16_t payloadLen);

  void onCoprocReady(const uint8_t* payload, uint16_t payloadLen);
  void onPeripheralData(const uint8_t* payload, uint16_t payloadLen);
  void onPeripheralError(const uint8_t* payload, uint16_t payloadLen);
  void onSetFalTableResponse(const uint8_t* payload, uint16_t payloadLen);

  uart::Decoder m_decoder {};

  bool m_coprocReady { false };
  uint8_t m_coprocProtoVersion { 0 };
  ReadyCallback m_readyCb { nullptr };

  void* m_txMutex { nullptr };  // SemaphoreHandle_t
};

}  // namespace alc
