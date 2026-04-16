#include "time_sync.hpp"

#include <cstdio>
#include <ctime>
#include <sys/time.h>

#include <esp_log.h>
#include <esp_sntp.h>

namespace alc {

namespace {
constexpr const char* M_TAG { "sntp" };
constexpr const char* M_PRIMARY_SERVER { "pool.ntp.org" };
constexpr const char* M_SECONDARY_SERVER { "time.google.com" };
constexpr uint32_t M_SYNC_INTERVAL_MS { 4U * 60U * 60U * 1000U };  // 4 hours.
}  // namespace

TimeSync& TimeSync::Instance()
{
  static TimeSync instance;
  return instance;
}

void TimeSync::Init()
{
  // Force UTC — no local timezone adjustment.
  setenv("TZ", "UTC0", 1);
  tzset();

  esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
  esp_sntp_setservername(0, M_PRIMARY_SERVER);
  esp_sntp_setservername(1, M_SECONDARY_SERVER);
  sntp_set_sync_mode(SNTP_SYNC_MODE_IMMED);
  sntp_set_sync_interval(M_SYNC_INTERVAL_MS);
  sntp_set_time_sync_notification_cb(&syncCallback);
}

void TimeSync::Start()
{
  if (esp_sntp_enabled()) { return; }
  esp_sntp_init();
  ESP_LOGI(M_TAG, "Started — servers: %s, %s", M_PRIMARY_SERVER, M_SECONDARY_SERVER);
}

int TimeSync::GetIsoTimestamp(char* buf, std::size_t len) const
{
  if (buf == nullptr || len < 21) { return -1; }

  if (!m_synced) {
    return std::snprintf(buf, len, "no_sync");
  }

  std::time_t now { std::time(nullptr) };
  std::tm tm {};
  gmtime_r(&now, &tm);
  return static_cast<int>(std::strftime(buf, len, "%Y-%m-%dT%H:%M:%SZ", &tm));
}

void TimeSync::syncCallback(struct timeval* tv)
{
  auto& self { Instance() };
  self.m_synced = true;

  char iso[32];
  self.GetIsoTimestamp(iso, sizeof(iso));
  ESP_LOGI(M_TAG, "Sync: %s (epoch=%lld)", iso, static_cast<long long>(tv->tv_sec));
}

}  // namespace alc
