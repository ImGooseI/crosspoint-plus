#pragma once

#if defined(CROSSPOINT_ENABLE_READING_STATS)

#include <cstddef>
#include <cstdint>
#include <string>

namespace readingStatsSync {

struct RemoteSummary {
  uint64_t seconds = 0;
  uint64_t sessions = 0;
  uint64_t pages = 0;
  uint32_t completedBooks = 0;
  uint32_t readingDays = 0;
  uint32_t currentStreak = 0;
  uint32_t longestStreak = 0;
  uint64_t todaySeconds = 0;
  uint64_t weekSeconds = 0;
};

// Persist a measured reader session so a later user-initiated CrossPoint Sync
// can upload it even when Wi-Fi was unavailable when the reader closed.
void recordSession(const std::string& bookPath, int64_t startedAt, int64_t endedAt, uint32_t seconds);

// Uploads pending measured sessions to the CrossPoint Sync session endpoint.
// Unsupported KOSync servers and unacknowledged IDs are left untouched.
bool sync();
bool readRemoteSummary(RemoteSummary& summary);
bool readRemoteDayPages(int year, uint32_t* pages, size_t count);
bool readRemoteDayStats(int year, uint32_t* seconds, uint16_t* sessions, size_t count);

}  // namespace readingStatsSync

#endif
