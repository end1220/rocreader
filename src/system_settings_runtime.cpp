#include "system_settings_runtime.h"

#include <algorithm>
#include <array>
#include <string>

namespace {
struct RowSpec {
  const char *label;
  const SystemControlValue *value;
};

constexpr int kControlRowCount = 2;
constexpr int kSelectableRowCount = 7;
constexpr int kButtonMinus = 0;
constexpr int kButtonPlus = 1;
constexpr int kButtonSingle = 1;
constexpr std::array<const char *, 5> kSleepIntervalLabels = {{
    u8"30\u79d2",
    u8"1\u5206\u949f",
    u8"5\u5206\u949f",
    u8"10\u5206\u949f",
    u8"30\u5206\u949f",
}};
constexpr std::array<uint32_t, 5> kSleepIntervalValuesMs = {{
    30u * 1000u,
    60u * 1000u,
    5u * 60u * 1000u,
    10u * 60u * 1000u,
    30u * 60u * 1000u,
}};
std::array<RowSpec, kControlRowCount> BuildRows(const SystemSettingsState &state) {
  return {{
      {LocalizedAppText(state.system_language_index, AppTextId::SystemKeySound), &state.levels.volume},
      {LocalizedAppText(state.system_language_index, AppTextId::SystemBrightness), &state.levels.brightness},
  }};
}
} // namespace

int ClampAutoSleepIntervalIndex(int value) {
  return std::clamp(value, 0, static_cast<int>(kSleepIntervalLabels.size()) - 1);
}

uint32_t AutoSleepIntervalMsFromIndex(int index) {
  return kSleepIntervalValuesMs[static_cast<size_t>(ClampAutoSleepIntervalIndex(index))];
}

const char *AutoSleepIntervalLabel(int index) {
  return kSleepIntervalLabels[static_cast<size_t>(ClampAutoSleepIntervalIndex(index))];
}

const char *AutoSleepIntervalLocalizedLabel(int language_index, int interval_index) {
  return LocalizedSleepIntervalLabel(language_index, interval_index);
}

