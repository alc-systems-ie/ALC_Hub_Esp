#pragma once

#include <cstddef>
#include <sys/time.h>

namespace alc {

// SNTP-based UTC time.
// Call Init() once at boot, Start() after WiFi has an IP.
class TimeSync
{
public:
  static TimeSync& Instance();

  void Init();
  void Start();

  bool IsSynced() const { return m_synced; }

  // Fills buf with ISO 8601 UTC like "2026-04-16T19:23:45Z", or "no_sync".
  // Returns number of chars written (excluding NUL), or -1 on error.
  int GetIsoTimestamp(char* buf, std::size_t len) const;

private:
  TimeSync() = default;

  static void syncCallback(struct timeval* tv);

  bool m_synced { false };
};

}  // namespace alc
