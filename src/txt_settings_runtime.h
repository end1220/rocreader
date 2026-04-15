#pragma once

#include "input_manager.h"
#include "reader_session_state.h"
#include "ui_text_cache.h"

#include <SDL.h>

#include <functional>
#include <string>

struct TxtSettingsState {
  bool panel_active = false;
  int selected_row = 0;
  int selected_option = 0;
  int background_color = 0;
  int font_color = 0;
  int font_size_level = 2;
};

struct TxtSettingsCallbacks {
  std::function<void(TxtSettingsState &)> refresh_state;
  std::function<bool(int, TxtSettingsState &)> set_background_color;
  std::function<bool(int, TxtSettingsState &)> set_font_color;
  std::function<bool(int, TxtSettingsState &)> adjust_font_size;
  std::function<bool()> start_transcode;
};

struct TxtSettingsRenderDeps {
  SDL_Renderer *renderer = nullptr;
  SDL_Rect preview_rect{};
  const TxtSettingsState &state;
  const TxtTranscodeJob &txt_transcode_job;
  bool light_theme = false;
  int language_index = 0;
  int first_row_y = 0;
  int row_pitch = 42;
  int row_height = 30;
  std::function<void(int, int, int, int, SDL_Color, bool)> draw_rect;
  std::function<TextCacheEntry *(const std::string &, SDL_Color)> get_text_texture;
  std::function<TextCacheEntry *(const std::string &, SDL_Color)> get_emphasis_text_texture;
  std::function<std::string(const std::string &, size_t)> utf8_ellipsize;
};

int ClampTxtColorIndex(int value);
int ClampTxtFontSizeLevel(int value);
int TxtFontPointSizeForLevel(int level);
SDL_Color GetTxtBackgroundColor(int index);
SDL_Color GetTxtFontColor(int index);

bool HandleTxtSettingsInput(const InputManager &input, TxtSettingsState &state,
                            const TxtSettingsCallbacks &callbacks);
void DrawTxtSettingsPreview(const TxtSettingsRenderDeps &deps);
