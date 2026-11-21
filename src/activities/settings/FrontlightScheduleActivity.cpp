#include "FrontlightScheduleActivity.h"

#include <GfxRenderer.h>
#include <HalClock.h>
#include <I18n.h>
#include <Logging.h>
#include <Memory.h>

#include <cstdio>
#include <memory>

#include "CrossPointSettings.h"
#include "MappedInputManager.h"
#include "activities/util/IntervalSelectionActivity.h"
#include "components/UITheme.h"

namespace fui = freeink::ui;

FrontlightScheduleActivity::FrontlightScheduleActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
    : UiListActivity("FrontlightSchedule", renderer, mappedInput) {}

void FrontlightScheduleActivity::onEnter() {
  UiListActivity::onEnter();
  rowItems_[Enabled].label = tr(STR_FRONTLIGHT_SCHEDULE);
  rowItems_[Start].label = tr(STR_SCHEDULE_START);
  rowItems_[End].label = tr(STR_SCHEDULE_END);
  for (int i = 0; i < Count; ++i) rowItems_[i].actionValue = static_cast<int16_t>(i);
}

const char* FrontlightScheduleActivity::headerTitle() const { return tr(STR_FRONTLIGHT_SCHEDULE); }

void FrontlightScheduleActivity::activateIndex(const int index) {
  nav.selected = index;
  app.clearTapFlash();
  if (index == Enabled) {
    SETTINGS.frontlightScheduleEnabled = !SETTINGS.frontlightScheduleEnabled;
    if (SETTINGS.frontlightScheduleEnabled) {
      if (SETTINGS.frontlightScheduleStartMinute >= 24 * 60) SETTINGS.frontlightScheduleStartMinute = 8 * 60;
      if (SETTINGS.frontlightScheduleEndMinute >= 24 * 60) SETTINGS.frontlightScheduleEndMinute = 22 * 60;
    }
    SETTINGS.saveToFile();
    requestUpdate();
  } else if (index == Start || index == End) {
    openTimePicker(static_cast<Item>(index));
  }
}

void FrontlightScheduleActivity::openTimePicker(const Item item) {
  uint16_t& stored = item == Start ? SETTINGS.frontlightScheduleStartMinute : SETTINGS.frontlightScheduleEndMinute;
  const int initial = stored < 24 * 60 ? stored : (item == Start ? 8 * 60 : 22 * 60);
  const StrId title = item == Start ? StrId::STR_SCHEDULE_START : StrId::STR_SCHEDULE_END;
  if (auto activity = makeUniqueNoThrow<IntervalSelectionActivity>(
          renderer, mappedInput, "FrontlightScheduleTime", title, initial, 0, 1439, 30, 30, StrId::STR_NONE_OPT,
          false, StrId::STR_NONE_OPT, true)) {
    startActivityForResult(std::move(activity), [this, item](const ActivityResult& result) {
      if (!result.isCancelled) {
        const uint16_t value = static_cast<uint16_t>(std::get<IntervalResult>(result.data).value);
        if (item == Start) {
          SETTINGS.frontlightScheduleStartMinute = value;
        } else {
          SETTINGS.frontlightScheduleEndMinute = value;
        }
        SETTINGS.saveToFile();
      }
      requestUpdate();
    });
  } else {
    LOG_ERR("FL SCHED", "OOM: schedule time picker");
  }
}

void FrontlightScheduleActivity::buildScreen(UiScreen& screen) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  screen.setContentMarginFromScreen(fui::Insets{static_cast<int16_t>(metrics.topPadding + metrics.headerHeight), 0,
                                                static_cast<int16_t>(metrics.buttonHintsHeight), 0});
  screen.spacer(static_cast<int16_t>(metrics.verticalSpacing));
  GUI.setCheckboxRow(rowItems_[Enabled], SETTINGS.frontlightScheduleEnabled != 0);

  auto formatTime = [](char* buffer, const size_t size, const uint16_t value) {
    if (value >= 24 * 60) {
      snprintf(buffer, size, "%s", tr(STR_NOT_SET));
      return;
    }
    int hour = value / 60;
    const int minute = value % 60;
    if (SETTINGS.clockFormat == 1) {
      const char* suffix = hour < 12 ? tr(STR_AM) : tr(STR_PM);
      hour %= 12;
      if (hour == 0) hour = 12;
      snprintf(buffer, size, "%d:%02d %s", hour, minute, suffix);
    } else {
      snprintf(buffer, size, "%02d:%02d", hour, minute);
    }
  };
  formatTime(startValue_, sizeof(startValue_), SETTINGS.frontlightScheduleStartMinute);
  formatTime(endValue_, sizeof(endValue_), SETTINGS.frontlightScheduleEndMinute);
  rowItems_[Start].value = startValue_;
  rowItems_[End].value = endValue_;

  fui::ListProps props;
  props.items = rowItems_;
  props.count = Count;
  props.action = ACTION_ROW;
  props.inputMask = fui::InputTouch;
  props.valueInset = 8;
  props.labelText = screen.theme().smallText;
  props.labelText.maxLines = 2;
  syncListViewport(screen, props);
  screen.list(props);
}
