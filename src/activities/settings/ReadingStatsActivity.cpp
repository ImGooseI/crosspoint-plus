#include "ReadingStatsActivity.h"

#if defined(CROSSPOINT_ENABLE_READING_STATS)

#include <GfxRenderer.h>
#include <Bitmap.h>
#include <Epub.h>
#include <FsHelpers.h>
#include <HalStorage.h>
#include <I18n.h>
#include <LibraryBuilder.h>
#include <LibraryIndexFile.h>
#include <Logging.h>
#include <Memory.h>
#include <Xtc.h>

#include <LibraryText.h>

#include <algorithm>
#include <cstdio>
#include <ctime>

#include "MappedInputManager.h"
#include "CrossPointState.h"
#include "components/UITheme.h"
#include "components/themes/BaseTheme.h"
#include "RecentBooksStore.h"
#include "fontIds.h"
#include "activities/util/IntervalSelectionActivity.h"

namespace {
constexpr int GOAL_MINUTES_MIN = 5;
constexpr int GOAL_MINUTES_MAX = 300;
constexpr int GOAL_MINUTES_STEP = 5;
constexpr int TOP_INSET = 4;
constexpr int GRID_COLUMNS = 7;

bool isLeapYear(const int year) { return year % 4 == 0 && (year % 100 != 0 || year % 400 == 0); }

int daysInMonth(const int year, const int month) {
  static constexpr int MONTH_DAYS[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  if (month < 1 || month > 12) return 30;
  if (month == 2 && isLeapYear(year)) return 29;
  return MONTH_DAYS[month - 1];
}

int dayOfYear(const int year, const int month, const int day) {
  static constexpr int STARTS[] = {0, 0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334};
  return STARTS[month] + day - 1 + ((isLeapYear(year) && month > 2) ? 1 : 0);
}

int firstMondayOffset(const int year, const int month) {
  std::tm local{};
  local.tm_year = year - 1900;
  local.tm_mon = month - 1;
  local.tm_mday = 1;
  local.tm_hour = 12;
  local.tm_isdst = -1;
  const time_t value = mktime(&local);
  if (value == static_cast<time_t>(-1)) return 0;
  localtime_r(&value, &local);
  return (local.tm_wday + 6) % 7;
}

uint32_t makeDayKey(const int year, const int month, const int day) {
  return static_cast<uint32_t>(year * 10000 + month * 100 + day);
}

void keyToDate(const uint32_t key, int& year, int& month, int& day) {
  year = static_cast<int>(key / 10000);
  month = static_cast<int>((key / 100) % 100);
  day = static_cast<int>(key % 100);
}

uint8_t intensityFor(const uint32_t seconds, const uint16_t goalMinutes) {
  if (seconds == 0 || goalMinutes == 0) return 0;
  const uint64_t goalSeconds = static_cast<uint64_t>(goalMinutes) * 60;
  const uint64_t level = (static_cast<uint64_t>(seconds) * 4 + goalSeconds - 1) / goalSeconds;
  return static_cast<uint8_t>(std::min<uint64_t>(4, std::max<uint64_t>(1, level)));
}

void formatDuration(const uint64_t seconds, char* buffer, const size_t bufferSize) {
  const uint64_t minutes = seconds / 60;
  const uint64_t hours = minutes / 60;
  if (hours > 0) {
    snprintf(buffer, bufferSize, "%llu h %llu m", static_cast<unsigned long long>(hours),
             static_cast<unsigned long long>(minutes % 60));
  } else {
    snprintf(buffer, bufferSize, "%llu min", static_cast<unsigned long long>(minutes));
  }
}

void formatCount(const uint64_t value, char* buffer, const size_t bufferSize) {
  snprintf(buffer, bufferSize, "%llu", static_cast<unsigned long long>(value));
}

void drawTargetIcon(GfxRenderer& renderer, const int x, const int y) {
  constexpr int CENTER = 12;
  for (int py = 1; py < 24; ++py) {
    for (int px = 1; px < 24; ++px) {
      const int dx = px - CENTER;
      const int dy = py - CENTER;
      const int radiusSquared = dx * dx + dy * dy;
      if (abs(radiusSquared - 100) <= 12 || abs(radiusSquared - 49) <= 10 || radiusSquared <= 5) {
        renderer.drawPixel(x + px, y + py);
      }
    }
  }
  renderer.drawLine(x + 12, y + 12, x + 23, y + 1, true);
  renderer.drawLine(x + 17, y + 1, x + 23, y + 1, true);
  renderer.drawLine(x + 23, y + 1, x + 23, y + 7, true);
}

void formatShortDuration(const uint64_t seconds, char* buffer, const size_t bufferSize) {
  const uint64_t minutes = seconds / 60;
  const uint64_t hours = minutes / 60;
  if (hours > 0) snprintf(buffer, bufferSize, "%llu h %02llu m", static_cast<unsigned long long>(hours),
                          static_cast<unsigned long long>(minutes % 60));
  else snprintf(buffer, bufferSize, "%llu m", static_cast<unsigned long long>(minutes));
}

const char* monthName(const int month) {
  static constexpr StrId NAMES[] = {StrId::STR_JANUARY,   StrId::STR_FEBRUARY, StrId::STR_MARCH,  StrId::STR_APRIL,
                                    StrId::STR_MAY,       StrId::STR_JUNE,     StrId::STR_JULY,   StrId::STR_AUGUST,
                                    StrId::STR_SEPTEMBER, StrId::STR_OCTOBER,  StrId::STR_NOVEMBER, StrId::STR_DECEMBER};
  return month >= 1 && month <= 12 ? I18n::getInstance().get(NAMES[month - 1]) : "";
}

}  // namespace

ReadingStatsActivity::ReadingStatsActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
    : Activity("ReadingStats", renderer, mappedInput) {}

void ReadingStatsActivity::onEnter() {
  Activity::onEnter();
  const uint32_t today = ReadingStatsJournal::currentDayKey();
  if (today != 0) {
    keyToDate(today, selectedYear, selectedMonth, selectedDay);
    reloadSnapshot();
    prepareBookCovers();
    requestUpdate();
    return;
  }

  // A trusted wall clock may not be available offline. Use the latest saved
  // reading date to open a useful view without assigning today's activity to it.
  ReadingStatsJournal::readSnapshot(2100, 0, snapshot);
  if (snapshot.latestDayKey != 0) {
    keyToDate(snapshot.latestDayKey, selectedYear, selectedMonth, selectedDay);
  } else {
    std::tm local{};
    const time_t now = time(nullptr);
    if (localtime_r(&now, &local) && local.tm_year >= 125 && local.tm_year <= 200) {
      selectedYear = local.tm_year + 1900;
      selectedMonth = local.tm_mon + 1;
      selectedDay = local.tm_mday;
    } else {
      selectedYear = 2026;
      selectedMonth = 1;
      selectedDay = 1;
    }
  }
  reloadSnapshot();
  prepareBookCovers();
  requestUpdate();
}

uint32_t ReadingStatsActivity::selectedDayKey() const { return makeDayKey(selectedYear, selectedMonth, selectedDay); }

ReadingStatsDay ReadingStatsActivity::selectedDayStats() const {
  const int index = dayOfYear(selectedYear, selectedMonth, selectedDay);
  if (index < 0 || index >= ReadingStatsSnapshot::DAYS_IN_YEAR) return {};
  return snapshot.days[index];
}

void ReadingStatsActivity::reloadSnapshot() {
  if (!ReadingStatsJournal::readSnapshot(selectedYear, selectedDayKey(), snapshot)) {
    LOG_ERR("STAT", "Could not load reading statistics");
    snapshot = {};
    snapshot.year = selectedYear;
  }
}

void ReadingStatsActivity::prepareBookCovers() {
  constexpr int COVER_HEIGHT = 94;
  const auto prepare = [](const char* title, const uint32_t bookHash, std::string& author) {
    author.clear();
    const bool hasTitle = title && title[0];
    std::string bookPath;
    std::string thumbBase;
    for (const auto& book : RECENT_BOOKS.getBooks()) {
      const bool matches = bookHash != 0 ? ReadingStatsJournal::bookHashForPath(book.path.c_str()) == bookHash
                                         : (hasTitle && book.title == title);
      if (!matches) continue;
      bookPath = book.path;
      thumbBase = book.coverBmpPath;
      author = book.author;
      break;
    }

    if (bookPath.empty() && hasTitle) {
      library::LibraryIndexFile index;
      if (index.open(library::libraryIndexPath()) && !index.ranksDegraded()) {
        library::ClixRecord record;
        const std::string foldedTitle = library::fold(title);
        // Index sort keys are stored in a fixed 96-byte field. Search with its
        // longest complete UTF-8 prefix, then verify full titles or path hashes.
        size_t keyLength = std::min(foldedTitle.size(), library::CLIX_FOLD_BYTES);
        while (keyLength > 0 && keyLength < foldedTitle.size() &&
               (static_cast<uint8_t>(foldedTitle[keyLength]) & 0xC0) == 0x80) {
          --keyLength;
        }
        const std::string_view foldedKey(foldedTitle.data(), keyLength);
        const bool keyIsComplete = keyLength == foldedTitle.size();
        uint16_t low = 0;
        uint16_t high = index.bookCount();
        while (low < high) {
          const uint16_t middle = static_cast<uint16_t>(low + (high - low) / 2);
          if (!index.readRecord(index.ordinalForRow(library::SortOrder::TitleAsc, middle), record)) break;
          const std::string_view foldedRecord(record.fold, record.foldLen);
          if (foldedRecord.compare(0, foldedKey.size(), foldedKey) < 0) low = middle + 1;
          else high = middle;
        }

        for (uint16_t row = low; row < index.bookCount(); ++row) {
          if (!index.readRecord(index.ordinalForRow(library::SortOrder::TitleAsc, row), record)) break;
          const std::string_view foldedRecord(record.fold, record.foldLen);
          if (foldedRecord.compare(0, foldedKey.size(), foldedKey) != 0) break;
          if (keyIsComplete && foldedRecord != foldedKey) break;
          std::string indexedTitle;
          if (bookHash == 0 || !keyIsComplete) {
            if (!index.readTitle(record, indexedTitle) && !index.readName(record, indexedTitle)) continue;
            if (indexedTitle != title) continue;
          }
          std::string indexedPath;
          if (!index.readPath(record, indexedPath)) break;
          if (bookHash != 0 && ReadingStatsJournal::bookHashForPath(indexedPath.c_str()) != bookHash) continue;
          bookPath = std::move(indexedPath);
          index.readAuthor(record, author);
          break;
        }
        index.close();
      }
    }

    if (bookPath.empty()) return std::string{};
    if (thumbBase.empty() && FsHelpers::hasReflowableBookExtension(bookPath)) {
      auto epub = makeUniqueNoThrow<Epub>(bookPath, "/.crosspoint");
      if (!epub) return std::string{};
      thumbBase = epub->getThumbBmpPath();
    } else if (thumbBase.empty() && FsHelpers::hasXtcExtension(bookPath)) {
      auto xtc = makeUniqueNoThrow<Xtc>(bookPath, "/.crosspoint");
      if (!xtc) return std::string{};
      thumbBase = xtc->getThumbBmpPath();
    }
    if (thumbBase.empty()) return std::string{};

    const std::string thumbPath = UITheme::getCoverThumbPath(thumbBase, COVER_HEIGHT);
    if (Storage.exists(thumbPath.c_str())) return thumbPath;
    if (FsHelpers::hasReflowableBookExtension(bookPath)) {
      auto epub = makeUniqueNoThrow<Epub>(bookPath, "/.crosspoint");
      if (epub) {
        const bool cacheLoaded = epub->load(false, true);
        if (cacheLoaded) epub->generateThumbBmp(COVER_HEIGHT);
        else epub->generateThumbBmpFromSource(COVER_HEIGHT);
      }
    } else if (FsHelpers::hasXtcExtension(bookPath)) {
      auto xtc = makeUniqueNoThrow<Xtc>(bookPath, "/.crosspoint");
      if (xtc && xtc->load()) xtc->generateThumbBmp(COVER_HEIGHT);
    }
    return Storage.exists(thumbPath.c_str()) ? thumbPath : std::string{};
  };

  currentCoverThumbPath = prepare(snapshot.currentBook, snapshot.currentBookHash, currentBookAuthor);
  lastCompletedCoverThumbPath = prepare(snapshot.lastCompletedBook, snapshot.lastCompletedBookHash,
                                        lastCompletedBookAuthor);
  if (!APP_STATE.openEpubPath.empty()) {
    const uint32_t openBookHash = ReadingStatsJournal::bookHashForPath(APP_STATE.openEpubPath.c_str());
    for (const auto& book : RECENT_BOOKS.getBooks()) {
      if (ReadingStatsJournal::bookHashForPath(book.path.c_str()) != openBookHash) continue;
      memcpy(snapshot.currentBook, book.title.c_str(), std::min(book.title.size(), sizeof(snapshot.currentBook) - 1));
      snapshot.currentBookHash = openBookHash;
      break;
    }
    currentCoverThumbPath = prepare(snapshot.currentBook, snapshot.currentBookHash, currentBookAuthor);
  }
}

void ReadingStatsActivity::setSelectedDate(const int year, const int month, const int day) {
  const int clampedMonth = std::clamp(month, 1, 12);
  const int clampedDay = std::clamp(day, 1, daysInMonth(year, clampedMonth));
  if (selectedYear == year && selectedMonth == clampedMonth && selectedDay == clampedDay) return;
  selectedYear = year;
  selectedMonth = clampedMonth;
  selectedDay = clampedDay;
  reloadSnapshot();
  requestUpdate();
}

void ReadingStatsActivity::moveDate(const int days) {
  std::tm local{};
  local.tm_year = selectedYear - 1900;
  local.tm_mon = selectedMonth - 1;
  local.tm_mday = selectedDay;
  local.tm_hour = 12;
  local.tm_isdst = -1;
  time_t value = mktime(&local);
  if (value == static_cast<time_t>(-1)) return;
  value += static_cast<time_t>(days) * 86400;
  localtime_r(&value, &local);
  setSelectedDate(local.tm_year + 1900, local.tm_mon + 1, local.tm_mday);
}

void ReadingStatsActivity::moveMonth(const int months) {
  std::tm local{};
  local.tm_year = selectedYear - 1900;
  local.tm_mon = selectedMonth - 1 + months;
  local.tm_mday = 1;
  local.tm_hour = 12;
  local.tm_isdst = -1;
  time_t value = mktime(&local);
  if (value == static_cast<time_t>(-1)) return;
  local.tm_mday = std::min(selectedDay, daysInMonth(local.tm_year + 1900, local.tm_mon + 1));
  value = mktime(&local);
  localtime_r(&value, &local);
  setSelectedDate(local.tm_year + 1900, local.tm_mon + 1, local.tm_mday);
}

void ReadingStatsActivity::moveYear(const int years) {
  setSelectedDate(selectedYear + years, selectedMonth, selectedDay);
}

void ReadingStatsActivity::togglePage() {
  page = page == Page::Activity ? Page::Overview : Page::Activity;
  if (page == Page::Activity) calendarHeaderFocused = true;
  requestUpdate();
}

void ReadingStatsActivity::toggleCalendarMode() {
  calendarMode = calendarMode == CalendarMode::Month ? CalendarMode::Year : CalendarMode::Month;
  calendarHeaderFocused = true;
  requestUpdate();
}

void ReadingStatsActivity::openGoalPicker() {
  const uint16_t currentGoal = ReadingStatsJournal::goalMinutes();
  auto activity = makeUniqueNoThrow<IntervalSelectionActivity>(
      renderer, mappedInput, "ReadingGoal", StrId::STR_READING_GOAL, currentGoal, GOAL_MINUTES_MIN, GOAL_MINUTES_MAX,
      GOAL_MINUTES_STEP, GOAL_MINUTES_STEP * 3, StrId::STR_GOAL_MINUTES_FORMAT);
  if (!activity) {
    LOG_ERR("STAT", "OOM: Reading goal picker");
    return;
  }
  startActivityForResult(std::move(activity), [this](const ActivityResult& result) {
    if (!result.isCancelled) {
      const auto minutes = static_cast<uint16_t>(std::get<IntervalResult>(result.data).value);
      if (ReadingStatsJournal::setGoalMinutes(minutes)) reloadSnapshot();
    }
    requestUpdate();
  });
}

void ReadingStatsActivity::handleTouch(const int x, const int y) {
  const int screenWidth = renderer.getScreenWidth();
  const auto& metrics = UITheme::getInstance().getMetrics();
  if (detailsOpen) {
    const int closeLeft = popupX + popupWidth - 38;
    if (x >= closeLeft && x < popupX + popupWidth && y >= popupY + 8 && y < popupY + 48) {
      detailsOpen = false;
      requestUpdate();
    }
    return;
  }
  if (y >= metrics.topPadding && y < metrics.topPadding + metrics.headerHeight && x >= screenWidth - 54) {
    togglePage();
    return;
  }
  if (page == Page::Activity && y >= calendarModeY && y < calendarModeY + 32) {
    toggleCalendarMode();
    return;
  }
  if (page == Page::Activity && calendarMode == CalendarMode::Month && y >= monthArrowY + 4 &&
      y < monthArrowY + 36) {
    if (x < monthArrowLeftX + 28) moveMonth(-1);
    else if (x >= monthArrowRightX - 20) moveMonth(1);
    return;
  }
  if (page == Page::Overview && y >= goalRowY && y < goalRowY + goalRowHeight) {
    openGoalPicker();
    return;
  }
  if (page == Page::Overview && y >= currentBookRowY && y < lastCompletedRowY && !APP_STATE.openEpubPath.empty()) {
    onSelectBook(APP_STATE.openEpubPath);
    return;
  }
  if (page != Page::Activity) return;

  if (calendarMode == CalendarMode::Month && x >= gridX && x < gridX + GRID_COLUMNS * (cellWidth + cellGap) - cellGap &&
      y >= gridY && y < gridY + 6 * (cellHeight + cellGap)) {
    const int offsetX = x - gridX;
    const int offsetY = y - gridY;
    const int col = offsetX / (cellWidth + cellGap);
    const int row = offsetY / (cellHeight + cellGap);
    if (offsetX % (cellWidth + cellGap) >= cellWidth || offsetY % (cellHeight + cellGap) >= cellHeight) return;
    const int day = row * GRID_COLUMNS + col - firstMondayOffset(selectedYear, selectedMonth) + 1;
    if (col >= 0 && col < GRID_COLUMNS && row >= 0 && row < 6 && day >= 1 && day <= daysInMonth(selectedYear, selectedMonth)) {
      setSelectedDate(selectedYear, selectedMonth, day);
      calendarHeaderFocused = false;
      detailsOpen = true;
      requestUpdate();
    }
    return;
  }

  if (calendarMode == CalendarMode::Year) {
    for (int month = 0; month < 12; ++month) {
      if (x >= monthRectsX[month] && x < monthRectsX[month] + monthRectsW[month] && y >= monthRectsY[month] &&
          y < monthRectsY[month] + monthRectsH[month]) {
        setSelectedDate(selectedYear, month + 1, 1);
        calendarMode = CalendarMode::Month;
        calendarHeaderFocused = false;
        requestUpdate();
        return;
      }
    }
  }
}

void ReadingStatsActivity::loop() {
  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    if (detailsOpen) {
      detailsOpen = false;
      requestUpdate();
      return;
    }
    if (page == Page::Overview) {
      page = Page::Activity;
      calendarHeaderFocused = true;
      requestUpdate();
      return;
    }
    if (!calendarHeaderFocused) {
      calendarHeaderFocused = true;
      requestUpdate();
      return;
    }
    finish();
    return;
  }
  if (mappedInput.wasLongPressed(MappedInputManager::Button::Confirm, 800)) {
    detailsOpen = false;
    togglePage();
    return;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    if (detailsOpen) {
      detailsOpen = false;
      requestUpdate();
    } else if (page == Page::Activity && calendarHeaderFocused) {
      toggleCalendarMode();
    } else if (page == Page::Activity && calendarMode == CalendarMode::Year) {
      setSelectedDate(selectedYear, selectedMonth, 1);
      calendarMode = CalendarMode::Month;
      calendarHeaderFocused = false;
      requestUpdate();
    } else if (page == Page::Activity) {
      detailsOpen = true;
      requestUpdate();
    }
    else openGoalPicker();
    return;
  }
  int x = 0;
  int y = 0;
  if (mappedInput.wasScreenTapped(x, y)) {
    handleTouch(x, y);
    return;
  }
  if (detailsOpen) return;
  // The X3's third and fourth front buttons are labeled Previous Page and
  // Next Page. Keep those labels actionable so both stats pages are reachable
  // without a touchscreen; the side Left/Right buttons still operate the
  // visible month/year arrows below.
  if (mappedInput.wasReleased(MappedInputManager::Button::Up)) {
    if (page == Page::Overview) {
      page = Page::Activity;
      requestUpdate();
    }
    return;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Down)) {
    if (page == Page::Activity) {
      page = Page::Overview;
      requestUpdate();
    }
    return;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Left) ||
      mappedInput.wasReleased(MappedInputManager::Button::Right)) {
    const int direction = mappedInput.wasReleased(MappedInputManager::Button::Left) ? -1 : 1;
    if (calendarHeaderFocused) {
      // First movement leaves the Month/Year control and lands on today's
      // date (or the latest saved day when the clock is unavailable).
      calendarHeaderFocused = false;
      requestUpdate();
    } else if (calendarMode == CalendarMode::Month) {
      moveDate(direction);
    } else {
      moveMonth(direction);
    }
    return;
  }
}

