#include "app_runtime.h"

#include <SDL.h>

#include <algorithm>
#include <iostream>

VolumeController::VolumeController(bool prefer_system) : prefer_system_(prefer_system), service_(prefer_system) {}

bool VolumeController::UsesSystemVolume() const { return prefer_system_; }

bool VolumeController::AdjustUp() {
  int ignored_percent = 0;
  return AdjustBySteps(+1, ignored_percent);
}

bool VolumeController::AdjustDown() {
  int ignored_percent = 0;
  return AdjustBySteps(-1, ignored_percent);
}

bool VolumeController::RefreshPercent(int &out_percent) {
  if (!prefer_system_) return false;
  if (!service_.RefreshVolumeOnly(levels_.volume) || !levels_.volume.available) return false;
  out_percent = std::clamp(levels_.volume.level * 10, 0, 100);
  return true;
}

bool VolumeController::AdjustBySteps(int delta_steps, int &out_percent) {
  if (!prefer_system_) return false;
  std::cout << "[native_h700] volume adjust request: mode=system steps=" << delta_steps << "\n";
  if (!service_.AdjustVolume(delta_steps, levels_) || !levels_.volume.available) return false;
  out_percent = std::clamp(levels_.volume.level * 10, 0, 100);
  std::cout << "[native_h700] volume adjust success: percent=" << out_percent
            << " level=" << levels_.volume.level << "\n";
  return true;
}

void TickAppUiState(AppUiState &state, float dt) {
  state.menu_toggle_cooldown = std::max(0.0f, state.menu_toggle_cooldown - dt);
}

void HandleVolumeControls(AppUiState &state, const InputManager &input, uint32_t now,
                          VolumeController &volume_controller, ConfigStore &config,
                          const std::function<void(int)> &apply_sfx_volume,
                          const std::function<void()> &play_change_sfx) {
  const bool vol_up_pressed = input.IsJustPressed(Button::VolUp) || input.IsRepeated(Button::VolUp);
  const bool vol_down_pressed = input.IsJustPressed(Button::VolDown) || input.IsRepeated(Button::VolDown);
  if (!vol_up_pressed && !vol_down_pressed) return;

  std::cout << "[native_h700] volume handler: up=" << (vol_up_pressed ? "1" : "0")
            << " down=" << (vol_down_pressed ? "1" : "0")
            << " prefer_system=" << (volume_controller.UsesSystemVolume() ? "1" : "0")
            << " app_volume=" << config.Get().sfx_volume << "\n";

  bool system_volume_changed = false;
  int system_percent = state.volume_display_percent;
  if (volume_controller.UsesSystemVolume()) {
    if (vol_up_pressed) system_volume_changed = volume_controller.AdjustUp() || system_volume_changed;
    if (vol_down_pressed) system_volume_changed = volume_controller.AdjustDown() || system_volume_changed;
    if (system_volume_changed) {
      volume_controller.RefreshPercent(system_percent);
    }
    if (system_volume_changed) {
      std::cout << "[native_h700] system volume: "
                << (vol_up_pressed && vol_down_pressed ? "unchanged-step" : (vol_up_pressed ? "up" : "down"))
                << " percent=" << system_percent << "\n";
    } else if (!state.warned_system_volume_fallback) {
      state.warned_system_volume_fallback = true;
      std::cout << "[native_h700] system volume control unavailable, fallback to app sfx volume\n";
    }
  }

  if (!system_volume_changed) {
    NativeConfig &cfg = config.Mutable();
    const int old_volume = cfg.sfx_volume;
    if (vol_up_pressed) cfg.sfx_volume = std::min(SDL_MIX_MAXVOLUME, cfg.sfx_volume + 13);
    if (vol_down_pressed) cfg.sfx_volume = std::max(0, cfg.sfx_volume - 13);
    if (cfg.sfx_volume != old_volume) {
      if (apply_sfx_volume) apply_sfx_volume(cfg.sfx_volume);
      config.MarkDirty();
      std::cout << "[native_h700] sound volume: " << cfg.sfx_volume << "\n";
      if (cfg.audio && cfg.sfx_volume > 0 && play_change_sfx) {
        play_change_sfx();
      }
    }
    state.volume_display_percent =
        std::clamp((cfg.sfx_volume * 100) / std::max(1, SDL_MIX_MAXVOLUME), 0, 100);
  } else {
    NativeConfig &cfg = config.Mutable();
    const int clamped_percent = std::clamp(system_percent, 0, 100);
    if (cfg.system_volume_percent != clamped_percent) {
      cfg.system_volume_percent = clamped_percent;
      config.MarkDirty();
    }
    state.volume_display_percent = system_percent;
  }

  state.volume_display_until = now + 1500;
}

MenuToggleAction HandleMenuToggleInput(AppUiState &state, const InputManager &input, bool is_settings, bool is_shelf,
                                       bool is_reader, bool settings_close_armed, float settings_toggle_guard,
                                       bool menu_closing, float debounce_sec) {
  const bool start_just_pressed = input.IsJustPressed(Button::Start);
  const bool select_just_pressed = input.IsJustPressed(Button::Select);
  const bool menu_toggle_pressed = input.IsPressed(Button::Start) || input.IsPressed(Button::Select);
  if (!menu_toggle_pressed && state.menu_toggle_cooldown <= 0.0f) {
    state.menu_toggle_armed = true;
  }

  const bool menu_toggle_request = start_just_pressed || select_just_pressed;
  if (!menu_toggle_request || !state.menu_toggle_armed || state.menu_toggle_cooldown > 0.0f) {
    return MenuToggleAction::None;
  }

  state.menu_toggle_armed = false;
  state.menu_toggle_cooldown = debounce_sec;

  if (is_settings) {
    if (settings_close_armed && settings_toggle_guard <= 0.0f && !menu_closing) {
      return MenuToggleAction::CloseSettings;
    }
    return MenuToggleAction::None;
  }
  if (is_shelf) return MenuToggleAction::OpenFromShelf;
  if (is_reader) return MenuToggleAction::OpenFromReader;
  return MenuToggleAction::None;
}
