#pragma once

#include "app_stores.h"
#include "animation.h"
#include "input_manager.h"
#include "system_controls.h"

#include <cstdint>
#include <functional>
#include <string>

class VolumeController {
public:
  explicit VolumeController(bool prefer_system);

  bool UsesSystemVolume() const;
  bool AdjustUp();
  bool AdjustDown();
  bool RefreshPercent(int &out_percent);

private:
  bool AdjustBySteps(int delta_steps, int &out_percent);

  bool prefer_system_ = false;
  SystemControlService service_;
  SystemControlLevels levels_;
};

struct AppUiState {
  bool warned_system_volume_fallback = false;
  int volume_display_percent = 0;
  uint32_t volume_display_until = 0;
  bool menu_toggle_armed = true;
  float menu_toggle_cooldown = 0.0f;
};

enum class MenuToggleAction {
  None,
  CloseSettings,
  OpenFromShelf,
  OpenFromReader,
};

void TickAppUiState(AppUiState &state, float dt);
void HandleVolumeControls(AppUiState &state, const InputManager &input, uint32_t now,
                          VolumeController &volume_controller, ConfigStore &config,
                          const std::function<void(int)> &apply_sfx_volume,
                          const std::function<void()> &play_change_sfx);
MenuToggleAction HandleMenuToggleInput(AppUiState &state, const InputManager &input, bool is_settings,
                                       bool is_shelf, bool is_reader, bool settings_close_armed,
                                       float settings_toggle_guard, bool menu_closing, float debounce_sec);
