#pragma once

#if defined(CROSSPOINT_ENABLE_READING_STATS)

#include "ReadingStatsJournal.h"
#include "activities/Activity.h"

#include <string>

class ReadingStatsActivity final : public Activity {
 public:
  explicit ReadingStatsActivity(GfxRenderer& renderer, MappedInputManager& mappedInput);
  void onEnter() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  enum class Page : uint8_t { Activity, Overview };
  enum class CalendarMode : uint8_t { Month, Year };

  ReadingStatsSnapshot snapshot;
  Page page = Page::Activity;
  CalendarMode calendarMode = CalendarMode::Month;
  int selectedYear = 0;
  int selectedMonth = 1;
  int selectedDay = 1;
  bool calendarHeaderFocused = true;
  int tabsY = 0;
  int calendarModeY = 0;
  int gridX = 0;
  int gridY = 0;
  int cellWidth = 0;
  int cellHeight = 0;
  int cellGap = 0;
  int monthRectsX[12]{};
  int monthRectsY[12]{};
  int monthRectsW[12]{};
  int monthRectsH[12]{};
  int goalRowY = 0;
  int goalRowHeight = 0;
  int currentBookRowY = 0;
  int lastCompletedRowY = 0;
  bool detailsOpen = false;
  int popupX = 0;
  int popupY = 0;
  int popupWidth = 0;
  int popupHeight = 0;
  std::string currentCoverThumbPath;
  std::string lastCompletedCoverThumbPath;
  std::string currentBookAuthor;
  std::string lastCompletedBookAuthor;
  int monthTitleY = 0;
  int monthArrowLeftX = 0;
  int monthArrowRightX = 0;
  int monthArrowY = 0;

  uint32_t selectedDayKey() const;
  ReadingStatsDay selectedDayStats() const;
  void reloadSnapshot();
  void prepareBookCovers();
  void setSelectedDate(int year, int month, int day);
  void moveDate(int days);
  void moveMonth(int months);
  void moveYear(int years);
  void togglePage();
  void toggleCalendarMode();
  void openGoalPicker();
  void handleTouch(int x, int y);
  void drawTabs();
  void drawActivityPage();
  void drawOverviewPage();
  void drawSelectedDayPopup();
  void drawHeatCell(int x, int y, int width, int height, const ReadingStatsDay& day, bool selected);
  void drawMonthCalendar(int month, int year, int x, int y, int cellW, int cellH, int gap, bool large);
  void drawWeeklyChart(int topY);
  void drawMetric(int x, int y, const char* label, const char* value, int width);
  const char* pageTitle() const;
};

#endif  // CROSSPOINT_ENABLE_READING_STATS
