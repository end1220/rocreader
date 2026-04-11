#include "txt_reader_runtime.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>

namespace {
int ClampPercentLocal(int value) {
  return std::max(0, std::min(100, value));
}

void SyncOverlayPreviewFromCurrent(ReaderUiState &ui, int current_pct) {
  const int clamped = ClampPercentLocal(current_pct);
  ui.progress_overlay_preview_pct = clamped;
  ui.progress_overlay_preview_pct_f = static_cast<float>(clamped);
}
}

void HandleTxtReaderInput(TxtReaderInputDeps &deps) {
  if (deps.ui.mode != ReaderMode::Txt || !deps.ui.txt_reader.open) return;

  std::array<Button, 2> vdirs = {Button::Up, Button::Down};
  for (Button b : vdirs) {
    int bi = static_cast<int>(b);
    const int long_dir = (b == Button::Down) ? 1 : -1;
    if (deps.input.IsPressed(b)) {
      const float hold = deps.input.HoldTime(b);
      const float delay = 0.30f;
      const float speed_min = 120.0f;
      const float speed_max = 620.0f;
      const float speed_accel = 860.0f;
      if (hold >= delay) {
        deps.ui.long_fired[bi] = true;
        deps.ui.hold_speed[bi] = (deps.ui.hold_speed[bi] <= 0.0f)
                                     ? speed_min
                                     : std::min(speed_max, deps.ui.hold_speed[bi] + speed_accel * deps.dt);
        const int step_px = std::max(1, static_cast<int>(deps.ui.hold_speed[bi] * deps.dt));
        deps.text_scroll_by(long_dir * step_px);
      } else {
        deps.ui.hold_speed[bi] = 0.0f;
      }
    } else {
      deps.ui.hold_speed[bi] = 0.0f;
    }
  }

  for (Button b : vdirs) {
    int bi = static_cast<int>(b);
    if (!deps.input.IsJustReleased(b)) continue;
    deps.ui.hold_speed[bi] = 0.0f;
    if (deps.ui.long_fired[bi]) {
      deps.ui.long_fired[bi] = false;
      continue;
    }
    const int tap_dir = (b == Button::Down) ? 1 : -1;
    deps.text_scroll_by(tap_dir * deps.tap_step_px);
  }

  if (deps.input.IsJustPressed(Button::Right)) {
    deps.text_page_by(1);
  } else if (deps.input.IsJustPressed(Button::Left)) {
    deps.text_page_by(-1);
  }

  deps.ui.progress.page = (deps.ui.txt_reader.line_h > 0) ? (deps.ui.txt_reader.scroll_px / deps.ui.txt_reader.line_h) : 0;
  deps.ui.progress.scroll_y = deps.ui.txt_reader.scroll_px;
  if (!deps.ui.txt_reader.line_source_offsets.empty()) {
    const size_t top_line = std::min(
        deps.ui.txt_reader.line_source_offsets.size() - 1,
        static_cast<size_t>(std::max(0, deps.ui.txt_reader.scroll_px /
                                               std::max(1, deps.ui.txt_reader.line_h))));
    deps.ui.progress.scroll_x = static_cast<int>(std::min<size_t>(
        deps.ui.txt_reader.line_source_offsets[top_line], static_cast<size_t>(std::numeric_limits<int>::max())));
  } else {
    deps.ui.progress.scroll_x = 0;
  }
}