const char* ReadingStatsActivity::pageTitle() const {
  return page == Page::Activity ? tr(STR_READING_ACTIVITY) : tr(STR_READING_OVERVIEW);
}

void ReadingStatsActivity::drawTabs() {
  const auto& metrics = UITheme::getInstance().getMetrics();
  tabsY = metrics.topPadding;
  gridY = metrics.topPadding + metrics.headerHeight + TOP_INSET + 8;
  calendarModeY = gridY;
}

void ReadingStatsActivity::drawHeatCell(const int x, const int y, const int width, const int height,
                                        const ReadingStatsDay& day, const bool selected) {
  const uint8_t intensity = intensityFor(day.seconds, snapshot.goalMinutes);
  if (intensity > 0 && width > 4 && height > 4) {
    static constexpr uint8_t BAYER[4][4] = {{0, 8, 2, 10}, {12, 4, 14, 6}, {3, 11, 1, 9}, {15, 7, 13, 5}};
    const int inkThreshold = intensity == 4 ? 16 : intensity * 4;
    for (int py = 2; py < height - 2; ++py) {
      for (int px = 2; px < width - 2; ++px) {
        renderer.drawPixel(x + px, y + py, BAYER[py % 4][px % 4] < inkThreshold);
      }
    }
  }
  const int radius = std::min(4, std::max(1, std::min(width, height) / 7));
  renderer.drawRoundedRect(x, y, width, height, selected ? 2 : 1, radius, true);
}