bool HandleSystemSettingsInput(const InputManager &input, SystemSettingsState &state,
                               const SystemSettingsCallbacks &callbacks) {
  if (!state.panel_active) {
    if (input.IsJustPressed(Button::A) || input.IsJustPressed(Button::Right)) {
      state.panel_active = true;
      state.selected_row = std::clamp(state.selected_row, 0, kSelectableRowCount - 1);
      if (state.selected_row >= 5) state.selected_button = kButtonSingle;
      else state.selected_button = std::clamp(state.selected_button, 0, 1);
      if (callbacks.refresh_levels) callbacks.refresh_levels(state.levels);
      if (callbacks.refresh_lid_close_state) callbacks.refresh_lid_close_state(state);
      return true;
    }
    return false;
  }

  if (input.IsJustPressed(Button::B)) {
    state.panel_active = false;
    return true;
  }

  if (input.IsJustPressed(Button::Up) || input.IsRepeated(Button::Up)) {
    state.selected_row = (state.selected_row - 1 + kSelectableRowCount) % kSelectableRowCount;
    state.selected_button = (state.selected_row >= 5) ? kButtonSingle : std::clamp(state.selected_button, 0, 1);
    return true;
  }
  if (input.IsJustPressed(Button::Down) || input.IsRepeated(Button::Down)) {
    state.selected_row = (state.selected_row + 1) % kSelectableRowCount;
    state.selected_button = (state.selected_row >= 5) ? kButtonSingle : std::clamp(state.selected_button, 0, 1);
    return true;
  }

  const bool dual_button_row = state.selected_row <= 4;
  if (input.IsJustPressed(Button::Left) && dual_button_row) {
    if (state.selected_button == kButtonPlus) {
      state.selected_button = kButtonMinus;
      return true;
    }
    state.selected_button = kButtonMinus;
  } else if (input.IsJustPressed(Button::Right) && dual_button_row) {
    if (state.selected_button == kButtonMinus) {
      state.selected_button = kButtonPlus;
      return true;
    }
    state.selected_button = kButtonPlus;
  }

  if (state.selected_row == 2) {
    if (input.IsJustPressed(Button::A) || input.IsRepeated(Button::A)) {
      const bool enabled = state.selected_button == kButtonMinus;
      if (callbacks.set_lid_close_state) {
        return callbacks.set_lid_close_state(enabled, state);
      }
      state.lid_close_screen_off_enabled = enabled;
      return true;
    }
    return false;
  }
  if (state.selected_row == 3) {
    int delta = 0;
    if (input.IsJustPressed(Button::A) || input.IsRepeated(Button::A)) {
      delta = (state.selected_button == kButtonPlus) ? 1 : -1;
    } else if (input.IsRepeated(Button::Right) && state.selected_button == kButtonPlus) {
      delta = 1;
    } else if (input.IsRepeated(Button::Left) && state.selected_button == kButtonMinus) {
      delta = -1;
    }
    if (delta != 0 && callbacks.adjust_auto_sleep_interval) {
      return callbacks.adjust_auto_sleep_interval(delta, state);
    }
    return delta != 0;
  }
  if (state.selected_row == 4) {
    int delta = 0;
    if (input.IsJustPressed(Button::A) || input.IsRepeated(Button::A)) {
      delta = (state.selected_button == kButtonPlus) ? 1 : -1;
    } else if (input.IsRepeated(Button::Right) && state.selected_button == kButtonPlus) {
      delta = 1;
    } else if (input.IsRepeated(Button::Left) && state.selected_button == kButtonMinus) {
      delta = -1;
    }
    if (delta != 0 && callbacks.adjust_system_language) {
      return callbacks.adjust_system_language(delta, state);
    }
    return delta != 0;
  }
  if (state.selected_row == 5) {
    if ((input.IsJustPressed(Button::A) || input.IsRepeated(Button::A)) && callbacks.clear_cache) {
      return callbacks.clear_cache();
    }
    return false;
  }
  if (state.selected_row == 6) {
    if ((input.IsJustPressed(Button::A) || input.IsRepeated(Button::A)) && callbacks.clear_history) {
      return callbacks.clear_history();
    }
    return false;
  }

  int delta = 0;
  if (input.IsJustPressed(Button::A) || input.IsRepeated(Button::A)) {
    delta = (state.selected_button == kButtonPlus) ? 1 : -1;
  } else if (input.IsRepeated(Button::Right) && state.selected_button == kButtonPlus) {
    delta = 1;
  } else if (input.IsRepeated(Button::Left) && state.selected_button == kButtonMinus) {
    delta = -1;
  }

  if (delta == 0) return false;

  bool handled = false;
  if (state.selected_row == 0 && callbacks.adjust_volume) {
    handled = callbacks.adjust_volume(delta, state.levels);
  } else if (state.selected_row == 1 && callbacks.adjust_brightness) {
    handled = callbacks.adjust_brightness(delta, state.levels);
  }
  if (!handled && callbacks.refresh_levels) callbacks.refresh_levels(state.levels);
  return true;
}

