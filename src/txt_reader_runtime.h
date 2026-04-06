#pragma once

#include "input_manager.h"
#include "reader_session_state.h"

#include <SDL.h>

#include <functional>

struct TxtReaderInputDeps {
  const InputManager &input;
  ReaderUiState &ui;
  float dt = 0.0f;
  int tap_step_px = 0;
  std::function<void(int)> text_scroll_by;
  std::function<void(int)> text_page_by;
};

struct TxtReaderRenderDeps {
  SDL_Renderer *renderer = nullptr;
  ReaderUiState &ui;
  std::function<void()> clamp_text_scroll;
  std::function<void(const SDL_Rect &)> set_clip_rect;
  std::function<void()> clear_clip_rect;
  std::function<void(const std::string &, int, int)> draw_text_line;
};

struct TxtProgressOverlayInputDeps {
  const InputManager &input;
  ReaderUiState &ui;
  float dt = 0.0f;
  int current_pct = 0;
  bool interaction_enabled = true;
  int tap_step_pct = 1;
  float hold_delay_sec = 0.0f;
  float hold_speed_min = 0.0f;
  float hold_speed_max = 0.0f;
  float hold_speed_accel = 0.0f;
  std::function<void(int)> jump_to_percent;
};

void HandleTxtReaderInput(TxtReaderInputDeps &deps);
void HandleTxtProgressOverlayInput(TxtProgressOverlayInputDeps &deps);
void DrawTxtReaderRuntime(TxtReaderRenderDeps &deps);