void ReadingStatsActivity::drawMonthCalendar(const int month, const int year, const int x, const int y,
                                             const int cellW, const int cellH, const int gap, const bool large) {
  char dayLabel[4];
  const int firstDay = firstMondayOffset(year, month);
  const int numDays = daysInMonth(year, month);
  for (int day = 1; day <= numDays; ++day) {
    const int position = firstDay + day - 1;
    const int col = position % GRID_COLUMNS;
    const int row = position / GRID_COLUMNS;
    const int left = x + col * (cellW + gap);
    const int top = y + row * (cellH + gap);
    ReadingStatsDay stats{};
    const int index = dayOfYear(year, month, day);
    if (year == selectedYear && index >= 0 && index < ReadingStatsSnapshot::DAYS_IN_YEAR) stats = snapshot.days[index];
    const bool selected = !calendarHeaderFocused && year == selectedYear && month == selectedMonth && day == selectedDay;
    drawHeatCell(left, top, cellW, cellH, stats, selected);
    if (large) {
      snprintf(dayLabel, sizeof(dayLabel), "%d", day);
      const bool inverse = intensityFor(stats.seconds, snapshot.goalMinutes) >= 3;
      const int textWidth = renderer.getTextWidth(UI_10_FONT_ID, dayLabel);
      const int textHeight = renderer.getTextHeight(UI_10_FONT_ID);
      renderer.drawText(UI_10_FONT_ID, left + (cellW - textWidth) / 2,
                        top + (cellH - textHeight) / 2, dayLabel, !inverse);
    }
  }
}

