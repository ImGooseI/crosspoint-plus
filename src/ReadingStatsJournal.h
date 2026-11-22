#pragma once

#if defined(CROSSPOINT_ENABLE_READING_STATS)

#include <array>
#include <atomic>
#include <cstdint>

struct ReadingStatsEvent {
  uint32_t dayKey = 0;
  uint32_t bookHash = 0;
  uint32_t seconds = 0;
  uint16_t pages = 0;
  uint16_t progressBasisPoints = 0;
  uint8_t sessions = 0;
  bool completed = false;
  char title[48] = {};
};

class ReadingStatsCapture {
 public:
  void setBook(const char* path, const char* title);
  void noteTurn(bool forward, bool succeeded);
  void onRenderComplete(uint32_t monotonicMs, int64_t trustedEpoch, int progressBasisPoints);
  void finish(uint32_t monotonicMs, int64_t trustedEpoch);
  void queueCompletion(uint32_t dayKey, int progressBasisPoints);
  bool pop(ReadingStatsEvent& event);
  uint8_t takeDroppedEvents();
  void reset();

 private:
  static constexpr uint8_t QUEUE_SIZE = 8;
  std::array<ReadingStatsEvent, QUEUE_SIZE> queue{};
  std::atomic<uint8_t> head{0};
  std::atomic<uint8_t> tail{0};
  std::atomic<uint8_t> dropped{0};
  uint32_t anchorMs = 0;
  int64_t anchorEpoch = 0;
  uint32_t bookHash = 0;
  uint32_t sessionDayKey = 0;
  uint16_t progressBasisPoints = 0;
  bool hasAnchor = false;
  bool pendingForward = false;
  bool pendingTurn = false;
  char title[48] = {};

  bool push(const ReadingStatsEvent& event);
};

struct ReadingStatsDay {
  uint32_t seconds = 0;
  uint32_t pages = 0;
  uint16_t sessions = 0;
};

struct ReadingStatsBook {
  uint32_t hash = 0;
  uint32_t lastReadDayKey = 0;
  uint32_t completionDayKey = 0;
  uint64_t seconds = 0;
  uint32_t pages = 0;
  uint16_t sessions = 0;
  uint16_t progressBasisPoints = 0;
  bool completed = false;
  char title[48] = {};
};

struct ReadingStatsSnapshot {
  static constexpr int DAYS_IN_YEAR = 366;
  static constexpr int TOP_BOOKS = 5;

  int year = 0;
  uint16_t goalMinutes = 30;
  uint64_t lifetimeSeconds = 0;
  uint64_t lifetimePages = 0;
  uint64_t lifetimeSessions = 0;
  uint64_t lifetimeReadingDays = 0;
  uint32_t completedBooks = 0;
  uint32_t currentStreak = 0;
  uint32_t longestStreak = 0;
  uint32_t latestDayKey = 0;
  uint32_t latestBookHash = 0;
  uint16_t latestBookProgressBasisPoints = 0;
  uint64_t latestBookSeconds = 0;
  bool latestBookCompleted = false;
  uint32_t currentBookDayKey = 0;
  uint32_t currentBookHash = 0;
  uint16_t currentBookProgressBasisPoints = 0;
  uint64_t currentBookSeconds = 0;
  uint32_t lastCompletedDayKey = 0;
  uint32_t lastCompletedBookHash = 0;
  uint64_t lastCompletedBookSeconds = 0;
  uint32_t monthDaysRead = 0;
  uint64_t monthSeconds = 0;
  uint32_t weekDayKeys[7]{};
  ReadingStatsDay week[7]{};
  ReadingStatsDay days[DAYS_IN_YEAR]{};
  ReadingStatsBook topBooks[TOP_BOOKS]{};
  char latestBook[48] = {};
  char currentBook[48] = {};
  char lastCompletedBook[48] = {};
};

class ReadingStatsJournal {
 public:
  static uint32_t bookHashForPath(const char* path);
  static uint16_t goalMinutes();
  static bool setGoalMinutes(uint16_t minutes);
  static uint32_t currentDayKey();
  static int currentYear();
  static void accumulate(const ReadingStatsEvent& event);
  static bool flush();
  static bool flushDue(uint32_t monotonicMs);
  static bool readSnapshot(int year, uint32_t selectedDayKey, ReadingStatsSnapshot& snapshot);
};

#endif  // CROSSPOINT_ENABLE_READING_STATS
