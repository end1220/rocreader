#pragma once

#include "input_manager.h"
#include "app_language.h"
#include "system_controls.h"
#include "ui_text_cache.h"

#include <SDL.h>

#include <functional>

struct SystemSettingsState {
  bool panel_active = false;
  int selected_row = 0;
  int selected_button = 0;
  bool lid_close_screen_off_enabled = true;
  int auto_sleep_interval_index = 2;
  int system_language_index = 0;
  SystemControlLevels levels;
};

struct SystemSettingsCallbacks {
  std::function<void(SystemControlLevels &)> refresh_levels;
  std::function<bool(int, SystemControlLevels &)> adjust_volume;
  std::function<bool(int, SystemControlLevels &)> adjust_brightness;
  std::function<void(SystemSettingsState &)> refresh_lid_close_state;
  std::function<bool(bool, SystemSettingsState &)> set_lid_close_state;
  std::function<bool(int, SystemSettingsState &)> adjust_auto_sleep_interval;
  std::function<bool(int, SystemSettingsState &)> adjust_system_language;
  std::function<bool()> clear_cache;
  std::function<bool()> clear_history;
};

struct SystemSettingsRenderDeps {
  SDL_Renderer *renderer = nullptr;
  SDL_Rect preview_rect{};
  const SystemSettingsState &state;
  bool light_theme = false;
  int first_row_y = 0;
  int row_pitch = 42;
  int row_height = 30;
  float ui_scale = 1.0f;
  std::function<void(int, int, int, int, SDL_Color, bool)> draw_rect;
  std::function<TextCacheEntry *(const std::string &, SDL_Color)> get_text_texture;
  std::function<TextCacheEntry *(const std::string &, SDL_Color)> get_emphasis_text_texture;
};

bool HandleSystemSettingsInput(const InputManager &input, SystemSettingsState &state,
                               const SystemSettingsCallbacks &callbacks);
void DrawSystemSettingsPreview(const SystemSettingsRenderDeps &deps);
int ClampAutoSleepIntervalIndex(int value);
uint32_t AutoSleepIntervalMsFromIndex(int index);
const char *AutoSleepIntervalLabel(int index);