void ReadingStatsActivity::drawWeeklyChart(const int topY) {
  const int width = renderer.getScreenWidth();
  int startMonth = static_cast<int>((snapshot.weekDayKeys[0] / 100) % 100);
  int startDay = static_cast<int>(snapshot.weekDayKeys[0] % 100);
  int endMonth = static_cast<int>((snapshot.weekDayKeys[6] / 100) % 100);
  int endDay = static_cast<int>(snapshot.weekDayKeys[6] % 100);
  char heading[64];
  snprintf(heading, sizeof(heading), "%s (%02d/%02d – %02d/%02d)", tr(STR_WEEKLY_READING_TIME), startMonth,
           startDay, endMonth, endDay);
  renderer.drawText(UI_10_FONT_ID, 20, topY, heading);
  const int slotWidth = (width - 40) / 7;
  const int barWidth = std::min(34, slotWidth - 12);
  const int left = 20;
  const int chartTop = topY + renderer.getLineHeight(UI_10_FONT_ID) + 4;
  constexpr int BAR_HEIGHT = 42;
  const int baseY = chartTop + BAR_HEIGHT;
  uint32_t maxSeconds = 1;
  for (const auto& day : snapshot.week) maxSeconds = std::max(maxSeconds, day.seconds);
  static constexpr StrId WEEKDAYS[] = {StrId::STR_MONDAY_SHORT,  StrId::STR_TUESDAY_SHORT, StrId::STR_WEDNESDAY_SHORT,
                                       StrId::STR_THURSDAY_SHORT, StrId::STR_FRIDAY_SHORT,  StrId::STR_SATURDAY_SHORT,
                                       StrId::STR_SUNDAY_SHORT};
  char label[16];
  for (int i = 0; i < 7; ++i) {
    const int slotX = left + i * slotWidth;
    const int x = slotX + (slotWidth - barWidth) / 2;
    const int barHeight = static_cast<int>((static_cast<uint64_t>(snapshot.week[i].seconds) * BAR_HEIGHT) / maxSeconds);
    renderer.drawLine(x, baseY, x + barWidth - 1, baseY, true);
    if (barHeight > 0) renderer.fillRectDither(x + 1, baseY - barHeight, barWidth - 2, barHeight, DarkGray);
    const uint64_t minutes = snapshot.week[i].seconds / 60;
    if (minutes >= 60) snprintf(label, sizeof(label), "%lluh%02llu", static_cast<unsigned long long>(minutes / 60),
                                static_cast<unsigned long long>(minutes % 60));
    else snprintf(label, sizeof(label), "%llum", static_cast<unsigned long long>(minutes));
    renderer.drawText(UI_10_FONT_ID, slotX + (slotWidth - renderer.getTextWidth(UI_10_FONT_ID, label)) / 2,
                      baseY + 2, label);
    const char* weekday = I18n::getInstance().get(WEEKDAYS[i]);
    renderer.drawText(UI_10_FONT_ID, slotX + (slotWidth - renderer.getTextWidth(UI_10_FONT_ID, weekday)) / 2,
                      baseY + renderer.getLineHeight(UI_10_FONT_ID) + 2, weekday);
  }
}

