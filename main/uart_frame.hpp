#pragma once

// ESP32-side port of the Nordic alc_uart_protocol framing library.
// Wire-compatible with ~/nordic/ncs/v3.2.4/alc_hub/alc_uart_protocol/:
//   [SOF:0xAC][TYPE:u8][LENGTH:u16-LE][PAYLOAD:0..1024][CRC16:u16-LE]
// CRC is CRC-16/CCITT-FALSE (poly 0x1021, init 0xFFFF) over TYPE+LEN+PAYLOAD.

#include <cstddef>
#include <cstdint>

namespace alc::uart {

constexpr uint8_t M_SOF { 0xAC };
constexpr uint16_t M_MAX_PAYLOAD { 1024 };
constexpr uint8_t M_FRAME_OVERHEAD { 6 };  // SOF + TYPE + LEN(2) + CRC(2).

// Message type IDs — see docs/esp_ble_coprocessor_brief.md §6.
// Requests (host -> coproc): 0x20..0x3F.
constexpr uint8_t M_REQ_COPROC_PING { 0x20 };
constexpr uint8_t M_REQ_SET_FAL_TABLE { 0x21 };
constexpr uint8_t M_REQ_GET_COPROC_INFO { 0x22 };

// Responses (coproc -> host): request | 0x40.
constexpr uint8_t M_RESP_COPROC_PING { 0x60 };
constexpr uint8_t M_RESP_SET_FAL_TABLE { 0x61 };
constexpr uint8_t M_RESP_GET_COPROC_INFO { 0x62 };
constexpr uint8_t M_RESP_UNKNOWN { 0x7F };

// Notifications (coproc -> host, unsolicited): 0xA0..0xBF.
constexpr uint8_t M_NOTIF_COPROC_READY { 0xA0 };
constexpr uint8_t M_NOTIF_PERIPHERAL_DATA { 0xA1 };
constexpr uint8_t M_NOTIF_PERIPHERAL_ERROR { 0xA2 };

uint16_t Crc16(const uint8_t* data, std::size_t len);
uint16_t Crc16Update(uint16_t crc, uint8_t byte);

// Encode a frame into `out`. Returns total bytes written (including overhead),
// or negative on error (-1 = buffer too small, -2 = payload too large).
int EncodeFrame(uint8_t type, const uint8_t* payload, std::uint16_t payloadLen,
                uint8_t* out, std::size_t outSize);

// Stateful byte-pump decoder. Feed bytes as they arrive; when a complete,
// CRC-valid frame is assembled, OnFrame(type, payload, len) is invoked and
// the parser resets to wait for the next SOF. Bad CRC / oversize / stray
// bytes trigger a silent resync on the next SOF.
class Decoder
{
public:
  using FrameCallback = void (*)(uint8_t type,
                                 const uint8_t* payload,
                                 std::uint16_t payloadLen,
                                 void* userCtx);

  void SetCallback(FrameCallback cb, void* userCtx)
  {
    m_cb = cb;
    m_userCtx = userCtx;
  }

  void Feed(const uint8_t* data, std::size_t len);
  void Reset();

  uint32_t GetCrcErrorCount() const { return m_crcErrors; }
  uint32_t GetOversizeCount() const { return m_oversize; }

private:
  enum class State : uint8_t {
    WaitSof,
    ReadType,
    ReadLenLo,
    ReadLenHi,
    ReadPayload,
    ReadCrcLo,
    ReadCrcHi,
  };

  void processByte(uint8_t b);

  State m_state { State::WaitSof };
  uint8_t m_type { 0 };
  uint16_t m_expectedLen { 0 };
  uint16_t m_payloadIdx { 0 };
  uint16_t m_crcCalc { 0xFFFF };
  uint16_t m_crcRx { 0 };
  uint8_t m_payload[M_MAX_PAYLOAD] {};

  FrameCallback m_cb { nullptr };
  void* m_userCtx { nullptr };

  uint32_t m_crcErrors { 0 };
  uint32_t m_oversize { 0 };
};

}  // namespace alc::uart
