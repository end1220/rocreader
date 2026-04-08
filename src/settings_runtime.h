#pragma once

#include "app_stores.h"
#include "animation.h"
#include "contributor_avatar_runtime.h"
#include "input_manager.h"
#include "reader_session_state.h"
#include "system_settings_runtime.h"
#include "ui_assets.h"
#include "ui_text_cache.h"

#include <SDL.h>

#include <functional>
#include <string>
#include <vector>

enum class SettingId { SystemControls, KeyGuide, ClearHistory, CleanCache, TxtToUtf8, ContributorAvatars, ContactMe, ExitApp };

struct SettingsRuntimeInputDeps {
  const InputManager &input;
  const NativeConfig &ui_cfg;
  float dt = 0.0f;
  bool &menu_closing;
  bool &settings_close_armed;
  float &settings_toggle_guard;
  int &menu_selected;
  const std::vector<SettingId> &menu_items;
  animation::TweenFloat &menu_anim;
  SystemSettingsState &system_settings_state;
  SystemSettingsCallbacks system_settings_callbacks;
  ContributorAvatarState &contributor_avatar_state;
  size_t contributor_avatar_count = 0;
  std::function<void(int)> on_contributor_avatar_confirm;
  bool menu_toggle_request = false;
  std::function<void()> on_close;
  std::function<void()> on_exit_app;
  std::function<void()> on_clear_history;
  std::function<void()> on_clean_cache;
  std::function<void()> on_txt_to_utf8;
};

struct SettingsRuntimeLayout {
  int screen_w = 0;
  int screen_h = 0;
  int top_bar_y = 0;
  int top_bar_h = 0;
  int bottom_bar_y = 0;
  int bottom_bar_h = 0;
  int settings_sidebar_w = 0;
  int settings_y_offset = 0;
  int settings_content_offset_y = 0;
};

struct SettingsRuntimeRenderDeps {
  SDL_Renderer *renderer = nullptr;
  UiAssets &ui_assets;
  const NativeConfig &cfg;
  const std::vector<SettingId> &menu_items;
  int menu_selected = 0;
  animation::TweenFloat &menu_anim;
  int sidebar_mask_max_alpha = 0;
  const TxtTranscodeJob &txt_transcode_job;
  const SystemSettingsState &system_settings_state;
  const std::vector<ContributorAvatarEntry> &contributor_avatar_entries;
  const ContributorAvatarState &contributor_avatar_state;
  SettingsRuntimeLayout layout;
  std::function<void(int, int, int, int, SDL_Color, bool)> draw_rect;
  std::function<void(SDL_Texture *, int &, int &)> get_texture_size;
  std::function<TextCacheEntry *(const std::string &, SDL_Color)> get_text_texture;
  std::function<TextCacheEntry *(const std::string &, SDL_Color)> get_title_text_texture;
  std::function<std::string(const std::string &, size_t)> utf8_ellipsize;
  std::function<void()> draw_volume_overlay;
};

void HandleSettingsInput(SettingsRuntimeInputDeps &deps);
void DrawSettingsRuntime(SettingsRuntimeRenderDeps &deps);
