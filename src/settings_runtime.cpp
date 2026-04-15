#include "settings_runtime.h"
#include "app_language.h"
#include "contributor_avatar_runtime.h"
#include "system_settings_runtime.h"
#include "txt_settings_runtime.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <string>

namespace {
std::string SettingLabel(SettingId id, int language_index) {
  switch (id) {
  case SettingId::SystemControls: return LocalizedAppText(language_index, AppTextId::SettingSystemControls);
  case SettingId::KeyGuide: return LocalizedAppText(language_index, AppTextId::SettingKeyGuide);
  case SettingId::ClearHistory: return LocalizedAppText(language_index, AppTextId::SettingClearHistory);
  case SettingId::CleanCache: return LocalizedAppText(language_index, AppTextId::SettingClearCache);
  case SettingId::TxtToUtf8: return LocalizedAppText(language_index, AppTextId::SettingTxtToUtf8);
  case SettingId::ContributorAvatars: return LocalizedAppText(language_index, AppTextId::SettingContributorAvatars);
  case SettingId::ContactMe: return LocalizedAppText(language_index, AppTextId::SettingContactMe);
  case SettingId::VersionUpdate: return LocalizedAppText(language_index, AppTextId::SettingVersionUpdate);
  case SettingId::ExitApp: return LocalizedAppText(language_index, AppTextId::SettingExitApp);
  }
  return {};
}

SDL_Texture *SelectedPreviewTexture(const UiAssets &ui_assets, SettingId id) {
  switch (id) {
  case SettingId::SystemControls: return ui_assets.settings_preview_default;
  case SettingId::KeyGuide: return ui_assets.settings_preview_default;
  case SettingId::ClearHistory: return ui_assets.settings_preview_clean_history;
  case SettingId::CleanCache: return ui_assets.settings_preview_clean_cache;
  case SettingId::TxtToUtf8: return ui_assets.settings_preview_default;
  case SettingId::ContributorAvatars: return ui_assets.settings_preview_default;
  case SettingId::ContactMe: return ui_assets.settings_preview_contact;
  case SettingId::VersionUpdate: return ui_assets.settings_preview_default;
  case SettingId::ExitApp: return ui_assets.settings_preview_default;
  }
  return ui_assets.settings_preview_default;
}

std::string ExitHintText(int language_index) {
  return LocalizedAppText(language_index, AppTextId::ExitHint);
}

std::string MenuTitleText(int language_index) {
  return LocalizedAppText(language_index, AppTextId::MenuTitle);
}

struct KeyGuideLine {
  const char *button_label;
  AppTextId action_text;
};

constexpr std::array<KeyGuideLine, 9> kKeyGuideLines = {{
    {"D-Pad", AppTextId::KeyGuideActionDpad},
    {"A", AppTextId::KeyGuideActionA},
    {"B", AppTextId::KeyGuideActionB},
    {"X", AppTextId::KeyGuideActionX},
    {"Y", AppTextId::KeyGuideActionY},
    {"L1 / R1", AppTextId::KeyGuideActionShoulders},
    {"L2 / R2", AppTextId::KeyGuideActionTriggers},
    {"Menu / Start / Select", AppTextId::KeyGuideActionMenu},
    {"Vol+ / Vol-", AppTextId::KeyGuideActionVolume},
}};

size_t Utf8CharSize(unsigned char lead) {
  if ((lead & 0x80u) == 0) return 1;
  if ((lead & 0xE0u) == 0xC0u) return 2;
  if ((lead & 0xF0u) == 0xE0u) return 3;
  if ((lead & 0xF8u) == 0xF0u) return 4;
  return 1;
}

std::string TrimAsciiSpaces(std::string text) {
  while (!text.empty() && (text.front() == ' ' || text.front() == '\t')) {
    text.erase(text.begin());
  }
  while (!text.empty() && (text.back() == ' ' || text.back() == '\t')) {
    text.pop_back();
  }
  return text;
}

std::vector<std::string> WrapTextByWidth(
    const std::string &text, int max_width,
    const std::function<TextCacheEntry *(const std::string &, SDL_Color, bool)> &get_text,
    SDL_Color color, bool emphasis = false) {
  std::vector<std::string> lines;
  if (text.empty() || max_width <= 0) {
    if (!text.empty()) lines.push_back(text);
    return lines;
  }

  auto measure = [&](const std::string &value) -> int {
    if (TextCacheEntry *entry = get_text(value, color, emphasis); entry) return entry->w;
    return 0;
  };

  std::string current;
  size_t last_break = std::string::npos;
  for (size_t pos = 0; pos < text.size();) {
    const size_t char_size = std::min(Utf8CharSize(static_cast<unsigned char>(text[pos])), text.size() - pos);
    const std::string glyph = text.substr(pos, char_size);
    const std::string candidate = current + glyph;
    if (!current.empty() && measure(candidate) > max_width) {
      if (last_break != std::string::npos) {
        std::string line = TrimAsciiSpaces(current.substr(0, last_break));
        std::string remain = TrimAsciiSpaces(current.substr(last_break));
        if (!line.empty()) lines.push_back(line);
        current = remain + glyph;
      } else {
        lines.push_back(TrimAsciiSpaces(current));
        current = glyph;
      }
      last_break = std::string::npos;
      for (size_t i = 0; i < current.size(); ++i) {
        if (current[i] == ' ' || current[i] == '/' || current[i] == ';') last_break = i + 1;
      }
    } else {
      current = candidate;
      if (glyph == " " || glyph == "/" || glyph == ";") last_break = current.size();
    }
    pos += char_size;
  }

  current = TrimAsciiSpaces(current);
  if (!current.empty()) lines.push_back(current);
  return lines;
}

void DrawKeyGuidePreview(const SettingsRuntimeRenderDeps &deps, SDL_Rect preview_rect, int language_index,
                         int first_row_y) {
  if (!deps.renderer) return;

  const SDL_Color title_color{240, 246, 255, 255};
  const SDL_Color key_color{191, 221, 247, 255};
  const SDL_Color divider_color{66, 95, 124, 255};

  auto get_text = [&](const std::string &text, SDL_Color color, bool emphasis = false) -> TextCacheEntry * {
    if (emphasis && deps.get_title_text_texture) return deps.get_title_text_texture(text, color);
    if (deps.get_text_texture) return deps.get_text_texture(text, color);
    return nullptr;
  };

  const int left = preview_rect.x + 22;
  const int divider_y = first_row_y - 12;
  const bool use_34xxsp = deps.use_h700_34xx_keymap;
  const std::string profile_title = LocalizedAppText(
      language_index, use_34xxsp ? AppTextId::KeyGuideProfile34xxSp : AppTextId::KeyGuideProfileOtherH700);
  if (TextCacheEntry *profile = get_text(profile_title, title_color, true); profile && profile->texture) {
    SDL_Rect dst{left, divider_y - profile->h - 8, profile->w, profile->h};
    SDL_RenderCopy(deps.renderer, profile->texture, nullptr, &dst);
  }
  deps.draw_rect(preview_rect.x + 10,
                 divider_y,
                 std::max(0, preview_rect.w - 20),
                 1,
                 divider_color,
                 true);

  const int max_text_w = std::max(0, (preview_rect.x + preview_rect.w - 20) - left);
  const int line_gap = 4;
  const int row_gap = 10;
  const int start_y = divider_y + 16;
  int cursor_y = start_y;
  for (size_t i = 0; i < kKeyGuideLines.size(); ++i) {
    const auto &line = kKeyGuideLines[i];
    const std::string line_text =
        std::string(line.button_label) + ": " + LocalizedAppText(language_index, line.action_text);
    const std::vector<std::string> wrapped_lines = WrapTextByWidth(line_text, max_text_w, get_text, key_color);
    for (size_t line_index = 0; line_index < wrapped_lines.size(); ++line_index) {
      if (TextCacheEntry *line_entry = get_text(wrapped_lines[line_index], key_color);
          line_entry && line_entry->texture) {
        SDL_Rect dst{left, cursor_y, line_entry->w, line_entry->h};
        SDL_RenderCopy(deps.renderer, line_entry->texture, nullptr, &dst);
        cursor_y += line_entry->h + line_gap;
      }
    }
    cursor_y += row_gap;
  }
}

void DrawContactMePreview(const SettingsRuntimeRenderDeps &deps, SDL_Rect preview_rect, int language_index) {
  if (!deps.renderer || !deps.get_text_texture) return;

  const SDL_Color hint_color{240, 246, 255, 255};
  const std::string hint_text = LocalizedAppText(language_index, AppTextId::ContactRewardHint);
  if (hint_text.empty()) return;

  const int safe_x = preview_rect.x;
  const int safe_y = preview_rect.y + 35;
  const int safe_w = preview_rect.w;
  const int safe_h = 40;
  if (safe_w <= 0 || safe_h <= 0) return;

  using TextGetter = std::function<TextCacheEntry *(const std::string &, SDL_Color)>;
  std::vector<TextGetter> getters;
  if (deps.get_text_texture) getters.push_back(deps.get_text_texture);
  if (deps.get_reader_text_texture) getters.push_back(deps.get_reader_text_texture);

  std::vector<std::string> fitted_lines;
  int total_h = 0;
  int line_gap = 0;
  int max_line_w = 0;

  auto measure_lines = [&](const TextGetter &getter, int wrap_width) -> bool {
    fitted_lines = WrapTextByWidth(
        hint_text, wrap_width,
        [&](const std::string &text, SDL_Color color, bool) -> TextCacheEntry * { return getter ? getter(text, color) : nullptr; },
        hint_color);
    if (fitted_lines.empty()) return false;

    total_h = 0;
    line_gap = 2;
    max_line_w = 0;
    for (const std::string &line : fitted_lines) {
      TextCacheEntry *entry = getter ? getter(line, hint_color) : nullptr;
      if (!entry || !entry->texture) return false;
      max_line_w = std::max(max_line_w, entry->w);
      total_h += entry->h;
    }
    total_h += std::max(0, static_cast<int>(fitted_lines.size()) - 1) * line_gap;
    return true;
  };

  TextGetter render_getter = nullptr;
  float render_scale = 1.0f;
  for (const TextGetter &getter : getters) {
    if (!measure_lines(getter, safe_w)) continue;
    const float width_scale = (max_line_w > 0) ? (static_cast<float>(safe_w) / static_cast<float>(max_line_w)) : 1.0f;
    const float height_scale = (total_h > 0) ? (static_cast<float>(safe_h) / static_cast<float>(total_h)) : 1.0f;
    const float candidate_scale = std::min(1.0f, std::min(width_scale, height_scale));
    if (candidate_scale > 0.0f) {
      render_getter = getter;
      render_scale = candidate_scale;
      break;
    }
  }
  if (!render_getter) return;

  const int scaled_total_h = std::max(1, static_cast<int>(std::round(total_h * render_scale)));
  const int scaled_gap = std::max(0, static_cast<int>(std::round(line_gap * render_scale)));
  const int block_y = safe_y + std::max(0, (safe_h - scaled_total_h) / 2);
  int cursor_y = block_y;

  for (size_t i = 0; i < fitted_lines.size(); ++i) {
    TextCacheEntry *entry = render_getter(fitted_lines[i], hint_color);
    if (!entry || !entry->texture) continue;
    const int scaled_w = std::max(1, static_cast<int>(std::round(entry->w * render_scale)));
    const int scaled_h = std::max(1, static_cast<int>(std::round(entry->h * render_scale)));
    const int line_x = safe_x + std::max(0, (safe_w - scaled_w) / 2);
    SDL_Rect dst{line_x, cursor_y, scaled_w, scaled_h};
    SDL_RenderCopy(deps.renderer, entry->texture, nullptr, &dst);
    cursor_y += scaled_h + scaled_gap;
  }
}
}

