#include "uart_frame.hpp"

#include <cstring>

namespace alc::uart {

uint16_t Crc16Update(uint16_t crc, uint8_t byte)
{
  crc ^= static_cast<uint16_t>(byte) << 8;
  for (int j = 0; j < 8; ++j) {
    crc = (crc & 0x8000) ? static_cast<uint16_t>((crc << 1) ^ 0x1021)
                         : static_cast<uint16_t>(crc << 1);
  }
  return crc;
}

uint16_t Crc16(const uint8_t* data, std::size_t len)
{
  // CRC-16/CCITT-FALSE — poly 0x1021, init 0xFFFF, no refin/refout/xorout.
  uint16_t crc { 0xFFFF };
  for (std::size_t i = 0; i < len; ++i) {
    crc = Crc16Update(crc, data[i]);
  }
  return crc;
}

int EncodeFrame(uint8_t type, const uint8_t* payload, std::uint16_t payloadLen,
                uint8_t* out, std::size_t outSize)
{
  if (payloadLen > M_MAX_PAYLOAD) { return -2; }
  std::size_t total { static_cast<std::size_t>(M_FRAME_OVERHEAD) + payloadLen };
  if (total > outSize) { return -1; }

  out[0] = M_SOF;
  out[1] = type;
  out[2] = static_cast<uint8_t>(payloadLen & 0xFF);
  out[3] = static_cast<uint8_t>((payloadLen >> 8) & 0xFF);
  if (payloadLen > 0) {
    std::memcpy(&out[4], payload, payloadLen);
  }

  uint16_t crc { Crc16(&out[1], static_cast<std::size_t>(3) + payloadLen) };
  out[4 + payloadLen] = static_cast<uint8_t>(crc & 0xFF);
  out[5 + payloadLen] = static_cast<uint8_t>((crc >> 8) & 0xFF);
  return static_cast<int>(total);
}

void Decoder::Reset()
{
  m_state = State::WaitSof;
  m_type = 0;
  m_expectedLen = 0;
  m_payloadIdx = 0;
  m_crcCalc = 0xFFFF;
  m_crcRx = 0;
}

void Decoder::Feed(const uint8_t* data, std::size_t len)
{
  for (std::size_t i = 0; i < len; ++i) {
    processByte(data[i]);
  }
}

void Decoder::processByte(uint8_t b)
{
  switch (m_state) {
    case State::WaitSof:
      if (b == M_SOF) {
        m_crcCalc = 0xFFFF;
        m_state = State::ReadType;
      }
      break;

    case State::ReadType:
      m_type = b;
      m_crcCalc = Crc16Update(m_crcCalc, b);
      m_state = State::ReadLenLo;
      break;

    case State::ReadLenLo:
      m_expectedLen = b;
      m_crcCalc = Crc16Update(m_crcCalc, b);
      m_state = State::ReadLenHi;
      break;

    case State::ReadLenHi:
      m_expectedLen |= static_cast<uint16_t>(b) << 8;
      m_crcCalc = Crc16Update(m_crcCalc, b);
      if (m_expectedLen > M_MAX_PAYLOAD) {
        ++m_oversize;
        Reset();
        break;
      }
      m_payloadIdx = 0;
      m_state = (m_expectedLen == 0) ? State::ReadCrcLo : State::ReadPayload;
      break;

    case State::ReadPayload:
      m_payload[m_payloadIdx++] = b;
      m_crcCalc = Crc16Update(m_crcCalc, b);
      if (m_payloadIdx >= m_expectedLen) {
        m_state = State::ReadCrcLo;
      }
      break;

    case State::ReadCrcLo:
      m_crcRx = b;
      m_state = State::ReadCrcHi;
      break;

    case State::ReadCrcHi:
      m_crcRx |= static_cast<uint16_t>(b) << 8;
      if (m_crcCalc == m_crcRx) {
        if (m_cb != nullptr) {
          m_cb(m_type, m_payload, m_expectedLen, m_userCtx);
        }
      } else {
        ++m_crcErrors;
      }
      Reset();
      break;
  }
}

}  // namespace alc::uart
