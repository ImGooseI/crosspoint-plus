#pragma once

#include "activities/UiListActivity.h"

class FrontlightScheduleActivity final : public UiListActivity {
 public:
  explicit FrontlightScheduleActivity(GfxRenderer& renderer, MappedInputManager& mappedInput);
  void onEnter() override;

 private:
  enum Item : int { Enabled = 0, Start, End, Count };
  int listCount() const override { return Count; }
  void buildScreen(UiScreen& screen) override;
  void activateIndex(int index) override;
  const char* headerTitle() const override;
  void openTimePicker(Item item);

  freeink::ui::ListItem rowItems_[Count]{};
  char startValue_[9]{};
  char endValue_[9]{};
};