void HandleSettingsInput(SettingsRuntimeInputDeps &deps) {
  if (!deps.ui_cfg.animations) {
    deps.menu_anim.Snap(deps.menu_closing ? 0.0f : 1.0f);
  }
  deps.menu_anim.Update(deps.dt);

  if (deps.menu_closing && deps.menu_anim.Value() <= 0.0001f && !deps.menu_anim.IsAnimating()) {
    if (deps.on_close) deps.on_close();
    return;
  }

  if (!deps.settings_close_armed) {
    const bool any_toggle_held =
        deps.input.IsPressed(Button::Start) || deps.input.IsPressed(Button::Select) ||
        deps.input.IsPressed(Button::Menu);
    if (!any_toggle_held) deps.settings_close_armed = true;
  }

  const int menu_count = static_cast<int>(deps.menu_items.size());
  const SettingId current_id =
      menu_count > 0 ? deps.menu_items[std::clamp(deps.menu_selected, 0, menu_count - 1)] : SettingId::KeyGuide;
  const bool system_settings_active =
      current_id == SettingId::SystemControls && deps.system_settings_state.panel_active;
  const bool txt_settings_active =
      current_id == SettingId::TxtToUtf8 && deps.txt_settings_state.panel_active;
  const bool avatar_grid_active =
      current_id == SettingId::ContributorAvatars && deps.contributor_avatar_state.grid_active;
  const bool version_update_active =
      current_id == SettingId::VersionUpdate && deps.version_update_state.panel_active;

  if (!system_settings_active && !txt_settings_active && !avatar_grid_active && !version_update_active &&
      deps.settings_close_armed && deps.settings_toggle_guard <= 0.0f &&
      !deps.menu_closing &&
      (deps.input.IsJustPressed(Button::B) || deps.menu_toggle_request)) {
    if (deps.ui_cfg.animations) deps.menu_anim.AnimateTo(0.0f, 0.16f, animation::Ease::InOutCubic);
    else deps.menu_anim.Snap(0.0f);
    deps.menu_closing = true;
    return;
  }

  if (deps.menu_closing) return;

  if (menu_count <= 0) return;

  const SettingId id = current_id;
  if (id == SettingId::SystemControls &&
      HandleSystemSettingsInput(deps.input, deps.system_settings_state, deps.system_settings_callbacks)) {
    return;
  }
  if (id == SettingId::TxtToUtf8 &&
      HandleTxtSettingsInput(deps.input, deps.txt_settings_state, deps.txt_settings_callbacks)) {
    return;
  }
  if (id == SettingId::ContributorAvatars &&
      HandleContributorAvatarInput(deps.input, deps.dt, deps.contributor_avatar_state, deps.contributor_avatar_count,
                                   deps.on_contributor_avatar_confirm)) {
    return;
  }
  if (id == SettingId::VersionUpdate &&
      HandleVersionUpdateInput(deps.input, deps.version_update_state, deps.version_update_callbacks)) {
    return;
  }

  if (deps.input.IsJustPressed(Button::Up) || deps.input.IsRepeated(Button::Up)) {
    deps.menu_selected = (deps.menu_selected - 1 + menu_count) % menu_count;
  } else if (deps.input.IsJustPressed(Button::Down) || deps.input.IsRepeated(Button::Down)) {
    deps.menu_selected = (deps.menu_selected + 1) % menu_count;
  } else if (deps.input.IsJustPressed(Button::A) || deps.input.IsJustPressed(Button::Right)) {
    if (id == SettingId::ExitApp) {
      if (deps.on_exit_app) deps.on_exit_app();
    } else if (id == SettingId::ClearHistory) {
      if (deps.on_clear_history) deps.on_clear_history();
    } else if (id == SettingId::CleanCache) {
      if (deps.on_clean_cache) deps.on_clean_cache();
    }
  }
}