void ReadingStatsActivity::drawMetric(const int x, const int y, const char* label, const char* value,
                                      const int width) {
  renderer.drawText(UI_10_FONT_ID, x, y, label);
  const int valueWidth = renderer.getTextWidth(UI_12_FONT_ID, value);
  renderer.drawText(UI_12_FONT_ID, x + width - valueWidth, y, value);
}

void ReadingStatsActivity::drawActivityPage() {
  const int width = renderer.getScreenWidth();
  char value[40];
  drawTabs();
  const int contentX = 20;
  const int contentW = width - 40;
  calendarModeY = gridY;
  const int pillW = 208;
  const int pillH = 28;
  const int pillX = (width - pillW) / 2;
  renderer.drawRoundedRect(pillX, gridY, pillW, pillH, 1, 8, true);
  const int half = pillW / 2;
  if (calendarMode == CalendarMode::Month)
    renderer.fillRoundedRect(pillX + 1, gridY + 1, half - 1, pillH - 2, 7, Black);
  else
    renderer.fillRoundedRect(pillX + half, gridY + 1, half - 1, pillH - 2, 7, Black);
  const int modeY = gridY + (pillH - renderer.getTextHeight(UI_10_FONT_ID)) / 2;
  const int monthW = renderer.getTextWidth(UI_10_FONT_ID, tr(STR_MONTH));
  const int yearW = renderer.getTextWidth(UI_10_FONT_ID, tr(STR_YEAR));
  renderer.drawText(UI_10_FONT_ID, pillX + (half - monthW) / 2, modeY, tr(STR_MONTH),
                    calendarMode != CalendarMode::Month);
  renderer.drawText(UI_10_FONT_ID, pillX + half + (half - yearW) / 2, modeY, tr(STR_YEAR),
                    calendarMode != CalendarMode::Year);
  if (calendarHeaderFocused) renderer.drawRoundedRect(pillX - 3, gridY - 3, pillW + 6, pillH + 6, 1, 10, true);

  if (calendarMode == CalendarMode::Month) {
    cellWidth = std::min(52, (contentW - 6 * 4) / GRID_COLUMNS);
    cellHeight = 42;
    cellGap = 4;
    const int gridWidth = GRID_COLUMNS * cellWidth + (GRID_COLUMNS - 1) * cellGap;
    gridX = (width - gridWidth) / 2;
    monthTitleY = gridY + 46;
    monthArrowY = monthTitleY;
    monthArrowLeftX = contentX;
    monthArrowRightX = width - contentX - 12;
    renderer.drawText(UI_12_FONT_ID, monthArrowLeftX, monthArrowY, "<");
    renderer.drawText(UI_12_FONT_ID, monthArrowRightX, monthArrowY, ">");
    snprintf(value, sizeof(value), "%s %d", monthName(selectedMonth), selectedYear);
    const int headingY = monthTitleY;
    renderer.drawCenteredText(UI_12_FONT_ID, headingY, value);
    gridY = headingY + renderer.getLineHeight(UI_12_FONT_ID) + 19;
    static constexpr StrId WEEKDAYS[] = {StrId::STR_MONDAY_SHORT,  StrId::STR_TUESDAY_SHORT,
                                         StrId::STR_WEDNESDAY_SHORT, StrId::STR_THURSDAY_SHORT,
                                         StrId::STR_FRIDAY_SHORT,   StrId::STR_SATURDAY_SHORT,
                                         StrId::STR_SUNDAY_SHORT};
    for (int weekday = 0; weekday < 7; ++weekday) {
      const char* weekdayLabel = I18n::getInstance().get(WEEKDAYS[weekday]);
      const int labelW = renderer.getTextWidth(UI_10_FONT_ID, weekdayLabel);
      renderer.drawText(UI_10_FONT_ID, gridX + weekday * (cellWidth + cellGap) + (cellWidth - labelW) / 2,
                        gridY, weekdayLabel);
    }
    gridY += renderer.getLineHeight(UI_10_FONT_ID) + 4;
    drawMonthCalendar(selectedMonth, selectedYear, gridX, gridY, cellWidth, cellHeight, cellGap, true);
    const int calendarRows = (firstMondayOffset(selectedYear, selectedMonth) + daysInMonth(selectedYear, selectedMonth) + 6) / 7;
    // Reserve all six calendar rows so the summary and chart stay in place
    // when navigating between months with different row counts.
    const int calendarBottom = gridY + 6 * (cellHeight + cellGap) - cellGap;
    int legendY = calendarBottom + 12;
    const char* less = tr(STR_LESS);
    const char* more = tr(STR_MORE);
    const int legendWidth = renderer.getTextWidth(UI_10_FONT_ID, less) + 10 + 4 * 23 + 2 +
                            renderer.getTextWidth(UI_10_FONT_ID, more);
    const int legendX = gridX + (gridWidth - legendWidth) / 2;
    renderer.drawText(UI_10_FONT_ID, legendX, legendY + 2, less);
    int levelX = legendX + renderer.getTextWidth(UI_10_FONT_ID, less) + 10;
    for (int level = 1; level <= 4; ++level) {
      ReadingStatsDay example{};
      example.seconds = static_cast<uint32_t>(snapshot.goalMinutes * 60u * level / 4);
      drawHeatCell(levelX, legendY, 18, 18, example, false);
      levelX += 23;
    }
    renderer.drawText(UI_10_FONT_ID, levelX + 2, legendY + 2, more);
    const int summaryY = legendY + 29;
    renderer.drawLine(contentX, summaryY, width - contentX, summaryY, true);
    char daysRead[16], streak[16], totalTime[24];
    formatCount(snapshot.monthDaysRead, daysRead, sizeof(daysRead));
    snprintf(streak, sizeof(streak), "%u", static_cast<unsigned>(snapshot.currentStreak));
    formatShortDuration(snapshot.monthSeconds, totalTime, sizeof(totalTime));
    const int third = contentW / 3;
    const char* values[] = {daysRead, streak, totalTime};
    const char* labels[] = {tr(STR_DAYS_READ), tr(STR_DAY_STREAK), tr(STR_READING_TIME)};
    for (int i = 0; i < 3; ++i) {
      const int x = contentX + i * third;
      const int valW = renderer.getTextWidth(UI_12_FONT_ID, values[i]);
      renderer.drawText(UI_12_FONT_ID, x + (third - valW) / 2, summaryY + 3, values[i]);
      const int labW = renderer.getTextWidth(UI_10_FONT_ID, labels[i]);
      renderer.drawText(UI_10_FONT_ID, x + (third - labW) / 2, summaryY + 27, labels[i]);
      if (i > 0) renderer.drawLine(x, summaryY + 3, x, summaryY + 50, true);
    }
    const int weeklyY = summaryY + 57;
    renderer.drawLine(contentX, weeklyY - 2, width - contentX, weeklyY - 2, true);
    drawWeeklyChart(weeklyY + 4);
  } else {
    const int startX = 20;
    const int gapX = 12;
    const int gapY = 8;
    const int monthW = (width - 2 * startX - 2 * gapX) / 3;
    const int miniCell = std::max(8, std::min(15, (monthW - 10) / 7));
    const int miniGap = 2;
    const int monthH = 25 + 6 * (miniCell + miniGap) + 2;
    snprintf(value, sizeof(value), "%d", selectedYear);
    renderer.drawCenteredText(UI_12_FONT_ID, gridY + 45, value);
    for (int month = 0; month < 12; ++month) {
      const int col = month % 3;
      const int row = month / 3;
      const int x = startX + col * (monthW + gapX);
      const int y = gridY + 78 + row * (monthH + gapY);
      monthRectsX[month] = x;
      monthRectsY[month] = y;
      monthRectsW[month] = monthW;
      monthRectsH[month] = monthH;
      const char* name = monthName(month + 1);
      const int nameWidth = renderer.getTextWidth(UI_10_FONT_ID, name);
      renderer.drawText(UI_10_FONT_ID, x + (monthW - nameWidth) / 2, y + 1, name);
      const int dayX = x + (monthW - GRID_COLUMNS * miniCell - 6 * miniGap) / 2;
      drawMonthCalendar(month + 1, selectedYear, dayX, y + 25, miniCell, miniCell, miniGap, false);
      if (!calendarHeaderFocused && selectedMonth == month + 1)
        renderer.drawRoundedRect(x + 1, y, monthW - 2, monthH - 2, 1, 4, true);
    }
  }
}