void HandleTxtProgressOverlayInput(TxtProgressOverlayInputDeps &deps) {
  if (!deps.ui.progress_overlay_visible) return;

  if (!deps.interaction_enabled) {
    deps.ui.progress_overlay_scrubbing = false;
    deps.ui.progress_overlay_dirty = false;
    for (auto &value : deps.ui.progress_overlay_hold_speed) value = 0.0f;
    for (auto &value : deps.ui.progress_overlay_long_fired) value = false;
    SyncOverlayPreviewFromCurrent(deps.ui, deps.current_pct);
    return;
  }

  std::array<Button, 2> hdirs = {Button::Left, Button::Right};
  for (Button b : hdirs) {
    const int bi = static_cast<int>(b);
    const int dir = (b == Button::Right) ? 1 : -1;
    if (deps.input.IsJustPressed(b)) {
      SyncOverlayPreviewFromCurrent(deps.ui, deps.current_pct);
      deps.ui.progress_overlay_hold_speed[bi] = 0.0f;
      deps.ui.progress_overlay_long_fired[bi] = false;
    }
    if (deps.input.IsPressed(b)) {
      const float hold = deps.input.HoldTime(b);
      if (hold >= deps.hold_delay_sec) {
        if (!deps.ui.progress_overlay_scrubbing || !deps.ui.progress_overlay_dirty) {
          SyncOverlayPreviewFromCurrent(deps.ui, deps.current_pct);
        }
        deps.ui.progress_overlay_scrubbing = true;
        deps.ui.progress_overlay_dirty = true;
        deps.ui.progress_overlay_long_fired[bi] = true;
        const float next_speed = (deps.ui.progress_overlay_hold_speed[bi] <= 0.0f)
                                     ? deps.hold_speed_min
                                     : std::min(deps.hold_speed_max,
                                                deps.ui.progress_overlay_hold_speed[bi] +
                                                    deps.hold_speed_accel * deps.dt);
        deps.ui.progress_overlay_hold_speed[bi] = next_speed;
        deps.ui.progress_overlay_preview_pct_f += static_cast<float>(dir) * next_speed * deps.dt;
        deps.ui.progress_overlay_preview_pct_f = std::clamp(deps.ui.progress_overlay_preview_pct_f, 0.0f, 100.0f);
        deps.ui.progress_overlay_preview_pct = ClampPercentLocal(
            static_cast<int>(std::lround(deps.ui.progress_overlay_preview_pct_f)));
      }
    } else {
      deps.ui.progress_overlay_hold_speed[bi] = 0.0f;
    }
  }

  for (Button b : hdirs) {
    const int bi = static_cast<int>(b);
    if (!deps.input.IsJustReleased(b)) continue;
    deps.ui.progress_overlay_hold_speed[bi] = 0.0f;
    if (deps.ui.progress_overlay_long_fired[bi] && deps.ui.progress_overlay_dirty) {
      deps.jump_to_percent(deps.ui.progress_overlay_preview_pct);
      SyncOverlayPreviewFromCurrent(deps.ui, deps.ui.progress_overlay_preview_pct);
      deps.ui.progress_overlay_dirty = false;
    } else {
      const int dir = (b == Button::Right) ? 1 : -1;
      const int target_pct = ClampPercentLocal(deps.current_pct + dir * deps.tap_step_pct);
      deps.jump_to_percent(target_pct);
      SyncOverlayPreviewFromCurrent(deps.ui, target_pct);
      deps.ui.progress_overlay_dirty = false;
    }
    deps.ui.progress_overlay_long_fired[bi] = false;
    const bool any_lr_held = deps.input.IsPressed(Button::Left) || deps.input.IsPressed(Button::Right);
    deps.ui.progress_overlay_scrubbing = any_lr_held;
  }

  if (!deps.input.IsPressed(Button::Left) && !deps.input.IsPressed(Button::Right) &&
      !deps.ui.progress_overlay_dirty) {
    deps.ui.progress_overlay_scrubbing = false;
    SyncOverlayPreviewFromCurrent(deps.ui, deps.current_pct);
  }
}

void DrawTxtReaderRuntime(TxtReaderRenderDeps &deps) {
  if (deps.ui.mode != ReaderMode::Txt || !deps.ui.txt_reader.open) return;

  deps.clamp_text_scroll();
  const SDL_Rect clip{
      deps.ui.txt_reader.viewport_x,
      deps.ui.txt_reader.viewport_y,
      deps.ui.txt_reader.viewport_w,
      deps.ui.txt_reader.viewport_h,
  };
  deps.set_clip_rect(clip);
  const int text_x = deps.ui.txt_reader.viewport_x + 2;
  const int start_line = std::max(0, deps.ui.txt_reader.scroll_px / std::max(1, deps.ui.txt_reader.line_h));
  int y = deps.ui.txt_reader.viewport_y - (deps.ui.txt_reader.scroll_px % std::max(1, deps.ui.txt_reader.line_h));
  for (int i = start_line; i < static_cast<int>(deps.ui.txt_reader.lines.size()); ++i) {
    if (y > deps.ui.txt_reader.viewport_y + deps.ui.txt_reader.viewport_h) break;
    deps.draw_text_line(deps.ui.txt_reader.lines[i], text_x, y);
    y += deps.ui.txt_reader.line_h;
  }
  deps.clear_clip_rect();
}
