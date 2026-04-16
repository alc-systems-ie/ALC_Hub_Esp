#pragma once

#include <cstdint>

namespace alc {

// WiFi station manager — connect, handle events, expose got-IP status.
class WifiSta
{
public:
  using ConnectedCallback = void (*)(bool connected);

  static WifiSta& Instance();

  int Init();
  int Connect(const char* ssid, const char* password);
  bool IsConnected() const { return m_connected; }
  void SetCallback(ConnectedCallback cb) { m_cb = cb; }

private:
  WifiSta() = default;

  static void eventHandler(void* arg, const char* base, int32_t id, void* data);

  bool m_connected { false };
  ConnectedCallback m_cb { nullptr };
  uint8_t m_retryCount { 0 };
};

}  // namespace alc