void ReadingStatsActivity::drawSelectedDayPopup() {
  const int width = renderer.getScreenWidth();
  const int height = renderer.getScreenHeight();
  popupWidth = std::min(424, width - 24);
  popupHeight = 276;
  popupX = (width - popupWidth) / 2;
  popupY = (height - popupHeight) / 2;
  renderer.fillRectDither(0, 0, width, height, Color::LightGray);
  renderer.fillRoundedRect(popupX, popupY, popupWidth, popupHeight, 10, White);
  renderer.drawRoundedRect(popupX, popupY, popupWidth, popupHeight, 2, 10, true);
  char date[40], value[48], duration[32];
  static constexpr StrId WEEKDAYS[] = {StrId::STR_SUNDAY_SHORT, StrId::STR_MONDAY_SHORT, StrId::STR_TUESDAY_SHORT,
                                       StrId::STR_WEDNESDAY_SHORT, StrId::STR_THURSDAY_SHORT,
                                       StrId::STR_FRIDAY_SHORT, StrId::STR_SATURDAY_SHORT};
  std::tm dateParts{};
  dateParts.tm_year = selectedYear - 1900;
  dateParts.tm_mon = selectedMonth - 1;
  dateParts.tm_mday = selectedDay;
  mktime(&dateParts);
  snprintf(date, sizeof(date), "%s, %s %02d, %d", I18n::getInstance().get(WEEKDAYS[dateParts.tm_wday]),
           monthName(selectedMonth), selectedDay, selectedYear);
  renderer.drawText(UI_10_FONT_ID, popupX + 18, popupY + 18, date);
  renderer.drawText(UI_12_FONT_ID, popupX + popupWidth - 30, popupY + 10, "X");
  formatShortDuration(selectedDayStats().seconds, duration, sizeof(duration));
  renderer.drawText(NOTOSANS_18_FONT_ID, popupX + 18, popupY + 60, duration);
  const uint64_t goalSeconds = static_cast<uint64_t>(snapshot.goalMinutes) * 60;
  const unsigned percent = goalSeconds ? static_cast<unsigned>(std::min<uint64_t>(999, selectedDayStats().seconds * 100ull / goalSeconds)) : 0;
  snprintf(value, sizeof(value), tr(STR_DAILY_GOAL_PROGRESS_FORMAT), percent,
           static_cast<unsigned>(snapshot.goalMinutes));
  renderer.drawText(UI_10_FONT_ID, popupX + 18, popupY + 111, value);
  renderer.drawLine(popupX + 18, popupY + 137, popupX + popupWidth - 18, popupY + 137, true);
  const ReadingStatsDay day = selectedDayStats();
  char pages[20], sessions[20], average[24];
  formatCount(day.pages, pages, sizeof(pages));
  formatCount(day.sessions, sessions, sizeof(sessions));
  formatShortDuration(day.sessions ? day.seconds / day.sessions : 0, average, sizeof(average));
  const int colW = (popupWidth - 36) / 3;
  const char* values[] = {pages, sessions, average};
  const char* labels[] = {tr(STR_PAGES), tr(STR_SESSIONS), tr(STR_AVG_SESSION)};
  for (int i = 0; i < 3; ++i) {
    const int x = popupX + 18 + i * colW;
    renderer.drawText(UI_12_FONT_ID, x + (colW - renderer.getTextWidth(UI_12_FONT_ID, values[i])) / 2,
                      popupY + 160, values[i]);
    renderer.drawText(UI_10_FONT_ID, x + (colW - renderer.getTextWidth(UI_10_FONT_ID, labels[i])) / 2,
                      popupY + 190, labels[i]);
    if (i > 0) renderer.drawLine(x, popupY + 153, x, popupY + 226, true);
  }
}

