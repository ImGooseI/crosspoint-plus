#include "ReadingStatsJournal.h"

#if defined(CROSSPOINT_ENABLE_READING_STATS)

#include <HalClock.h>
#include <HalStorage.h>
#include <Logging.h>
#include <TrustedTime.h>
#include <Utf8.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <ctime>

namespace {
constexpr char JOURNAL_PATH[] = "/.crosspoint/reading_stats.dat";
constexpr char GOAL_PATH[] = "/.crosspoint/reading_goal.dat";
constexpr char GOAL_TEMP_PATH[] = "/.crosspoint/reading_goal.tmp";
constexpr char GOAL_BACKUP_PATH[] = "/.crosspoint/reading_goal.bak";
constexpr uint16_t JOURNAL_VERSION = 1;
constexpr uint32_t GOAL_MAGIC = 0x47535043;  // "CPSG" in little-endian storage.
constexpr uint32_t MIN_TRUSTED_YEAR = 2025;
constexpr uint32_t MAX_TRUSTED_YEAR = 2100;
constexpr int READING_DAY_RANGE = 28000;
constexpr size_t MAX_TRACKED_BOOKS = 256;
constexpr size_t MAX_PENDING_RECORDS = 8;

#pragma pack(push, 1)
struct JournalHeader {
  char magic[4];
  uint16_t version;
  uint16_t recordSize;
};

struct JournalRecord {
  uint32_t dayKey;
  uint32_t bookHash;
  uint32_t seconds;
  uint16_t pages;
  uint16_t progressBasisPoints;
  uint8_t sessions;
  uint8_t flags;
  char title[48];
};

struct GoalFile {
  uint32_t magic;
  uint16_t version;
  uint16_t minutes;
};
#pragma pack(pop)

static_assert(sizeof(JournalRecord) == 66, "reading stats journal record layout changed");

constexpr uint8_t COMPLETE_FLAG = 1;
constexpr JournalHeader HEADER{{'C', 'P', 'S', 'T'}, JOURNAL_VERSION, sizeof(JournalRecord)};
JournalRecord pendingRecords[MAX_PENDING_RECORDS]{};
size_t pendingCount = 0;
uint32_t dirtySinceMs = 0;
uint32_t lastFlushMs = 0;
uint16_t cachedGoalMinutes = 30;
bool goalLoaded = false;

uint32_t hashPath(const char* path) {
  uint32_t hash = 2166136261u;
  if (!path) return hash;
  while (*path) {
    hash ^= static_cast<uint8_t>(*path++);
    hash *= 16777619u;
  }
  return hash == 0 ? 1 : hash;
}

uint32_t makeDayKey(const std::tm& local) {
  const int year = local.tm_year + 1900;
  if (year < static_cast<int>(MIN_TRUSTED_YEAR) || year > static_cast<int>(MAX_TRUSTED_YEAR)) return 0;
  return static_cast<uint32_t>(year * 10000 + (local.tm_mon + 1) * 100 + local.tm_mday);
}

bool localDate(const int64_t epoch, std::tm& local) {
  if (epoch <= 0) return false;
  const time_t value = static_cast<time_t>(epoch);
  if (!localtime_r(&value, &local)) return false;
  return makeDayKey(local) != 0;
}

int64_t civilDays(const int year, const unsigned month, const unsigned day) {
  int adjustedYear = year;
  adjustedYear -= month <= 2;
  const int era = (adjustedYear >= 0 ? adjustedYear : adjustedYear - 399) / 400;
  const unsigned yearOfEra = static_cast<unsigned>(adjustedYear - era * 400);
  const unsigned adjustedMonth = month > 2 ? month - 3 : month + 9;
  const unsigned dayOfYear = (153 * adjustedMonth + 2) / 5 + day - 1;
  const unsigned dayOfEra = yearOfEra * 365 + yearOfEra / 4 - yearOfEra / 100 + dayOfYear;
  return static_cast<int64_t>(era) * 146097 + static_cast<int64_t>(dayOfEra) - 719468;
}

bool splitDayKey(const uint32_t dayKey, int& year, int& month, int& day) {
  year = static_cast<int>(dayKey / 10000);
  month = static_cast<int>((dayKey / 100) % 100);
  day = static_cast<int>(dayKey % 100);
  return year >= 1970 && month >= 1 && month <= 12 && day >= 1 && day <= 31;
}

int dayOfYear(const int year, const int month, const int day) {
  static constexpr uint16_t STARTS[] = {0,   0,  31,  59,  90, 120, 151,
                                        181, 212, 243, 273, 304, 334};
  const bool leap = year % 4 == 0 && (year % 100 != 0 || year % 400 == 0);
  return STARTS[month] + day - 1 + ((leap && month > 2) ? 1 : 0);
}

int daysInMonth(const int year, const int month) {
  static constexpr int MONTH_DAYS[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  if (month < 1 || month > 12) return 30;
  const bool leap = year % 4 == 0 && (year % 100 != 0 || year % 400 == 0);
  return month == 2 && leap ? 29 : MONTH_DAYS[month - 1];
}

bool ensureJournalHeader(HalFile& file) {
  if (file.fileSize() == 0) return file.write(&HEADER, sizeof(HEADER)) == sizeof(HEADER);
  if (file.fileSize() < sizeof(JournalHeader)) return false;
  JournalHeader found{};
  if (!file.seek(0) || file.read(&found, sizeof(found)) != sizeof(found)) return false;
  return std::memcmp(found.magic, HEADER.magic, sizeof(found.magic)) == 0 && found.version == JOURNAL_VERSION &&
         found.recordSize == sizeof(JournalRecord);
}

bool recordAlreadyComplete(const uint32_t bookHash) {
  for (size_t i = 0; i < pendingCount; ++i) {
    if (pendingRecords[i].bookHash == bookHash && (pendingRecords[i].flags & COMPLETE_FLAG)) return true;
  }
  HalFile file;
  if (!Storage.openFileForRead("STAT", JOURNAL_PATH, file)) return false;
  if (file.fileSize() < sizeof(JournalHeader)) return false;
  JournalHeader header{};
  if (file.read(&header, sizeof(header)) != sizeof(header) ||
      std::memcmp(header.magic, HEADER.magic, sizeof(header.magic)) != 0 || header.version != JOURNAL_VERSION ||
      header.recordSize != sizeof(JournalRecord)) {
    return false;
  }
  const uint64_t count = (file.fileSize() - sizeof(JournalHeader)) / sizeof(JournalRecord);
  JournalRecord record{};
  for (uint64_t i = 0; i < count; ++i) {
    if (file.read(&record, sizeof(record)) != sizeof(record)) break;
    if (record.bookHash == bookHash && (record.flags & COMPLETE_FLAG)) return true;
  }
  return false;
}

bool appendPendingRecords() {
  if (pendingCount == 0) return true;
  Storage.mkdir("/.crosspoint");
  HalFile file = Storage.open(JOURNAL_PATH, O_RDWR | O_CREAT | O_AT_END);
  if (!file) {
    LOG_ERR("STAT", "Could not open reading journal for append");
    return false;
  }
  const uint64_t originalSize = file.fileSize();
  if (!ensureJournalHeader(file)) {
    LOG_ERR("STAT", "Reading journal header is invalid; preserving existing data");
    return false;
  }
  const uint64_t completeSize = sizeof(JournalHeader) +
                                ((file.fileSize() - sizeof(JournalHeader)) / sizeof(JournalRecord)) *
                                    sizeof(JournalRecord);
  if (completeSize != file.fileSize() && !file.truncate(completeSize)) {
    LOG_ERR("STAT", "Could not remove incomplete final reading record");
    return false;
  }
  if (!file.seek(file.fileSize())) return false;
  const size_t bytes = pendingCount * sizeof(JournalRecord);
  const size_t written = file.write(pendingRecords, bytes);
  if (written != bytes) {
    if (!file.truncate(originalSize)) LOG_ERR("STAT", "Could not roll back a partial journal append");
    LOG_ERR("STAT", "Short reading journal append");
    return false;
  }
  file.flush();
  pendingCount = 0;
  dirtySinceMs = 0;
  lastFlushMs = millis();
  return true;
}

void addToDay(ReadingStatsDay& day, const JournalRecord& record) {
  day.seconds += record.seconds;
  day.pages += record.pages;
  day.sessions = static_cast<uint16_t>(std::min<uint32_t>(UINT16_MAX, day.sessions + record.sessions));
}

ReadingStatsBook* findOrCreateBook(ReadingStatsBook* books, size_t& count, const JournalRecord& record) {
  for (size_t i = 0; i < count; ++i) {
    if (books[i].hash == record.bookHash) return &books[i];
  }
  if (count == MAX_TRACKED_BOOKS) return nullptr;
  ReadingStatsBook& book = books[count++];
  book.hash = record.bookHash;
  memcpy(book.title, record.title, sizeof(book.title));
  return &book;
}

int64_t todaySerial() {
  const int64_t now = trustedtime::trustedNow();
  std::tm local{};
  if (!localDate(now, local)) return INT64_MIN;
  return civilDays(local.tm_year + 1900, static_cast<unsigned>(local.tm_mon + 1),
                   static_cast<unsigned>(local.tm_mday));
}

void loadGoalOnce() {
  if (goalLoaded) return;
  goalLoaded = true;
  const auto readGoal = [](const char* path, GoalFile& saved) {
    HalFile file;
    return Storage.openFileForRead("STAT", path, file) && file.read(&saved, sizeof(saved)) == sizeof(saved) &&
           saved.magic == GOAL_MAGIC && saved.version == 1 && saved.minutes >= 5 && saved.minutes <= 300 &&
           saved.minutes % 5 == 0;
  };
  GoalFile saved{};
  if (readGoal(GOAL_PATH, saved) || readGoal(GOAL_BACKUP_PATH, saved)) {
    cachedGoalMinutes = saved.minutes;
    return;
  }
  if (Storage.exists(GOAL_PATH) || Storage.exists(GOAL_BACKUP_PATH)) {
    LOG_ERR("STAT", "Reading goal data is invalid; using the default");
  }
}

}  // namespace

void ReadingStatsCapture::setBook(const char* path, const char* bookTitle) {
  bookHash = hashPath(path);
  memset(title, 0, sizeof(title));
  if (!bookTitle) return;
  const size_t copySize = std::min(strlen(bookTitle), sizeof(title) - 1);
  const size_t safeSize = static_cast<size_t>(utf8SafeTruncateBuffer(bookTitle, static_cast<int>(copySize)));
  memcpy(title, bookTitle, safeSize);
}

bool ReadingStatsCapture::push(const ReadingStatsEvent& event) {
  const uint8_t current = head.load(std::memory_order_relaxed);
  const uint8_t next = static_cast<uint8_t>((current + 1) % QUEUE_SIZE);
  if (next == tail.load(std::memory_order_acquire)) {
    const uint8_t previous = dropped.load(std::memory_order_relaxed);
    if (previous < UINT8_MAX) dropped.store(static_cast<uint8_t>(previous + 1), std::memory_order_relaxed);
    return false;
  }
  queue[current] = event;
  head.store(next, std::memory_order_release);
  return true;
}

void ReadingStatsCapture::noteTurn(const bool forward, const bool succeeded) {
  pendingTurn = succeeded;
  pendingForward = forward && succeeded;
}

void ReadingStatsCapture::onRenderComplete(const uint32_t monotonicMs, const int64_t trustedEpoch,
                                           const int progressBp) {
  if (pendingTurn && hasAnchor && anchorEpoch > 0 && bookHash != 0) {
    const uint32_t elapsed = (monotonicMs - anchorMs) / 1000;
    if (elapsed >= 2) {
      uint32_t remaining = std::min<uint32_t>(elapsed, 1800);
      time_t cursor = static_cast<time_t>(anchorEpoch);
      bool pageCounted = false;
      while (remaining > 0) {
        std::tm local{};
        if (!localtime_r(&cursor, &local)) break;
        const uint32_t dayKey = makeDayKey(local);
        if (dayKey == 0) break;
        std::tm nextDay = local;
        nextDay.tm_hour = 0;
        nextDay.tm_min = 0;
        nextDay.tm_sec = 0;
        nextDay.tm_mday++;
        nextDay.tm_isdst = -1;
        time_t midnight = mktime(&nextDay);
        if (midnight <= cursor) midnight = cursor + 86400;
        const uint32_t untilMidnight = static_cast<uint32_t>(midnight - cursor);
        const uint32_t contribution = std::min(remaining, untilMidnight);

        ReadingStatsEvent event{};
        event.dayKey = dayKey;
        event.bookHash = bookHash;
        event.seconds = contribution;
        event.pages = pageCounted || !pendingForward ? 0 : 1;
        event.sessions = dayKey == sessionDayKey ? 0 : 1;
        event.progressBasisPoints = static_cast<uint16_t>(std::clamp(progressBp, 0, 10000));
        memcpy(event.title, title, sizeof(event.title));
        push(event);
        sessionDayKey = dayKey;
        pageCounted = true;
        remaining -= contribution;
        cursor += contribution;
      }
    }
  }
  anchorMs = monotonicMs;
  anchorEpoch = trustedEpoch;
  progressBasisPoints = static_cast<uint16_t>(std::clamp(progressBp, 0, 10000));
  hasAnchor = true;
  pendingForward = false;
  pendingTurn = false;
}

void ReadingStatsCapture::finish(const uint32_t monotonicMs, const int64_t trustedEpoch) {
  pendingTurn = true;
  pendingForward = false;
  onRenderComplete(monotonicMs, trustedEpoch, progressBasisPoints);
  hasAnchor = false;
}

void ReadingStatsCapture::queueCompletion(const uint32_t dayKey, const int progressBp) {
  if (dayKey == 0 || bookHash == 0) return;
  ReadingStatsEvent event{};
  event.dayKey = dayKey;
  event.bookHash = bookHash;
  event.progressBasisPoints = static_cast<uint16_t>(std::clamp(progressBp, 0, 10000));
  event.completed = true;
  memcpy(event.title, title, sizeof(event.title));
  push(event);
}

bool ReadingStatsCapture::pop(ReadingStatsEvent& event) {
  const uint8_t current = tail.load(std::memory_order_relaxed);
  if (current == head.load(std::memory_order_acquire)) return false;
  event = queue[current];
  tail.store(static_cast<uint8_t>((current + 1) % QUEUE_SIZE), std::memory_order_release);
  return true;
}

uint8_t ReadingStatsCapture::takeDroppedEvents() { return dropped.exchange(0, std::memory_order_relaxed); }

void ReadingStatsCapture::reset() {
  anchorMs = 0;
  anchorEpoch = 0;
  sessionDayKey = 0;
  progressBasisPoints = 0;
  hasAnchor = false;
  pendingForward = false;
  pendingTurn = false;
}

uint16_t ReadingStatsJournal::goalMinutes() {
  loadGoalOnce();
  return cachedGoalMinutes;
}

uint32_t ReadingStatsJournal::bookHashForPath(const char* path) { return hashPath(path); }

bool ReadingStatsJournal::setGoalMinutes(const uint16_t minutes) {
  if (minutes < 5 || minutes > 300 || minutes % 5 != 0) return false;
  Storage.mkdir("/.crosspoint");
  const GoalFile saved{GOAL_MAGIC, 1, minutes};
  HalFile file = Storage.open(GOAL_TEMP_PATH, O_RDWR | O_CREAT | O_TRUNC);
  if (!file || file.write(&saved, sizeof(saved)) != sizeof(saved)) {
    LOG_ERR("STAT", "Could not save reading goal");
    return false;
  }
  file.flush();
  if (!file.close()) {
    Storage.remove(GOAL_TEMP_PATH);
    LOG_ERR("STAT", "Could not close reading goal file");
    return false;
  }
  if (Storage.exists(GOAL_BACKUP_PATH) && !Storage.remove(GOAL_BACKUP_PATH)) return false;
  const bool hadGoal = Storage.exists(GOAL_PATH);
  if (hadGoal && !Storage.rename(GOAL_PATH, GOAL_BACKUP_PATH)) {
    Storage.remove(GOAL_TEMP_PATH);
    return false;
  }
  if (!Storage.rename(GOAL_TEMP_PATH, GOAL_PATH)) {
    if (hadGoal) Storage.rename(GOAL_BACKUP_PATH, GOAL_PATH);
    Storage.remove(GOAL_TEMP_PATH);
    LOG_ERR("STAT", "Could not install new reading goal");
    return false;
  }
  if (hadGoal) Storage.remove(GOAL_BACKUP_PATH);
  cachedGoalMinutes = minutes;
  goalLoaded = true;
  return true;
}

uint32_t ReadingStatsJournal::currentDayKey() {
  std::tm local{};
  return localDate(trustedtime::trustedNow(), local) ? makeDayKey(local) : 0;
}

int ReadingStatsJournal::currentYear() { return static_cast<int>(currentDayKey() / 10000); }

void ReadingStatsJournal::accumulate(const ReadingStatsEvent& event) {
  if (event.dayKey == 0 || event.bookHash == 0) return;
  if (event.completed && recordAlreadyComplete(event.bookHash)) return;

  JournalRecord* target = nullptr;
  for (size_t i = 0; i < pendingCount; ++i) {
    if (pendingRecords[i].dayKey == event.dayKey && pendingRecords[i].bookHash == event.bookHash) {
      target = &pendingRecords[i];
      break;
    }
  }
  if (!target && pendingCount == MAX_PENDING_RECORDS) {
    if (!appendPendingRecords()) return;
  }
  if (!target) {
    target = &pendingRecords[pendingCount++];
    *target = {};
    target->dayKey = event.dayKey;
    target->bookHash = event.bookHash;
    memcpy(target->title, event.title, sizeof(target->title));
  }
  target->seconds += event.seconds;
  target->pages = static_cast<uint16_t>(std::min<uint32_t>(UINT16_MAX, target->pages + event.pages));
  target->sessions = static_cast<uint8_t>(std::min<uint32_t>(UINT8_MAX, target->sessions + event.sessions));
  target->progressBasisPoints = std::max(target->progressBasisPoints, event.progressBasisPoints);
  if (event.completed) target->flags |= COMPLETE_FLAG;
  if (dirtySinceMs == 0) dirtySinceMs = millis();
}

bool ReadingStatsJournal::flush() { return appendPendingRecords(); }

bool ReadingStatsJournal::flushDue(const uint32_t monotonicMs) {
  if (pendingCount == 0 || dirtySinceMs == 0 || monotonicMs - dirtySinceMs < 300000) return true;
  return appendPendingRecords();
}

bool ReadingStatsJournal::readSnapshot(const int year, const uint32_t selectedDayKey,
                                       ReadingStatsSnapshot& snapshot) {
  static uint8_t readingDayBits[(READING_DAY_RANGE + 7) / 8];
  static ReadingStatsBook trackedBooks[MAX_TRACKED_BOOKS];
  memset(readingDayBits, 0, sizeof(readingDayBits));
  memset(trackedBooks, 0, sizeof(trackedBooks));
  size_t trackedBookCount = 0;
  bool bookTrackingOverflow = false;
  snapshot = {};
  snapshot.year = year;
  snapshot.goalMinutes = goalMinutes();
  if (year < 1970 || year > 2100) return false;
  int selectedYear, selectedMonth, selectedDay;
  const bool hasSelectedDate = splitDayKey(selectedDayKey, selectedYear, selectedMonth, selectedDay);
  const auto calculateMonthTotals = [&] {
    if (!hasSelectedDate || selectedYear != year) return;
    for (int day = 1; day <= daysInMonth(year, selectedMonth); ++day) {
      const ReadingStatsDay& stats = snapshot.days[dayOfYear(year, selectedMonth, day)];
      snapshot.monthSeconds += stats.seconds;
      if (stats.seconds > 0) ++snapshot.monthDaysRead;
    }
  };

  const uint32_t chartDayKey = currentDayKey() != 0 ? currentDayKey() : selectedDayKey;
  const int64_t chartSerial = [&] {
    int y, m, d;
    return splitDayKey(chartDayKey, y, m, d) ? civilDays(y, static_cast<unsigned>(m), static_cast<unsigned>(d))
                                              : INT64_MIN;
  }();
  int mondayOffset = 0;
  if (chartSerial != INT64_MIN) {
    const time_t noonUtc = static_cast<time_t>(chartSerial * 86400 + 43200);
    std::tm utc{};
    gmtime_r(&noonUtc, &utc);
    mondayOffset = (utc.tm_wday + 6) % 7;
  }
  for (int i = 0; i < 7; ++i) {
    if (chartSerial != INT64_MIN) {
      // Convert the neighboring serial day back through UTC noon, avoiding DST gaps.
      const int64_t epoch = (chartSerial - mondayOffset + i) * 86400 + 43200;
      const time_t stamp = static_cast<time_t>(epoch);
      std::tm utc{};
      gmtime_r(&stamp, &utc);
      snapshot.weekDayKeys[i] = static_cast<uint32_t>((utc.tm_year + 1900) * 10000 + (utc.tm_mon + 1) * 100 + utc.tm_mday);
    }
  }

  HalFile file;
  if (!Storage.openFileForRead("STAT", JOURNAL_PATH, file)) {
    calculateMonthTotals();
    return true;
  }
  if (file.fileSize() < sizeof(JournalHeader)) return false;
  JournalHeader header{};
  if (file.read(&header, sizeof(header)) != sizeof(header) ||
      std::memcmp(header.magic, HEADER.magic, sizeof(header.magic)) != 0 || header.version != JOURNAL_VERSION ||
      header.recordSize != sizeof(JournalRecord)) {
    LOG_ERR("STAT", "Reading journal header is invalid");
    return false;
  }

  const uint64_t recordCount = (file.fileSize() - sizeof(JournalHeader)) / sizeof(JournalRecord);
  if ((file.fileSize() - sizeof(JournalHeader)) % sizeof(JournalRecord) != 0) {
    LOG_ERR("STAT", "Ignoring a partial final reading journal record");
  }

  const int64_t firstTrackedDay = civilDays(2025, 1, 1);
  JournalRecord record{};
  for (uint64_t i = 0; i < recordCount; ++i) {
    if (file.read(&record, sizeof(record)) != sizeof(record)) break;
    int recordYear, month, day;
    if (!splitDayKey(record.dayKey, recordYear, month, day)) continue;
    const int64_t serial = civilDays(recordYear, static_cast<unsigned>(month), static_cast<unsigned>(day));
    snapshot.lifetimeSeconds += record.seconds;
    snapshot.lifetimePages += record.pages;
    snapshot.lifetimeSessions += record.sessions;
    if (ReadingStatsBook* book = findOrCreateBook(trackedBooks, trackedBookCount, record)) {
      book->seconds += record.seconds;
      book->pages += record.pages;
      book->sessions = static_cast<uint16_t>(std::min<uint32_t>(UINT16_MAX, book->sessions + record.sessions));
      book->progressBasisPoints = std::max(book->progressBasisPoints, record.progressBasisPoints);
      book->completed = book->completed || (record.flags & COMPLETE_FLAG);
      if (record.seconds > 0) book->lastReadDayKey = std::max(book->lastReadDayKey, record.dayKey);
      if (record.flags & COMPLETE_FLAG) book->completionDayKey = std::max(book->completionDayKey, record.dayKey);
      if (record.title[0]) memcpy(book->title, record.title, sizeof(book->title));
    } else {
      bookTrackingOverflow = true;
    }

    if (record.seconds > 0) {
      const int64_t bitIndex = serial - firstTrackedDay;
      if (bitIndex >= 0 && bitIndex < READING_DAY_RANGE) {
        readingDayBits[bitIndex / 8] |= static_cast<uint8_t>(1u << (bitIndex % 8));
      }
      if (record.dayKey >= snapshot.latestDayKey) {
        snapshot.latestDayKey = record.dayKey;
        snapshot.latestBookHash = record.bookHash;
        snapshot.latestBookProgressBasisPoints = record.progressBasisPoints;
        memcpy(snapshot.latestBook, record.title, sizeof(snapshot.latestBook));
      }
    }

    if (recordYear == year) {
      const int index = dayOfYear(recordYear, month, day);
      if (index >= 0 && index < ReadingStatsSnapshot::DAYS_IN_YEAR) addToDay(snapshot.days[index], record);
    }
    for (int weekDay = 0; weekDay < 7; ++weekDay) {
      if (snapshot.weekDayKeys[weekDay] == record.dayKey) addToDay(snapshot.week[weekDay], record);
    }
  }

  calculateMonthTotals();

  for (size_t bookIndex = 0; bookIndex < trackedBookCount; ++bookIndex) {
    const ReadingStatsBook& book = trackedBooks[bookIndex];
    if (book.completed) ++snapshot.completedBooks;
    if (book.hash == snapshot.latestBookHash) {
      snapshot.latestBookSeconds = book.seconds;
      snapshot.latestBookCompleted = book.completed;
    }
    if (book.completed && book.completionDayKey >= snapshot.lastCompletedDayKey) {
      snapshot.lastCompletedDayKey = book.completionDayKey;
      snapshot.lastCompletedBookHash = book.hash;
      snapshot.lastCompletedBookSeconds = book.seconds;
      memcpy(snapshot.lastCompletedBook, book.title, sizeof(snapshot.lastCompletedBook));
    }
    if (!book.completed && book.lastReadDayKey > snapshot.currentBookDayKey) {
      snapshot.currentBookDayKey = book.lastReadDayKey;
      snapshot.currentBookHash = book.hash;
      snapshot.currentBookProgressBasisPoints = book.progressBasisPoints;
      snapshot.currentBookSeconds = book.seconds;
      memcpy(snapshot.currentBook, book.title, sizeof(snapshot.currentBook));
    }
    if (book.seconds == 0) continue;
    size_t position = 0;
    while (position < ReadingStatsSnapshot::TOP_BOOKS &&
           snapshot.topBooks[position].seconds >= book.seconds) {
      ++position;
    }
    if (position == ReadingStatsSnapshot::TOP_BOOKS) continue;
    for (size_t move = ReadingStatsSnapshot::TOP_BOOKS - 1; move > position; --move) {
      snapshot.topBooks[move] = snapshot.topBooks[move - 1];
    }
    snapshot.topBooks[position] = book;
  }
  if (bookTrackingOverflow) {
    LOG_ERR("STAT", "Book statistics tracking limit reached (%u)", static_cast<unsigned>(MAX_TRACKED_BOOKS));
  }

  const int64_t today = todaySerial();
  uint32_t streak = 0;
  int64_t lastReadingSerial = INT64_MIN;
  for (int index = 0; index < READING_DAY_RANGE; ++index) {
    if ((readingDayBits[index / 8] & (1u << (index % 8))) == 0) {
      streak = 0;
      continue;
    }
    ++snapshot.lifetimeReadingDays;
    ++streak;
    lastReadingSerial = firstTrackedDay + index;
    snapshot.longestStreak = std::max(snapshot.longestStreak, streak);
  }
  snapshot.currentStreak =
      (today != INT64_MIN && lastReadingSerial >= today - 1 && lastReadingSerial <= today) ? streak : 0;
  return true;
}

#endif  // CROSSPOINT_ENABLE_READING_STATS
