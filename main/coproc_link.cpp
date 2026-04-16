#include "coproc_link.hpp"

#include <cstring>

#include <driver/uart.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#include "peripheral_handler.hpp"
#include "secrets_manager.hpp"

namespace alc {

namespace {
constexpr const char* M_TAG { "coproc" };
constexpr uart_port_t M_UART_NUM { UART_NUM_1 };
constexpr int M_UART_BAUD { CONFIG_ALC_COPROC_UART_BAUD };
constexpr int M_PIN_TX { CONFIG_ALC_COPROC_UART_TX_PIN };
constexpr int M_PIN_RX { CONFIG_ALC_COPROC_UART_RX_PIN };
constexpr int M_PIN_RTS { CONFIG_ALC_COPROC_UART_RTS_PIN };
constexpr int M_PIN_CTS { CONFIG_ALC_COPROC_UART_CTS_PIN };
constexpr int M_UART_RX_BUF { 2048 };
constexpr int M_UART_TX_BUF { 2048 };
constexpr int M_RX_TASK_STACK { 4096 };
constexpr UBaseType_t M_RX_TASK_PRIO { 10 };

// FAL entry on the wire (docs/esp_ble_coprocessor_brief.md §7):
// 14 serial + 6 addr + 32 secret = 52 bytes.
constexpr std::size_t M_FAL_ENTRY_SIZE { 52 };
constexpr std::size_t M_FAL_HEADER_SIZE { 2 };  // version + count.
}  // namespace

CoprocLink& CoprocLink::Instance()
{
  static CoprocLink instance;
  return instance;
}

int CoprocLink::Init()
{
  m_txMutex = xSemaphoreCreateMutex();
  if (m_txMutex == nullptr) {
    ESP_LOGE(M_TAG, "TX mutex create failed!");
    return -1;
  }

  uart_config_t cfg {};
  cfg.baud_rate = M_UART_BAUD;
  cfg.data_bits = UART_DATA_8_BITS;
  cfg.parity = UART_PARITY_DISABLE;
  cfg.stop_bits = UART_STOP_BITS_1;
  cfg.flow_ctrl = UART_HW_FLOWCTRL_CTS_RTS;
  cfg.rx_flow_ctrl_thresh = 122;
  cfg.source_clk = UART_SCLK_DEFAULT;

  esp_err_t err { uart_driver_install(M_UART_NUM, M_UART_RX_BUF, M_UART_TX_BUF,
                                      0, nullptr, 0) };
  if (err != ESP_OK) {
    ESP_LOGE(M_TAG, "uart_driver_install failed: %d!", err);
    return err;
  }

  err = uart_param_config(M_UART_NUM, &cfg);
  if (err != ESP_OK) {
    ESP_LOGE(M_TAG, "uart_param_config failed: %d!", err);
    return err;
  }

  err = uart_set_pin(M_UART_NUM, M_PIN_TX, M_PIN_RX, M_PIN_RTS, M_PIN_CTS);
  if (err != ESP_OK) {
    ESP_LOGE(M_TAG, "uart_set_pin failed: %d!", err);
    return err;
  }

  m_decoder.SetCallback(&frameCallback, this);

  BaseType_t ok { xTaskCreate(&rxTaskEntry, "coproc_rx", M_RX_TASK_STACK,
                              this, M_RX_TASK_PRIO, nullptr) };
  if (ok != pdPASS) {
    ESP_LOGE(M_TAG, "xTaskCreate failed!");
    return -1;
  }

  ESP_LOGI(M_TAG, "UART1 %d baud, pins TX=%d RX=%d RTS=%d CTS=%d",
           M_UART_BAUD, M_PIN_TX, M_PIN_RX, M_PIN_RTS, M_PIN_CTS);
  return 0;
}

void CoprocLink::rxTaskEntry(void* arg)
{
  auto* self { static_cast<CoprocLink*>(arg) };
  uint8_t buf[256];
  while (true) {
    int n { uart_read_bytes(M_UART_NUM, buf, sizeof(buf), pdMS_TO_TICKS(100)) };
    if (n > 0) {
      self->m_decoder.Feed(buf, static_cast<std::size_t>(n));
    }
  }
}

int CoprocLink::send(uint8_t type, const uint8_t* payload, uint16_t payloadLen)
{
  // Stack frame for common case; heap fallback only if we ever exceed it.
  uint8_t buf[uart::M_MAX_PAYLOAD + uart::M_FRAME_OVERHEAD];
  int n { uart::EncodeFrame(type, payload, payloadLen, buf, sizeof(buf)) };
  if (n < 0) {
    ESP_LOGE(M_TAG, "EncodeFrame failed: %d!", n);
    return n;
  }

  xSemaphoreTake(static_cast<SemaphoreHandle_t>(m_txMutex), portMAX_DELAY);
  int written { uart_write_bytes(M_UART_NUM, buf, static_cast<std::size_t>(n)) };
  xSemaphoreGive(static_cast<SemaphoreHandle_t>(m_txMutex));

  if (written != n) {
    ESP_LOGE(M_TAG, "uart_write_bytes short: %d/%d!", written, n);
    return -1;
  }
  return 0;
}

int CoprocLink::SendPing()
{
  ESP_LOGI(M_TAG, "tx CoprocPing");
  return send(uart::M_REQ_COPROC_PING, nullptr, 0);
}

int CoprocLink::SendGetCoprocInfo()
{
  return send(uart::M_REQ_GET_COPROC_INFO, nullptr, 0);
}

int CoprocLink::SendFalTable()
{
  auto& secrets { SecretsManager::Instance() };
  std::size_t count { secrets.GetDeviceCount() };

  uint8_t payload[M_FAL_HEADER_SIZE + SecretsManager::M_MAX_DEVICES * M_FAL_ENTRY_SIZE];
  payload[0] = 0x01;                                  // FAL payload version.
  payload[1] = static_cast<uint8_t>(count);

  uint8_t* cursor { &payload[M_FAL_HEADER_SIZE] };
  for (std::size_t i = 0; i < count; ++i) {
    const auto* dev { secrets.GetDevice(i) };
    const auto* addr { secrets.GetAddress(i) };
    if (dev == nullptr || addr == nullptr) { continue; }

    std::memset(cursor, 0, 14);
    std::strncpy(reinterpret_cast<char*>(cursor), dev->serial, 13);
    cursor += 14;

    std::memcpy(cursor, addr->addr, 6);
    cursor += 6;

    std::memcpy(cursor, dev->secret, 32);
    cursor += 32;
  }

  uint16_t payloadLen { static_cast<uint16_t>(M_FAL_HEADER_SIZE + count * M_FAL_ENTRY_SIZE) };
  ESP_LOGI(M_TAG, "tx SetFalTable (%u devices, %u bytes)",
           static_cast<unsigned>(count), payloadLen);
  return send(uart::M_REQ_SET_FAL_TABLE, payload, payloadLen);
}

void CoprocLink::frameCallback(uint8_t type, const uint8_t* payload,
                               uint16_t payloadLen, void* userCtx)
{
  auto* self { static_cast<CoprocLink*>(userCtx) };
  switch (type) {
    case uart::M_NOTIF_COPROC_READY:
      self->onCoprocReady(payload, payloadLen);
      break;
    case uart::M_NOTIF_PERIPHERAL_DATA:
      self->onPeripheralData(payload, payloadLen);
      break;
    case uart::M_NOTIF_PERIPHERAL_ERROR:
      self->onPeripheralError(payload, payloadLen);
      break;
    case uart::M_RESP_SET_FAL_TABLE:
      self->onSetFalTableResponse(payload, payloadLen);
      break;
    case uart::M_RESP_COPROC_PING:
      ESP_LOGI(M_TAG, "rx Pong (status=%d)",
               payloadLen >= 1 ? static_cast<int8_t>(payload[0]) : -99);
      break;
    default:
      ESP_LOGW(M_TAG, "rx unknown type 0x%02X (len=%u)", type, payloadLen);
      break;
  }
}

void CoprocLink::onCoprocReady(const uint8_t* payload, uint16_t payloadLen)
{
  if (payloadLen < 5) {
    ESP_LOGE(M_TAG, "CoprocReady short (%u bytes)!", payloadLen);
    return;
  }
  m_coprocProtoVersion = payload[0];
  uint8_t fwMajor { payload[1] };
  uint8_t fwMinor { payload[2] };
  uint8_t fwPatch { payload[3] };
  uint8_t resetCause { payload[4] };

  m_coprocReady = true;
  ESP_LOGI(M_TAG, "rx CoprocReady proto=%u fw=%u.%u.%u reset=0x%02X",
           m_coprocProtoVersion, fwMajor, fwMinor, fwPatch, resetCause);

  if (m_readyCb != nullptr) { m_readyCb(); }
}

void CoprocLink::onSetFalTableResponse(const uint8_t* payload, uint16_t payloadLen)
{
  if (payloadLen < 2) {
    ESP_LOGE(M_TAG, "SetFalTableResponse short!");
    return;
  }
  int8_t status { static_cast<int8_t>(payload[0]) };
  uint8_t accepted { payload[1] };
  ESP_LOGI(M_TAG, "rx SetFalTableResponse status=%d accepted=%u", status, accepted);
}

void CoprocLink::onPeripheralData(const uint8_t* payload, uint16_t payloadLen)
{
  // Layout per brief §8: 14+1+1+3+4+4+1+1+2 = 31 bytes header + data.
  constexpr std::size_t M_HEADER_SIZE { 31 };
  if (payloadLen < M_HEADER_SIZE) {
    ESP_LOGE(M_TAG, "PeripheralData short (%u bytes)!", payloadLen);
    return;
  }

  PeripheralRecord rec {};
  std::memcpy(rec.serial, &payload[0], 14);
  rec.serial[13] = '\0';
  rec.deviceType = payload[14];
  rec.batteryPct = payload[15];
  std::memcpy(rec.fwVersion, &payload[16], 3);
  rec.tries = static_cast<uint32_t>(payload[19])
            | (static_cast<uint32_t>(payload[20]) << 8)
            | (static_cast<uint32_t>(payload[21]) << 16)
            | (static_cast<uint32_t>(payload[22]) << 24);
  rec.updateCycleSecs = static_cast<uint32_t>(payload[23])
                     | (static_cast<uint32_t>(payload[24]) << 8)
                     | (static_cast<uint32_t>(payload[25]) << 16)
                     | (static_cast<uint32_t>(payload[26]) << 24);
  rec.capabilities = payload[27];
  rec.rssi = static_cast<int8_t>(payload[28]);
  rec.dataLen = static_cast<uint16_t>(payload[29])
              | (static_cast<uint16_t>(payload[30]) << 8);

  if (M_HEADER_SIZE + rec.dataLen != payloadLen) {
    ESP_LOGE(M_TAG, "PeripheralData length mismatch: header=%u+%u != frame=%u!",
             static_cast<unsigned>(M_HEADER_SIZE), rec.dataLen, payloadLen);
    return;
  }
  rec.data = &payload[M_HEADER_SIZE];

  ESP_LOGI(M_TAG, "rx PeripheralData %s type=%u rssi=%d batt=%u datalen=%u",
           rec.serial, rec.deviceType, rec.rssi, rec.batteryPct, rec.dataLen);

  PeripheralHandler::Instance().Handle(rec);
}

void CoprocLink::onPeripheralError(const uint8_t* payload, uint16_t payloadLen)
{
  if (payloadLen < 16) {
    ESP_LOGE(M_TAG, "PeripheralError short!");
    return;
  }
  char serial[14] {};
  std::memcpy(serial, payload, 14);
  serial[13] = '\0';
  int8_t code { static_cast<int8_t>(payload[14]) };
  uint8_t phase { payload[15] };
  ESP_LOGW(M_TAG, "rx PeripheralError %s phase=%u code=%d", serial, phase, code);
}

}  // namespace alc
