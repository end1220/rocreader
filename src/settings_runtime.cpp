#include "settings_runtime.h"
#include "app_language.h"
#include "settings_panel_router.h"

#include <algorithm>
#include <cmath>
#include <string>

namespace {
int ScalePx(float scale, int value) {
  return std::max(1, static_cast<int>(std::round(static_cast<float>(value) * std::max(0.1f, scale))));
}

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

std::string MenuTitleText(int language_index) {
  return LocalizedAppText(language_index, AppTextId::MenuTitle);
}
}

void HandleSettingsInput(SettingsRuntimeInputDeps &deps) {
  SettingsRuntimeMenuState &menu = deps.menu;
  if (!deps.ui_cfg.animations) {
    menu.anim.Snap(menu.closing ? 0.0f : 1.0f);
  }
  menu.anim.Update(deps.dt);

  if (menu.closing && menu.anim.Value() <= 0.0001f && !menu.anim.IsAnimating()) {
    if (deps.actions.on_close) deps.actions.on_close();
    return;
  }

  if (!menu.close_armed) {
    const bool any_toggle_held =
        deps.input.IsPressed(Button::Start) || deps.input.IsPressed(Button::Select) ||
        deps.input.IsPressed(Button::Menu);
    if (!any_toggle_held) menu.close_armed = true;
  }

  const int menu_count = static_cast<int>(menu.items.size());
  const SettingId current_id =
      menu_count > 0 ? menu.items[std::clamp(menu.selected, 0, menu_count - 1)] : SettingId::KeyGuide;
  const bool system_settings_active =
      current_id == SettingId::SystemControls && deps.system_settings_state.panel_active;
  const bool txt_settings_active =
      current_id == SettingId::TxtToUtf8 && deps.txt_settings_state.panel_active;
  const bool avatar_grid_active =
      current_id == SettingId::ContributorAvatars && deps.contributor_avatar_state.grid_active;
  const bool version_update_active =
      current_id == SettingId::VersionUpdate && deps.version_update_state.panel_active;

  if (!system_settings_active && !txt_settings_active && !avatar_grid_active && !version_update_active &&
      menu.close_armed && menu.toggle_guard <= 0.0f &&
      !menu.closing &&
      (deps.input.IsJustPressed(Button::B) || deps.actions.menu_toggle_request)) {
    if (deps.ui_cfg.animations) menu.anim.AnimateTo(0.0f, 0.16f, animation::Ease::InOutCubic);
    else menu.anim.Snap(0.0f);
    menu.closing = true;
    return;
  }

  if (menu.closing) return;

  if (menu_count <= 0) return;

  const SettingId id = current_id;
  if (HandleSelectedSettingsPanelInput(id, deps)) return;

  if (deps.input.IsJustPressed(Button::Up) || deps.input.IsRepeated(Button::Up)) {
    menu.selected = (menu.selected - 1 + menu_count) % menu_count;
  } else if (deps.input.IsJustPressed(Button::Down) || deps.input.IsRepeated(Button::Down)) {
    menu.selected = (menu.selected + 1) % menu_count;
  } else if (deps.input.IsJustPressed(Button::A) || deps.input.IsJustPressed(Button::Right)) {
    HandleSelectedSettingsPanelConfirm(id, deps);
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

  deps.services.draw_rect(x, menu_y, menu_width, menu_h,
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
      deps.services.get_texture_size(preview_tex, pw, ph);
      SDL_Rect pd{preview_x, menu_y, pw, ph};
      if (selected == SettingId::VersionUpdate) {
        pd.x = preview_x + std::max(0, (preview_w - pw) / 2);
        pd.y = menu_y + std::max(0, (menu_h - ph) / 2);
      }
      SDL_RenderCopy(deps.renderer, preview_tex, nullptr, &pd);
      preview_rect = pd;
    }
  }

  deps.services.draw_rect(x, menu_y, menu_width, menu_h, SDL_Color{24, 34, 46, 255}, true);
  deps.services.draw_rect(x + menu_width - 1, menu_y, 1, menu_h, SDL_Color{82, 125, 158, 255}, true);

  const float scale = deps.layout.ui_scale;
  const int sidebar_margin_x = ScalePx(scale, 12);
  const int sidebar_text_pad_x = ScalePx(scale, 24);
  const int sidebar_item_h = ScalePx(scale, 30);
  const int sidebar_item_pitch = ScalePx(scale, 42);
  const int sidebar_indicator_w = ScalePx(scale, 3);
  int text_left = x + sidebar_text_pad_x;
  int y = menu_y + ScalePx(scale, 84) + deps.layout.settings_content_offset_y;
  int first_menu_item_y = y;
#ifdef HAVE_SDL2_TTF
  const std::string menu_title = MenuTitleText(language_index);
  const SDL_Color title_color{240, 246, 255, 255};
  const SDL_Color item_color{230, 236, 248, 255};
  TextCacheEntry *title_tex = deps.services.get_title_text_texture ? deps.services.get_title_text_texture(menu_title, title_color) : nullptr;
  const int title_max_w = std::max(0, menu_width - 20);
  const bool compact_sidebar = deps.layout.screen_w <= 640 || menu_width <= 160;
  if (compact_sidebar && title_tex && title_tex->w > title_max_w && deps.services.get_text_texture) {
    title_tex = deps.services.get_text_texture(menu_title, title_color);
  }
  int divider_y = menu_y + ScalePx(scale, 68) + deps.layout.settings_content_offset_y;
  if (title_tex && title_tex->texture) {
    const int side_margin = std::max(0, (menu_width - title_tex->w) / 2);
    const int title_x = x + side_margin;
    const int title_y = menu_y + ScalePx(scale, 8) + deps.layout.settings_content_offset_y;
    const int title_gap = ScalePx(scale, 8);
    divider_y = title_y + title_tex->h + title_gap;
    SDL_Rect td{title_x, title_y, title_tex->w, title_tex->h};
    SDL_RenderCopy(deps.renderer, title_tex->texture, nullptr, &td);
  }
  deps.services.draw_rect(x + ScalePx(scale, 8), divider_y, menu_width - ScalePx(scale, 16), ScalePx(scale, 1),
                 SDL_Color{66, 95, 124, 255}, true);
  y = divider_y + ScalePx(scale, 12);
  text_left = x + sidebar_text_pad_x;
  first_menu_item_y = y;
#else
  deps.services.draw_rect(x + 8, 72 + deps.layout.settings_content_offset_y, menu_width - 16, 1,
                 SDL_Color{66, 95, 124, 255}, true);
#endif

  for (size_t i = 0; i < deps.menu_items.size(); ++i) {
    const bool sel = static_cast<int>(i) == deps.menu_selected;
    const SDL_Color c = sel ? SDL_Color{63, 119, 158, 255} : SDL_Color{57, 73, 96, 214};
    deps.services.draw_rect(x + sidebar_margin_x, y, menu_width - sidebar_margin_x * 2, sidebar_item_h, c, true);
    if (sel) {
      deps.services.draw_rect(x + sidebar_margin_x, y, sidebar_indicator_w, sidebar_item_h,
                     SDL_Color{139, 214, 255, 255}, true);
      deps.services.draw_rect(x + sidebar_margin_x - ScalePx(scale, 1), y - ScalePx(scale, 1),
                     menu_width - sidebar_margin_x * 2 + ScalePx(scale, 2),
                     sidebar_item_h + ScalePx(scale, 2), SDL_Color{85, 152, 198, 208}, false);
    }
#ifdef HAVE_SDL2_TTF
    const std::string label_text = SettingLabel(deps.menu_items[i], language_index);
    if (!label_text.empty() && deps.services.get_text_texture) {
      TextCacheEntry *label_tex = deps.services.get_text_texture(label_text, item_color);
      if (label_tex && label_tex->texture) {
        const int ty = y + std::max(0, (sidebar_item_h - label_tex->h) / 2);
        SDL_Rect td{text_left, ty, label_tex->w, label_tex->h};
        SDL_RenderCopy(deps.renderer, label_tex->texture, nullptr, &td);
      }
    }
#endif
    y += sidebar_item_pitch;
  }

  const SettingId selected =
      deps.menu_items[std::clamp(deps.menu_selected, 0, static_cast<int>(deps.menu_items.size()) - 1)];
  DrawSelectedSettingsPanel(selected, deps, preview_rect, language_index, first_menu_item_y,
                            sidebar_item_pitch, sidebar_item_h, scale);

  auto draw_native_topmost_stretch_x = [&](SDL_Texture *tex, int px, int py, int target_w) {
    if (!tex) return;
    int tw = 0;
    int th = 0;
    deps.services.get_texture_size(tex, tw, th);
    if (tw <= 0 || th <= 0 || target_w <= 0) return;
    SDL_Rect dst{px, py, target_w, th};
    SDL_RenderCopy(deps.renderer, tex, nullptr, &dst);
  };

  if (deps.ui_assets.top_status_bar) {
    draw_native_topmost_stretch_x(deps.ui_assets.top_status_bar, 0, 0, deps.layout.screen_w);
  } else {
    deps.services.draw_rect(0, deps.layout.top_bar_y, deps.layout.screen_w, deps.layout.top_bar_h, SDL_Color{8, 10, 14, 255},
                   true);
  }

  if (deps.services.draw_volume_overlay) deps.services.draw_volume_overlay();

  if (deps.ui_assets.bottom_hint_bar) {
    int bw = 0;
    int bh = 0;
    deps.services.get_texture_size(deps.ui_assets.bottom_hint_bar, bw, bh);
    draw_native_topmost_stretch_x(deps.ui_assets.bottom_hint_bar, 0, deps.layout.screen_h - bh, deps.layout.screen_w);
  } else {
    deps.services.draw_rect(0, deps.layout.bottom_bar_y, deps.layout.screen_w, deps.layout.bottom_bar_h,
                   SDL_Color{8, 10, 14, 255}, true);
  }
}