void DrawSystemSettingsPreview(const SystemSettingsRenderDeps &deps) {
  if (!deps.renderer) return;

  const auto rows = BuildRows(deps.state);
  const SDL_Color text_color = deps.light_theme ? SDL_Color{44, 50, 60, 255} : SDL_Color{236, 241, 247, 255};
  const SDL_Color muted_color = deps.light_theme ? SDL_Color{128, 134, 142, 255} : SDL_Color{149, 164, 181, 255};
  const SDL_Color button_fill = deps.light_theme ? SDL_Color{229, 224, 214, 240} : SDL_Color{29, 42, 57, 230};
  const SDL_Color button_selected = deps.light_theme ? SDL_Color{213, 228, 239, 250} : SDL_Color{41, 82, 113, 240};
  const SDL_Color button_border = deps.light_theme ? SDL_Color{104, 122, 144, 255} : SDL_Color{122, 201, 255, 255};
  const SDL_Color slot_fill = deps.light_theme ? SDL_Color{203, 209, 215, 255} : SDL_Color{62, 77, 92, 255};
  const SDL_Color slot_active = deps.light_theme ? SDL_Color{72, 122, 164, 255} : SDL_Color{122, 201, 255, 255};
  const SDL_Color state_lit = deps.light_theme ? SDL_Color{213, 228, 239, 250} : SDL_Color{63, 119, 158, 255};

  auto get_text_entry = [&](const std::string &text, SDL_Color color) -> TextCacheEntry * {
    if (!deps.get_text_texture) return nullptr;
    return deps.get_text_texture(text, color);
  };

  auto get_emphasis_entry = [&](const std::string &text, SDL_Color color) -> TextCacheEntry * {
    if (deps.get_emphasis_text_texture) return deps.get_emphasis_text_texture(text, color);
    if (deps.get_text_texture) return deps.get_text_texture(text, color);
    return nullptr;
  };

  const int button_size = 28;
  const int meter_h = 28;
  const int control_value_gap = 8;
  const int min_dual_button_w = 66;
  const int lid_button_gap = 16;
  const int min_action_button_w = 66;
  const int control_side_gap = 16;
  const int preview_padding_x = 16;
  const int label_control_gap = 18;
  const int top_divider_inset = 10;

  int max_value_w = 0;
  for (const std::string &text : {std::string("0"), std::string("100")}) {
    if (TextCacheEntry *entry = get_text_entry(text, text_color); entry) {
      max_value_w = std::max(max_value_w, entry->w);
    }
  }
  int max_label_w = 0;
  for (const auto &row : rows) {
    if (TextCacheEntry *entry = get_text_entry(row.label, text_color); entry) {
      max_label_w = std::max(max_label_w, entry->w);
    }
  }
  if (TextCacheEntry *entry = get_text_entry(LocalizedAppText(deps.state.system_language_index, AppTextId::SystemAutoSleep), text_color); entry) {
    max_label_w = std::max(max_label_w, entry->w);
  }
  if (TextCacheEntry *entry = get_text_entry(LocalizedAppText(deps.state.system_language_index, AppTextId::SystemSleepTimer), text_color); entry) {
    max_label_w = std::max(max_label_w, entry->w);
  }
  if (TextCacheEntry *entry = get_text_entry(LocalizedAppText(deps.state.system_language_index, AppTextId::SystemLanguage), text_color); entry) {
    max_label_w = std::max(max_label_w, entry->w);
  }
  if (TextCacheEntry *entry = get_text_entry(LocalizedAppText(deps.state.system_language_index, AppTextId::SystemClearCache), text_color); entry) {
    max_label_w = std::max(max_label_w, entry->w);
  }
  if (TextCacheEntry *entry = get_text_entry(LocalizedAppText(deps.state.system_language_index, AppTextId::SystemClearHistory), text_color); entry) {
    max_label_w = std::max(max_label_w, entry->w);
  }

  const int dual_button_padding = 16;
  const int action_button_padding = 18;
  const int on_off_max_w = std::max(
      get_text_entry(LocalizedAppText(deps.state.system_language_index, AppTextId::SystemOn), text_color)
              ? get_text_entry(LocalizedAppText(deps.state.system_language_index, AppTextId::SystemOn), text_color)->w
              : 0,
      get_text_entry(LocalizedAppText(deps.state.system_language_index, AppTextId::SystemOff), text_color)
              ? get_text_entry(LocalizedAppText(deps.state.system_language_index, AppTextId::SystemOff), text_color)->w
              : 0);
  const int clear_text_w =
      get_text_entry(LocalizedAppText(deps.state.system_language_index, AppTextId::SystemClear), text_color)
          ? get_text_entry(LocalizedAppText(deps.state.system_language_index, AppTextId::SystemClear), text_color)->w
          : 0;
  const int lid_button_w = std::max(min_dual_button_w, on_off_max_w + dual_button_padding * 2);
  const int action_button_w = std::max(min_action_button_w, clear_text_w + action_button_padding * 2);
  const int fixed_control_w =
      button_size + control_side_gap + control_side_gap + button_size + control_value_gap + max_value_w;
  const int single_row_total_w = max_label_w + label_control_gap + action_button_w;
  const int lid_total_w = max_label_w + label_control_gap + lid_button_w * 2 + lid_button_gap;
  int max_interval_w = 0;
  for (int i = 0; i < static_cast<int>(kSleepIntervalLabels.size()); ++i) {
    const char *label = AutoSleepIntervalLocalizedLabel(deps.state.system_language_index, i);
    if (TextCacheEntry *entry = get_text_entry(label, text_color); entry) {
      max_interval_w = std::max(max_interval_w, entry->w);
    }
  }
  int max_language_w = 0;
  for (int i = 0; i < SystemLanguageCount(); ++i) {
    const char *label = SystemLanguageDisplayLabel(i);
    if (TextCacheEntry *entry = get_text_entry(label, text_color); entry) {
      max_language_w = std::max(max_language_w, entry->w);
    }
  }
  const int interval_total_w = max_label_w + label_control_gap + button_size + 10 + max_interval_w + 10 + button_size;
  const int language_total_w =
      max_label_w + label_control_gap + button_size + 10 + max_language_w + 10 + button_size;
  const int meter_w =
      std::max(110, deps.preview_rect.w - preview_padding_x * 2 - max_label_w - label_control_gap - fixed_control_w);
  const int control_total_w = max_label_w + label_control_gap + fixed_control_w + meter_w;
  const int content_w = std::max({control_total_w, lid_total_w, interval_total_w, language_total_w, single_row_total_w});
  const int centered_x = deps.preview_rect.x + (deps.preview_rect.w - content_w) / 2;
  const int base_x = std::max(deps.preview_rect.x + 16, centered_x);
  const int control_right = base_x + content_w;
  const int plus_right = control_right - max_value_w - control_value_gap;
  const int clear_button_x = plus_right - action_button_w;
  const int lid_close_x = plus_right - lid_button_w;
  const int lid_open_x = lid_close_x - lid_button_gap - lid_button_w;

  const int row0_center_y = deps.first_row_y + deps.row_height / 2;
  const int row1_center_y = deps.first_row_y + deps.row_pitch + deps.row_height / 2;
  const int row2_center_y = deps.first_row_y + deps.row_pitch * 2 + deps.row_height / 2;
  const int row3_center_y = deps.first_row_y + deps.row_pitch * 3 + deps.row_height / 2;
  const int row4_center_y = deps.first_row_y + deps.row_pitch * 4 + deps.row_height / 2;
  const int row5_center_y = deps.first_row_y + deps.row_pitch * 5 + deps.row_height / 2;
  const int row6_center_y = deps.first_row_y + deps.row_pitch * 6 + deps.row_height / 2;
  deps.draw_rect(deps.preview_rect.x + top_divider_inset,
                 deps.first_row_y - 12,
                 std::max(0, deps.preview_rect.w - top_divider_inset * 2),
                 1,
                 deps.light_theme ? SDL_Color{138, 154, 170, 255} : SDL_Color{66, 95, 124, 255},
                 true);

  for (int i = 0; i < kControlRowCount; ++i) {
    const SystemControlValue &value = *rows[static_cast<size_t>(i)].value;
    const int control_center_y = (i == 0) ? row0_center_y : row1_center_y;

    if (TextCacheEntry *label_entry = get_text_entry(rows[static_cast<size_t>(i)].label, text_color);
        label_entry && label_entry->texture) {
      SDL_Rect label_dst{base_x, control_center_y - label_entry->h / 2, label_entry->w, label_entry->h};
      SDL_RenderCopy(deps.renderer, label_entry->texture, nullptr, &label_dst);
    }

    const int minus_x = base_x + max_label_w + label_control_gap;
    const int meter_x = minus_x + button_size + control_side_gap;
    const int plus_x = meter_x + meter_w + control_side_gap;
    const int button_y = control_center_y - button_size / 2;
    const int meter_y = control_center_y - meter_h / 2;

    const bool minus_selected =
        deps.state.panel_active && deps.state.selected_row == i && deps.state.selected_button == kButtonMinus;
    const bool plus_selected =
        deps.state.panel_active && deps.state.selected_row == i && deps.state.selected_button == kButtonPlus;

    deps.draw_rect(minus_x, button_y, button_size, button_size, minus_selected ? button_selected : button_fill, true);
    deps.draw_rect(minus_x, button_y, button_size, button_size, minus_selected ? button_border : muted_color, false);
    if (TextCacheEntry *minus_entry = get_emphasis_entry("-", text_color); minus_entry && minus_entry->texture) {
      SDL_Rect dst{minus_x + (button_size - minus_entry->w) / 2,
                   button_y + (button_size - minus_entry->h) / 2 - 2,
                   minus_entry->w,
                   minus_entry->h};
      SDL_RenderCopy(deps.renderer, minus_entry->texture, nullptr, &dst);
    }

    deps.draw_rect(meter_x, meter_y, meter_w, meter_h, slot_fill, true);
    const int max_level = std::max(1, value.max_level);
    const int clamped_level = value.available ? std::clamp(value.level, 0, max_level) : 0;
    const int fill_w = (meter_w * clamped_level) / max_level;
    if (fill_w > 0) {
      deps.draw_rect(meter_x, meter_y, fill_w, meter_h, slot_active, true);
    }
    deps.draw_rect(meter_x, meter_y, meter_w, meter_h, muted_color, false);

    deps.draw_rect(plus_x, button_y, button_size, button_size, plus_selected ? button_selected : button_fill, true);
    deps.draw_rect(plus_x, button_y, button_size, button_size, plus_selected ? button_border : muted_color, false);
    if (TextCacheEntry *plus_entry = get_emphasis_entry("+", text_color); plus_entry && plus_entry->texture) {
      SDL_Rect dst{plus_x + (button_size - plus_entry->w) / 2,
                   button_y + (button_size - plus_entry->h) / 2 - 2,
                   plus_entry->w,
                   plus_entry->h};
      SDL_RenderCopy(deps.renderer, plus_entry->texture, nullptr, &dst);
    }

    const std::string value_text =
        value.available ? std::to_string((std::clamp(value.level, 0, std::max(1, value.max_level)) * 100 +
                                          std::max(1, value.max_level) / 2) /
                                         std::max(1, value.max_level))
                        : std::string();
    if (!value_text.empty()) {
      if (TextCacheEntry *value_entry = get_text_entry(value_text, text_color);
        value_entry && value_entry->texture) {
        const int value_box_x = plus_x + button_size + control_value_gap;
        SDL_Rect dst{value_box_x + std::max(0, max_value_w - value_entry->w),
                     control_center_y - value_entry->h / 2,
                     value_entry->w,
                     value_entry->h};
        SDL_RenderCopy(deps.renderer, value_entry->texture, nullptr, &dst);
      }
    }
  }

  if (TextCacheEntry *lid_label =
          get_text_entry(LocalizedAppText(deps.state.system_language_index, AppTextId::SystemAutoSleep), text_color);
      lid_label && lid_label->texture) {
    SDL_Rect dst{base_x, row2_center_y - lid_label->h / 2, lid_label->w, lid_label->h};
    SDL_RenderCopy(deps.renderer, lid_label->texture, nullptr, &dst);
  }

  const int lid_button_y = row2_center_y - button_size / 2;
  const bool open_selected =
      deps.state.panel_active && deps.state.selected_row == 2 && deps.state.selected_button == kButtonMinus;
  const bool close_selected =
      deps.state.panel_active && deps.state.selected_row == 2 && deps.state.selected_button == kButtonPlus;

  deps.draw_rect(lid_open_x, lid_button_y, lid_button_w, button_size,
                 deps.state.lid_close_screen_off_enabled ? state_lit
                                                         : (open_selected ? button_selected : button_fill),
                 true);
  deps.draw_rect(lid_open_x, lid_button_y, lid_button_w, button_size, open_selected ? button_border : muted_color, false);
  if (TextCacheEntry *open_text =
          get_text_entry(LocalizedAppText(deps.state.system_language_index, AppTextId::SystemOn), text_color);
      open_text && open_text->texture) {
    SDL_Rect dst{lid_open_x + (lid_button_w - open_text->w) / 2,
                 lid_button_y + (button_size - open_text->h) / 2,
                 open_text->w,
                 open_text->h};
    SDL_RenderCopy(deps.renderer, open_text->texture, nullptr, &dst);
  }

  deps.draw_rect(lid_close_x, lid_button_y, lid_button_w, button_size,
                 !deps.state.lid_close_screen_off_enabled ? state_lit
                                                          : (close_selected ? button_selected : button_fill),
                 true);
  deps.draw_rect(lid_close_x, lid_button_y, lid_button_w, button_size, close_selected ? button_border : muted_color,
                 false);
  if (TextCacheEntry *close_text =
          get_text_entry(LocalizedAppText(deps.state.system_language_index, AppTextId::SystemOff), text_color);
      close_text && close_text->texture) {
    SDL_Rect dst{lid_close_x + (lid_button_w - close_text->w) / 2,
                 lid_button_y + (button_size - close_text->h) / 2,
                 close_text->w,
                 close_text->h};
    SDL_RenderCopy(deps.renderer, close_text->texture, nullptr, &dst);
  }

  if (TextCacheEntry *interval_label =
          get_text_entry(LocalizedAppText(deps.state.system_language_index, AppTextId::SystemSleepTimer), text_color);
      interval_label && interval_label->texture) {
    SDL_Rect dst{base_x, row3_center_y - interval_label->h / 2, interval_label->w, interval_label->h};
    SDL_RenderCopy(deps.renderer, interval_label->texture, nullptr, &dst);
  }

  const int selector_left_x = plus_right - button_size - 10 - max_language_w - 10 - button_size;
  const int selector_right_x = plus_right;
  const int selector_center_x = selector_left_x + (selector_right_x - selector_left_x) / 2;
  const int interval_button_y = row3_center_y - button_size / 2;
  const int interval_minus_x = selector_left_x;
  const int interval_plus_x = selector_right_x - button_size;
  const bool interval_minus_selected =
      deps.state.panel_active && deps.state.selected_row == 3 && deps.state.selected_button == kButtonMinus;
  const bool interval_plus_selected =
      deps.state.panel_active && deps.state.selected_row == 3 && deps.state.selected_button == kButtonPlus;

  deps.draw_rect(interval_minus_x, interval_button_y, button_size, button_size,
                 interval_minus_selected ? button_selected : button_fill, true);
  deps.draw_rect(interval_minus_x, interval_button_y, button_size, button_size,
                 interval_minus_selected ? button_border : muted_color, false);
  if (TextCacheEntry *minus_entry = get_emphasis_entry("<", text_color); minus_entry && minus_entry->texture) {
    SDL_Rect dst{interval_minus_x + (button_size - minus_entry->w) / 2,
                 interval_button_y + (button_size - minus_entry->h) / 2,
                 minus_entry->w,
                 minus_entry->h};
    SDL_RenderCopy(deps.renderer, minus_entry->texture, nullptr, &dst);
  }

  if (TextCacheEntry *value_entry =
          get_text_entry(AutoSleepIntervalLocalizedLabel(deps.state.system_language_index,
                                                        deps.state.auto_sleep_interval_index),
                         text_color);
      value_entry && value_entry->texture) {
    SDL_Rect dst{selector_center_x - value_entry->w / 2,
                 row3_center_y - value_entry->h / 2,
                 value_entry->w,
                 value_entry->h};
    SDL_RenderCopy(deps.renderer, value_entry->texture, nullptr, &dst);
  }

  deps.draw_rect(interval_plus_x, interval_button_y, button_size, button_size,
                 interval_plus_selected ? button_selected : button_fill, true);
  deps.draw_rect(interval_plus_x, interval_button_y, button_size, button_size,
                 interval_plus_selected ? button_border : muted_color, false);
  if (TextCacheEntry *plus_entry = get_emphasis_entry(">", text_color); plus_entry && plus_entry->texture) {
    SDL_Rect dst{interval_plus_x + (button_size - plus_entry->w) / 2,
                 interval_button_y + (button_size - plus_entry->h) / 2,
                 plus_entry->w,
                 plus_entry->h};
    SDL_RenderCopy(deps.renderer, plus_entry->texture, nullptr, &dst);
  }

  if (TextCacheEntry *language_label =
          get_text_entry(LocalizedAppText(deps.state.system_language_index, AppTextId::SystemLanguage), text_color);
      language_label && language_label->texture) {
    SDL_Rect dst{base_x, row4_center_y - language_label->h / 2, language_label->w, language_label->h};
    SDL_RenderCopy(deps.renderer, language_label->texture, nullptr, &dst);
  }

  const int language_button_y = row4_center_y - button_size / 2;
  const int language_minus_x = selector_left_x;
  const int language_value_x = language_minus_x + button_size + 10;
  const int language_plus_x = language_value_x + max_language_w + 10;
  const bool language_minus_selected =
      deps.state.panel_active && deps.state.selected_row == 4 && deps.state.selected_button == kButtonMinus;
  const bool language_plus_selected =
      deps.state.panel_active && deps.state.selected_row == 4 && deps.state.selected_button == kButtonPlus;

  deps.draw_rect(language_minus_x, language_button_y, button_size, button_size,
                 language_minus_selected ? button_selected : button_fill, true);
  deps.draw_rect(language_minus_x, language_button_y, button_size, button_size,
                 language_minus_selected ? button_border : muted_color, false);
  if (TextCacheEntry *minus_entry = get_emphasis_entry("<", text_color); minus_entry && minus_entry->texture) {
    SDL_Rect dst{language_minus_x + (button_size - minus_entry->w) / 2,
                 language_button_y + (button_size - minus_entry->h) / 2,
                 minus_entry->w,
                 minus_entry->h};
    SDL_RenderCopy(deps.renderer, minus_entry->texture, nullptr, &dst);
  }

  if (TextCacheEntry *value_entry = get_text_entry(SystemLanguageDisplayLabel(deps.state.system_language_index), text_color);
      value_entry && value_entry->texture) {
    SDL_Rect dst{language_value_x + std::max(0, (max_language_w - value_entry->w) / 2),
                 row4_center_y - value_entry->h / 2,
                 value_entry->w,
                 value_entry->h};
    SDL_RenderCopy(deps.renderer, value_entry->texture, nullptr, &dst);
  }

  deps.draw_rect(language_plus_x, language_button_y, button_size, button_size,
                 language_plus_selected ? button_selected : button_fill, true);
  deps.draw_rect(language_plus_x, language_button_y, button_size, button_size,
                 language_plus_selected ? button_border : muted_color, false);
  if (TextCacheEntry *plus_entry = get_emphasis_entry(">", text_color); plus_entry && plus_entry->texture) {
    SDL_Rect dst{language_plus_x + (button_size - plus_entry->w) / 2,
                 language_button_y + (button_size - plus_entry->h) / 2,
                 plus_entry->w,
                 plus_entry->h};
    SDL_RenderCopy(deps.renderer, plus_entry->texture, nullptr, &dst);
  }

  auto draw_action_row = [&](int row_center_y, const char *label, int selected_row) {
    if (TextCacheEntry *label_entry = get_text_entry(label, text_color); label_entry && label_entry->texture) {
      SDL_Rect dst{base_x, row_center_y - label_entry->h / 2, label_entry->w, label_entry->h};
      SDL_RenderCopy(deps.renderer, label_entry->texture, nullptr, &dst);
    }
    const bool selected =
        deps.state.panel_active && deps.state.selected_row == selected_row && deps.state.selected_button == kButtonSingle;
    const int button_y = row_center_y - button_size / 2;
    deps.draw_rect(clear_button_x, button_y, action_button_w, button_size, selected ? button_selected : button_fill, true);
    deps.draw_rect(clear_button_x, button_y, action_button_w, button_size, selected ? button_border : muted_color, false);
    if (TextCacheEntry *button_text =
            get_text_entry(LocalizedAppText(deps.state.system_language_index, AppTextId::SystemClear), text_color);
        button_text && button_text->texture) {
      SDL_Rect dst{clear_button_x + (action_button_w - button_text->w) / 2,
                   button_y + (button_size - button_text->h) / 2,
                   button_text->w,
                   button_text->h};
      SDL_RenderCopy(deps.renderer, button_text->texture, nullptr, &dst);
    }
  };

  draw_action_row(row5_center_y,
                  LocalizedAppText(deps.state.system_language_index, AppTextId::SystemClearCache),
                  5);
  draw_action_row(row6_center_y,
                  LocalizedAppText(deps.state.system_language_index, AppTextId::SystemClearHistory),
                  6);
}