void DrawSettingsRuntime(SettingsRuntimeRenderDeps &deps) {
  const int language_index = SystemLanguageIndexFromConfigValue(deps.cfg.system_language);
  const float anim_progress = deps.menu_anim.Value();
  const float eased =
      deps.cfg.animations ? animation::ApplyEase(animation::Ease::OutCubic, anim_progress) : anim_progress;
  const int menu_y = deps.layout.settings_y_offset;
  const int menu_h = std::max(0, deps.layout.screen_h - menu_y);
  const int menu_width = deps.layout.settings_sidebar_w;
  const int x = static_cast<int>(-menu_width + menu_width * eased);

  deps.draw_rect(x, menu_y, menu_width, menu_h,
                 SDL_Color{0, 0, 0, static_cast<Uint8>(eased * deps.sidebar_mask_max_alpha)}, true);

  const int preview_x = x + menu_width;
  const int preview_w = std::max(0, deps.layout.screen_w - preview_x);
  SDL_Rect preview_rect{preview_x, menu_y, preview_w, menu_h};
  if (preview_w > 0) {
    const SettingId selected =
        deps.menu_items[std::clamp(deps.menu_selected, 0, static_cast<int>(deps.menu_items.size()) - 1)];
    SDL_Texture *preview_tex = SelectedPreviewTexture(deps.ui_assets, selected);
    if (preview_tex) {
      int pw = 0;
      int ph = 0;
      deps.get_texture_size(preview_tex, pw, ph);
      SDL_Rect pd{preview_x, menu_y, pw, ph};
      if (selected == SettingId::VersionUpdate) {
        pd.x = preview_x + std::max(0, (preview_w - pw) / 2);
        pd.y = menu_y + std::max(0, (menu_h - ph) / 2);
      }
      SDL_RenderCopy(deps.renderer, preview_tex, nullptr, &pd);
      preview_rect = pd;
    }
  }

  deps.draw_rect(x, menu_y, menu_width, menu_h, SDL_Color{24, 34, 46, 255}, true);
  deps.draw_rect(x + menu_width - 1, menu_y, 1, menu_h, SDL_Color{82, 125, 158, 255}, true);

  int text_left = x + 32;
  int y = menu_y + 84 + deps.layout.settings_content_offset_y;
  int first_menu_item_y = y;
#ifdef HAVE_SDL2_TTF
  const std::string menu_title = MenuTitleText(language_index);
  const SDL_Color title_color{240, 246, 255, 255};
  const SDL_Color item_color{230, 236, 248, 255};
  TextCacheEntry *title_tex = deps.get_title_text_texture ? deps.get_title_text_texture(menu_title, title_color) : nullptr;
  const int title_max_w = std::max(0, menu_width - 20);
  const bool compact_sidebar = deps.layout.screen_w <= 640 || menu_width <= 160;
  if (compact_sidebar && title_tex && title_tex->w > title_max_w && deps.get_text_texture) {
    title_tex = deps.get_text_texture(menu_title, title_color);
  }
  int divider_y = menu_y + 68 + deps.layout.settings_content_offset_y;
  if (title_tex && title_tex->texture) {
    const int side_margin = std::max(0, (menu_width - title_tex->w) / 2);
    const int title_x = x + side_margin;
    const int title_y = menu_y + 8 + deps.layout.settings_content_offset_y;
    const int title_gap = 8;
    divider_y = title_y + title_tex->h + title_gap;
    SDL_Rect td{title_x, title_y, title_tex->w, title_tex->h};
    SDL_RenderCopy(deps.renderer, title_tex->texture, nullptr, &td);
  }
  deps.draw_rect(x + 8, divider_y, menu_width - 16, 1, SDL_Color{66, 95, 124, 255}, true);
  y = divider_y + 12;
  text_left = x + 32;
  first_menu_item_y = y;
#else
  deps.draw_rect(x + 8, 72 + deps.layout.settings_content_offset_y, menu_width - 16, 1,
                 SDL_Color{66, 95, 124, 255}, true);
#endif

  for (size_t i = 0; i < deps.menu_items.size(); ++i) {
    const bool sel = static_cast<int>(i) == deps.menu_selected;
    const SDL_Color c = sel ? SDL_Color{63, 119, 158, 255} : SDL_Color{57, 73, 96, 214};
    deps.draw_rect(x + 12, y, menu_width - 24, 30, c, true);
    if (sel) {
      deps.draw_rect(x + 12, y, 3, 30, SDL_Color{139, 214, 255, 255}, true);
      deps.draw_rect(x + 11, y - 1, menu_width - 22, 32, SDL_Color{85, 152, 198, 208}, false);
    }
#ifdef HAVE_SDL2_TTF
    const std::string label_text = SettingLabel(deps.menu_items[i], language_index);
    if (!label_text.empty() && deps.get_text_texture) {
      TextCacheEntry *label_tex = deps.get_text_texture(label_text, item_color);
      if (label_tex && label_tex->texture) {
        const int ty = y + std::max(0, (30 - label_tex->h) / 2);
        SDL_Rect td{text_left, ty, label_tex->w, label_tex->h};
        SDL_RenderCopy(deps.renderer, label_tex->texture, nullptr, &td);
      }
    }
#endif
    y += 42;
  }

  const SettingId selected =
      deps.menu_items[std::clamp(deps.menu_selected, 0, static_cast<int>(deps.menu_items.size()) - 1)];
  if (selected == SettingId::SystemControls) {
    DrawSystemSettingsPreview(SystemSettingsRenderDeps{
        deps.renderer,
        preview_rect,
        deps.system_settings_state,
        deps.cfg.theme != 0,
        first_menu_item_y,
        42,
        30,
        deps.draw_rect,
        deps.get_text_texture,
        deps.get_title_text_texture,
    });
  }
  if (selected == SettingId::KeyGuide) {
    DrawKeyGuidePreview(deps, preview_rect, language_index, first_menu_item_y);
  }
  if (selected == SettingId::ContactMe) {
    DrawContactMePreview(deps, preview_rect, language_index);
  }
  if (selected == SettingId::TxtToUtf8) {
    DrawTxtSettingsPreview(TxtSettingsRenderDeps{
        deps.renderer,
        preview_rect,
        deps.txt_settings_state,
        deps.txt_transcode_job,
        deps.cfg.theme != 0,
        language_index,
        first_menu_item_y,
        42,
        30,
        deps.draw_rect,
        deps.get_text_texture,
        deps.get_title_text_texture,
        deps.utf8_ellipsize,
    });
  }
  if (selected == SettingId::ContributorAvatars && !deps.contributor_avatar_entries.empty()) {
    DrawContributorAvatarPreview(ContributorAvatarRenderDeps{
        deps.renderer,
        preview_rect,
        deps.contributor_avatar_entries,
        deps.contributor_avatar_state,
        language_index,
        deps.draw_rect,
        deps.get_text_texture,
    });
  }
  if (selected == SettingId::VersionUpdate) {
    DrawVersionUpdatePreview(VersionUpdateRenderDeps{
        deps.renderer,
        preview_rect,
        deps.version_update_state,
        deps.cfg.theme != 0,
        language_index,
        deps.draw_rect,
        deps.get_text_texture,
        deps.get_title_text_texture,
    });
  }
  if (selected == SettingId::ExitApp && deps.get_title_text_texture) {
    const SDL_Color hint_color{240, 246, 255, 255};
    const std::string exit_hint = ExitHintText(language_index);
    TextCacheEntry *hint_tex = deps.get_title_text_texture(exit_hint, hint_color);
    if (!hint_tex || !hint_tex->texture) hint_tex = deps.get_text_texture ? deps.get_text_texture(exit_hint, hint_color) : nullptr;
    if (hint_tex && hint_tex->texture) {
      SDL_Rect dst{
          preview_rect.x + std::max(0, (preview_rect.w - hint_tex->w) / 2),
          preview_rect.y + std::max(0, (preview_rect.h - hint_tex->h) / 2),
          hint_tex->w,
          hint_tex->h,
      };
      SDL_RenderCopy(deps.renderer, hint_tex->texture, nullptr, &dst);
    }
  }

  auto draw_native_topmost = [&](SDL_Texture *tex, int px, int py) {
    if (!tex) return;
    int tw = 0;
    int th = 0;
    deps.get_texture_size(tex, tw, th);
    if (tw <= 0 || th <= 0) return;
    SDL_Rect dst{px, py, tw, th};
    SDL_RenderCopy(deps.renderer, tex, nullptr, &dst);
  };

  if (deps.ui_assets.top_status_bar) {
    draw_native_topmost(deps.ui_assets.top_status_bar, 0, 0);
  } else {
    deps.draw_rect(0, deps.layout.top_bar_y, deps.layout.screen_w, deps.layout.top_bar_h, SDL_Color{8, 10, 14, 255},
                   true);
  }

  if (deps.draw_volume_overlay) deps.draw_volume_overlay();

  if (deps.ui_assets.bottom_hint_bar) {
    int bw = 0;
    int bh = 0;
    deps.get_texture_size(deps.ui_assets.bottom_hint_bar, bw, bh);
    draw_native_topmost(deps.ui_assets.bottom_hint_bar, 0, deps.layout.screen_h - bh);
  } else {
    deps.draw_rect(0, deps.layout.bottom_bar_y, deps.layout.screen_w, deps.layout.bottom_bar_h,
                   SDL_Color{8, 10, 14, 255}, true);
  }
}