void ReadingStatsActivity::drawOverviewPage() {
  const int width = renderer.getScreenWidth();
  drawTabs();
  const int left = 20;
  const int rowWidth = width - 40;
  int y = gridY + 26;
  char value[48], duration[40];
  goalRowY = y;
  goalRowHeight = 58;
  drawTargetIcon(renderer, left, y - 3);
  renderer.drawText(UI_12_FONT_ID, left + 34, y, tr(STR_READING_GOAL));
  snprintf(value, sizeof(value), tr(STR_GOAL_MINUTES_FORMAT), static_cast<unsigned>(snapshot.goalMinutes));
  renderer.drawText(UI_10_FONT_ID, left + 34, y + 24, value);
  renderer.drawText(UI_12_FONT_ID, width - left - 12, y + 7, ">");
  y += goalRowHeight;
  renderer.drawLine(left, y, width - left, y, true);

  y += 12;
  renderer.drawText(UI_12_FONT_ID, left, y, tr(STR_LIFETIME_STATS));
  y += 24;
  const int cellW = rowWidth / 2;
  const int cellH = 78;
  struct StatCell { const char* value; const char* label; } cells[4];
  formatShortDuration(snapshot.lifetimeSeconds, duration, sizeof(duration));
  cells[0] = {duration, tr(STR_TOTAL_READING_TIME)};
  formatCount(snapshot.completedBooks, value, sizeof(value));
  cells[1] = {value, tr(STR_BOOKS_COMPLETED)};
  char sessions[24], average[24];
  formatCount(snapshot.lifetimeSessions, sessions, sizeof(sessions));
  cells[2] = {sessions, tr(STR_SESSIONS)};
  formatShortDuration(snapshot.lifetimeSessions ? snapshot.lifetimeSeconds / snapshot.lifetimeSessions : 0,
                      average, sizeof(average));
  cells[3] = {average, tr(STR_AVERAGE_SESSION)};
  for (int i = 0; i < 4; ++i) {
    const int col = i % 2;
    const int row = i / 2;
    const int x = left + col * cellW;
    const int cy = y + row * cellH;
    renderer.drawText(UI_12_FONT_ID, x + 5, cy + 7, cells[i].value);
    renderer.drawText(UI_10_FONT_ID, x + 5, cy + 38, cells[i].label);
    if (col == 0) renderer.drawLine(left + cellW, cy, left + cellW, cy + cellH - 4, true);
    if (row == 0) renderer.drawLine(left, cy + cellH - 1, width - left, cy + cellH - 1, true);
  }
  y += 2 * cellH + 22;
  renderer.drawLine(left, y, width - left, y, true);

  auto drawBookRow = [&](const int rowY, const char* section, const char* title, const uint64_t seconds,
                         const int progress, const std::string& coverPath) {
    renderer.drawText(UI_12_FONT_ID, left, rowY, section);
    const int coverY = rowY + 37;
    constexpr int coverWidth = 64;
    constexpr int coverHeight = 94;
    const Rect coverRect{left + 3, coverY, coverWidth, coverHeight};
    bool drewCover = false;
    if (!coverPath.empty()) {
      HalFile coverFile;
      if (Storage.openFileForRead("STATS", coverPath, coverFile)) {
        Bitmap cover(coverFile);
        if (cover.parseHeaders() == BmpReaderError::Ok)
          drewCover = BaseTheme::drawCoverThumbFill(renderer, cover, coverRect);
        coverFile.close();
      }
    }
    if (!drewCover) GUI.drawCoverPlaceholder(renderer, coverRect);
    renderer.drawRect(coverRect.x, coverRect.y, coverRect.width, coverRect.height, true);
    const int textX = left + 80;
    const char* safeTitle = title[0] ? title : tr(STR_NO_READING_DATA);
    renderer.drawText(UI_10_FONT_ID, textX, coverY + 2, safeTitle);
    const std::string& author = progress >= 0 ? currentBookAuthor : lastCompletedBookAuthor;
    if (!author.empty()) renderer.drawText(UI_10_FONT_ID, textX, coverY + 22, author.c_str());
    if (progress >= 0) {
      renderer.drawRoundedRect(textX, coverY + 52, width - textX - left, 8, 1, 4, true);
      const int filled = (width - textX - left - 2) * std::clamp(progress, 0, 100) / 100;
      if (filled > 0) renderer.fillRoundedRect(textX + 1, coverY + 53, filled, 6, 3, Black);
    snprintf(value, sizeof(value), tr(STR_PROGRESS_COMPLETE_FORMAT), static_cast<unsigned>(std::clamp(progress, 0, 100)));
      renderer.drawText(UI_10_FONT_ID, textX, coverY + 68, value);
      formatShortDuration(seconds, value, sizeof(value));
      const int sw = renderer.getTextWidth(UI_10_FONT_ID, value);
      renderer.drawText(UI_10_FONT_ID, width - left - sw, coverY + 68, value);
    } else {
      char date[24];
      int year = static_cast<int>(snapshot.lastCompletedDayKey / 10000);
      int month = static_cast<int>((snapshot.lastCompletedDayKey / 100) % 100);
      int day = static_cast<int>(snapshot.lastCompletedDayKey % 100);
      if (snapshot.lastCompletedDayKey != 0) {
        char shortDate[16];
        snprintf(shortDate, sizeof(shortDate), "%02d/%02d/%04d", month, day, year);
        snprintf(date, sizeof(date), tr(STR_FINISHED_DATE_FORMAT), shortDate);
      } else {
        snprintf(date, sizeof(date), "%s", tr(STR_COMPLETED));
      }
      renderer.drawText(UI_10_FONT_ID, textX, coverY + 52, date);
      formatShortDuration(seconds, value, sizeof(value));
      const int sw = renderer.getTextWidth(UI_10_FONT_ID, value);
      renderer.drawText(UI_10_FONT_ID, width - left - sw, coverY + 68, value);
    }
  };

  y += 16;
  currentBookRowY = y;
  drawBookRow(y, tr(STR_CURRENTLY_READING), snapshot.currentBook, snapshot.currentBookSeconds,
              snapshot.currentBookProgressBasisPoints / 100, currentCoverThumbPath);
  y += 160;
  renderer.drawLine(left, y, width - left, y, true);
  y += 12;
  lastCompletedRowY = y;
  const char* lastCompletedTitle = snapshot.lastCompletedBook;
  drawBookRow(y, tr(STR_LAST_COMPLETED), lastCompletedTitle, snapshot.lastCompletedBookSeconds, -1,
              lastCompletedCoverThumbPath);
}

void ReadingStatsActivity::render(RenderLock&&) {
  renderer.clearScreen();
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int width = renderer.getScreenWidth();
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, width, metrics.headerHeight}, pageTitle());
  renderer.drawText(UI_12_FONT_ID, width - 27, metrics.topPadding + (metrics.headerHeight - 12) / 2, ">");
  if (page == Page::Activity) drawActivityPage();
  else drawOverviewPage();
  if (page == Page::Activity && detailsOpen) drawSelectedDayPopup();
  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), tr(STR_PREV_PAGE), tr(STR_NEXT_PAGE));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  renderer.displayBuffer();
}

#endif  // CROSSPOINT_ENABLE_READING_STATS
