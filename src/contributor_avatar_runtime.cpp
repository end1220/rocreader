#include "contributor_avatar_runtime.h"

#include "app_language.h"
#include "sdl_utils.h"

#include <algorithm>
#include <codecvt>
#include <fstream>
#include <locale>
#include <cmath>
#include <limits>
#include <string>
#include <unordered_set>
#include <unordered_map>
#include <vector>

namespace {
struct AvatarCandidate {
  std::filesystem::path path;
  double contribution = 0.0;
  int avatar_index = 0;
  std::string label;
  bool contribution_is_max = false;
};

std::string WideToUtf8(const std::wstring &text) {
  std::wstring_convert<std::codecvt_utf8_utf16<wchar_t>> converter;
  return converter.to_bytes(text);
}

std::wstring Utf8ToWide(const std::string &text) {
  std::wstring_convert<std::codecvt_utf8_utf16<wchar_t>> converter;
  return converter.from_bytes(text);
}

bool ParseAvatarCandidateName(const std::wstring &filename, AvatarCandidate &candidate) {
  if (filename.rfind(L"avatar_", 0) != 0) return false;

  const size_t extension_pos = filename.find_last_of(L'.');
  const std::wstring stem = extension_pos == std::wstring::npos ? filename : filename.substr(0, extension_pos);
  const size_t index_end = stem.find(L'_', 7);
  if (index_end == std::wstring::npos) return false;

  int avatar_index = 0;
  try {
    avatar_index = std::stoi(stem.substr(7, index_end - 7));
  } catch (...) {
    return false;
  }

  const std::wstring contribution_marker = L"\u8d21\u732e\u503c";
  const size_t contribution_pos = stem.find(contribution_marker);
  if (contribution_pos == std::wstring::npos) return false;

  const size_t value_start = contribution_pos + contribution_marker.size();
  if (value_start > stem.size()) return false;

  const std::wstring contribution_text_raw = stem.substr(value_start);
  std::wstring contribution_text = contribution_text_raw;
  contribution_text.erase(
      std::remove_if(contribution_text.begin(), contribution_text.end(),
                     [](wchar_t ch) { return ch == L' ' || ch == L'\t'; }),
      contribution_text.end());
  if (contribution_text.empty()) return false;

  double contribution = 0.0;
  if (contribution_text == L"MAX" || contribution_text == L"max") {
    contribution = 0.0;
    candidate.contribution_is_max = true;
  } else {
    try {
      contribution = std::stod(contribution_text);
    } catch (...) {
      return false;
    }
  }

  const std::wstring label_w = stem.substr(index_end + 1);
  candidate.contribution = contribution;
  candidate.avatar_index = avatar_index;
  candidate.label = WideToUtf8(label_w);
  return !candidate.label.empty();
}

bool ParseAvatarCandidate(const std::filesystem::path &path, AvatarCandidate &candidate) {
  candidate.path = path;
  return ParseAvatarCandidateName(path.filename().wstring(), candidate);
}

int ParseRankFrameOrderName(const std::string &filename) {
  const std::string prefix = "AvatarFrame_NO.";
  if (filename.rfind(prefix, 0) != 0) return -1;

  const size_t ext_pos = filename.find_last_of('.');
  if (ext_pos == std::string::npos || ext_pos <= prefix.size()) return -1;

  const size_t order_end = filename.find('.', prefix.size());
  const size_t number_end = (order_end == std::string::npos) ? ext_pos : order_end;
  if (number_end <= prefix.size()) return -1;

  try {
    return std::stoi(filename.substr(prefix.size(), number_end - prefix.size()));
  } catch (...) {
    return -1;
  }
}

int ParseRankFrameOrder(const std::filesystem::path &path) { return ParseRankFrameOrderName(path.filename().string()); }

struct PackedUiAsset {
  std::string name;
  std::vector<unsigned char> payload;
};

void XorUiPayload(std::vector<unsigned char> &data, const std::string &name) {
  static const std::string key = "ROCreader::native_h700::ui_pack";
  if (name.empty()) return;
  for (size_t i = 0; i < data.size(); ++i) {
    const unsigned char k = static_cast<unsigned char>(key[i % key.size()]);
    const unsigned char n = static_cast<unsigned char>(name[i % name.size()]);
    data[i] = static_cast<unsigned char>(data[i] ^ k ^ n ^ static_cast<unsigned char>((i * 131u) & 0xFFu));
  }
}

std::vector<PackedUiAsset> LoadPackedAvatarAssets(const std::filesystem::path &exe_path) {
  std::vector<std::filesystem::path> ui_pack_paths = {
      exe_path / "ui.pack",
      exe_path / ".." / "ui.pack",
      std::filesystem::current_path() / "ui.pack",
  };

  for (const auto &pack_path : ui_pack_paths) {
    if (!std::filesystem::exists(pack_path)) continue;
    std::ifstream in(pack_path, std::ios::binary);
    if (!in) continue;
    char magic[8] = {};
    in.read(magic, sizeof(magic));
    if (!in || std::string(magic, sizeof(magic)) != "RCUIPK01") continue;

    uint32_t count = 0;
    in.read(reinterpret_cast<char *>(&count), sizeof(count));
    if (!in) continue;

    std::vector<PackedUiAsset> packed_assets;
    packed_assets.reserve(count);
    bool ok = true;
    for (uint32_t i = 0; i < count; ++i) {
      uint16_t name_len = 0;
      uint32_t original_size = 0;
      uint32_t enc_size = 0;
      in.read(reinterpret_cast<char *>(&name_len), sizeof(name_len));
      if (!in) {
        ok = false;
        break;
      }

      std::string name(name_len, '\0');
      if (name_len > 0) in.read(name.data(), static_cast<std::streamsize>(name_len));
      in.read(reinterpret_cast<char *>(&original_size), sizeof(original_size));
      in.read(reinterpret_cast<char *>(&enc_size), sizeof(enc_size));
      if (!in) {
        ok = false;
        break;
      }

      std::vector<unsigned char> payload(enc_size, 0);
      if (enc_size > 0) in.read(reinterpret_cast<char *>(payload.data()), static_cast<std::streamsize>(enc_size));
      if (!in) {
        ok = false;
        break;
      }

      XorUiPayload(payload, name);
      if (payload.size() != original_size) {
        ok = false;
        break;
      }

      const std::string leaf_name = std::filesystem::path(name).filename().string();
      const bool is_avatar = leaf_name.rfind("avatar_", 0) == 0;
      const bool is_rank_frame = ParseRankFrameOrderName(leaf_name) >= 0;
      if (is_avatar || is_rank_frame) {
        packed_assets.push_back(PackedUiAsset{name, std::move(payload)});
      }
    }

    if (ok && !packed_assets.empty()) return packed_assets;
  }

  return {};
}

int MaxScrollRowForCount(size_t entry_count) {
  const int total_rows = static_cast<int>((entry_count + 2) / 3);
  return std::max(0, total_rows - 2);
}

void EnsureFocusedVisible(ContributorAvatarState &state, size_t entry_count) {
  const int max_scroll_row = MaxScrollRowForCount(entry_count);
  const int focus_row = state.focus_index / 3;
  if (focus_row < state.scroll_row) {
    state.scroll_row = focus_row;
  } else if (focus_row > state.scroll_row + 1) {
    state.scroll_row = focus_row - 1;
  }
  state.scroll_row = std::clamp(state.scroll_row, 0, max_scroll_row);
}

constexpr float kAvatarNameMarqueePauseSec = 0.0f;
constexpr float kAvatarNameMarqueeSpeedPx = 84.0f;
constexpr float kAvatarNameMarqueeGapPx = 4.0f;

float ComputeEffectiveMarqueeDt(float dt, ContributorAvatarState &state) {
  const uint32_t now = SDL_GetTicks();
  float effective_dt = dt;
  if (state.marquee_last_tick_ms != 0) {
    const float tick_dt =
        static_cast<float>(now - state.marquee_last_tick_ms) / 1000.0f;
    if (tick_dt > 0.0f) effective_dt = tick_dt;
  }
  state.marquee_last_tick_ms = now;
  return std::clamp(effective_dt, 0.0f, 0.05f);
}
}

void DestroyContributorAvatarEntries(std::vector<ContributorAvatarEntry> &entries,
                                     const std::function<void(SDL_Texture *)> &before_destroy) {
  std::unordered_set<SDL_Texture *> destroyed_rank_frames;
  for (ContributorAvatarEntry &entry : entries) {
    if (entry.rank_frame && destroyed_rank_frames.insert(entry.rank_frame).second) {
      if (before_destroy) before_destroy(entry.rank_frame);
      SDL_DestroyTexture(entry.rank_frame);
    }
    entry.rank_frame = nullptr;
    if (!entry.texture) continue;
    if (before_destroy) before_destroy(entry.texture);
    SDL_DestroyTexture(entry.texture);
    entry.texture = nullptr;
  }
  entries.clear();
}

void LoadContributorAvatarEntries(std::vector<ContributorAvatarEntry> &entries, const std::filesystem::path &ui_root,
                                  const std::filesystem::path &exe_path, SDL_Renderer *renderer, int language_index,
                                  const std::function<SDL_Surface *(const void *, size_t)> &load_surface_from_memory,
                                  const std::function<void(SDL_Texture *, int, int)> &remember_texture_size,
                                  const std::function<void(SDL_Texture *)> &before_destroy) {
  DestroyContributorAvatarEntries(entries, before_destroy);
  if (!renderer) return;

  std::vector<AvatarCandidate> candidates;
  std::unordered_map<int, SDL_Texture *> rank_frames;
  std::vector<PackedUiAsset> packed_assets;
  std::unordered_map<std::string, const PackedUiAsset *> packed_asset_by_name;
  if (load_surface_from_memory) {
    packed_assets = LoadPackedAvatarAssets(exe_path);
    for (const PackedUiAsset &asset : packed_assets) {
      packed_asset_by_name.emplace(asset.name, &asset);
      packed_asset_by_name.emplace(std::filesystem::path(asset.name).filename().string(), &asset);
    }
  }
  const bool has_packed_assets = !packed_assets.empty();
  const bool has_ui_root = !ui_root.empty() && std::filesystem::exists(ui_root);
  if (has_packed_assets) {
    for (const PackedUiAsset &asset : packed_assets) {
      AvatarCandidate candidate;
      const std::wstring asset_name_w = Utf8ToWide(std::filesystem::path(asset.name).filename().string());
      if (ParseAvatarCandidateName(asset_name_w, candidate)) {
        candidate.path = std::filesystem::path(asset.name);
        candidates.push_back(std::move(candidate));
        continue;
      }

      const int rank_order = ParseRankFrameOrderName(std::filesystem::path(asset.name).filename().string());
      if (rank_order < 0 || rank_order > 3 || rank_frames.count(rank_order) > 0) continue;
      SDL_Surface *surface = load_surface_from_memory(asset.payload.data(), asset.payload.size());
      if (!surface) continue;
      SDL_Texture *rank_frame = CreateTextureFromSurface(renderer, surface);
      SDL_FreeSurface(surface);
      if (!rank_frame) continue;
      int width = 0;
      int height = 0;
      SDL_QueryTexture(rank_frame, nullptr, nullptr, &width, &height);
      if (remember_texture_size) remember_texture_size(rank_frame, width, height);
      rank_frames.emplace(rank_order, rank_frame);
    }
  } else if (has_ui_root) {
    for (const auto &entry : std::filesystem::directory_iterator(ui_root)) {
      if (!entry.is_regular_file()) continue;
      AvatarCandidate candidate;
      if (ParseAvatarCandidate(entry.path(), candidate)) candidates.push_back(std::move(candidate));
    }

    for (const auto &entry : std::filesystem::directory_iterator(ui_root)) {
      if (!entry.is_regular_file()) continue;
      const int rank_order = ParseRankFrameOrder(entry.path());
      if (rank_order < 0 || rank_order > 3 || rank_frames.count(rank_order) > 0) continue;

      SDL_Texture *rank_frame = LoadTextureFromFile(renderer, entry.path().string());
      if (!rank_frame) continue;
      int width = 0;
      int height = 0;
      SDL_QueryTexture(rank_frame, nullptr, nullptr, &width, &height);
      if (remember_texture_size) remember_texture_size(rank_frame, width, height);
      rank_frames.emplace(rank_order, rank_frame);
    }
  }

  if (candidates.empty()) return;

  std::vector<AvatarCandidate> ranked_candidates;
  std::vector<AvatarCandidate> max_candidates;
  ranked_candidates.reserve(candidates.size());
  max_candidates.reserve(candidates.size());
  for (AvatarCandidate &candidate : candidates) {
    if (candidate.contribution_is_max) max_candidates.push_back(std::move(candidate));
    else ranked_candidates.push_back(std::move(candidate));
  }

  auto sort_by_rank = [](std::vector<AvatarCandidate> &items) {
    std::sort(items.begin(), items.end(), [](const AvatarCandidate &lhs, const AvatarCandidate &rhs) {
      if (lhs.contribution != rhs.contribution) return lhs.contribution > rhs.contribution;
      return lhs.avatar_index < rhs.avatar_index;
    });
  };
  sort_by_rank(ranked_candidates);
  sort_by_rank(max_candidates);

  candidates.clear();
  const size_t insert_pos = std::min<size_t>(3, ranked_candidates.size());
  candidates.reserve(ranked_candidates.size() + max_candidates.size());
  candidates.insert(candidates.end(), ranked_candidates.begin(), ranked_candidates.begin() + insert_pos);
  candidates.insert(candidates.end(), max_candidates.begin(), max_candidates.end());
  candidates.insert(candidates.end(), ranked_candidates.begin() + insert_pos, ranked_candidates.end());
  const bool has_max_candidates = !max_candidates.empty();

  for (size_t i = 0; i < candidates.size(); ++i) {
    const AvatarCandidate &candidate = candidates[i];
    SDL_Surface *surface = nullptr;
    if (has_packed_assets) {
      auto it = packed_asset_by_name.find(candidate.path.filename().string());
      if (it != packed_asset_by_name.end()) {
        const PackedUiAsset *asset = it->second;
        surface = load_surface_from_memory(asset->payload.data(), asset->payload.size());
      }
    } else if (has_ui_root) {
      surface = LoadSurfaceFromFile(candidate.path.string());
    }
    if (!surface) continue;
    SDL_Texture *texture = CreateNormalizedCoverTexture(renderer, surface, 160, 160, 1.0f);
    SDL_FreeSurface(surface);
    if (!texture) continue;
    int width = 0;
    int height = 0;
    SDL_QueryTexture(texture, nullptr, nullptr, &width, &height);
    if (remember_texture_size) remember_texture_size(texture, width, height);
    SDL_Texture *rank_frame = nullptr;
    if (candidate.contribution_is_max) {
      auto max_frame_it = rank_frames.find(0);
      if (max_frame_it != rank_frames.end()) rank_frame = max_frame_it->second;
    } else {
      const int rank_order = static_cast<int>(i) + 1;
      auto rank_it = rank_frames.find(rank_order);
      if (rank_order <= 3 && rank_it != rank_frames.end()) {
        rank_frame = rank_it->second;
        rank_frames.erase(rank_it);
      }
    }
    entries.push_back(ContributorAvatarEntry{
        texture,
        rank_frame,
        candidate.label,
        LocalizeContributionLabel(language_index, candidate.label),
    });
  }

  if (has_max_candidates) {
    auto max_frame_it = rank_frames.find(0);
    if (max_frame_it != rank_frames.end()) {
      rank_frames.erase(max_frame_it);
    }
  }

  for (const auto &rank_pair : rank_frames) {
    SDL_Texture *rank_frame = rank_pair.second;
    if (!rank_frame) continue;
    if (before_destroy) before_destroy(rank_frame);
    SDL_DestroyTexture(rank_frame);
  }
}

void SyncContributorAvatarState(ContributorAvatarState &state, size_t entry_count) {
  if (entry_count == 0) {
    state.grid_active = false;
    state.focus_index = 0;
    state.scroll_row = 0;
    state.marquee_focus_index = -1;
    state.marquee_wait = 0.0f;
    state.marquee_offset = 0.0f;
    state.marquee_last_tick_ms = 0;
    return;
  }
  state.focus_index = std::clamp(state.focus_index, 0, static_cast<int>(entry_count) - 1);
  EnsureFocusedVisible(state, entry_count);
}

bool HandleContributorAvatarInput(const InputManager &input, float dt, ContributorAvatarState &state, size_t entry_count,
                                  const std::function<void(int)> &on_confirm_selection) {
  if (entry_count == 0) {
    state.grid_active = false;
    state.focus_index = 0;
    state.scroll_row = 0;
    state.marquee_focus_index = -1;
    state.marquee_wait = 0.0f;
    state.marquee_offset = 0.0f;
    state.marquee_last_tick_ms = 0;
    return false;
  }

  SyncContributorAvatarState(state, entry_count);
  if (state.focus_index != state.marquee_focus_index) {
    state.marquee_focus_index = state.focus_index;
    state.marquee_wait = kAvatarNameMarqueePauseSec;
    state.marquee_offset = 0.0f;
    state.marquee_last_tick_ms = SDL_GetTicks();
  } else {
    const float effective_dt = ComputeEffectiveMarqueeDt(dt, state);
    if (state.marquee_wait > 0.0f) {
      state.marquee_wait = std::max(0.0f, state.marquee_wait - effective_dt);
    }
    if (state.marquee_wait <= 0.0f) {
      state.marquee_offset += kAvatarNameMarqueeSpeedPx * effective_dt;
      if (state.marquee_offset > 8192.0f) {
        state.marquee_offset = std::fmod(state.marquee_offset, 8192.0f);
      }
    }
  }
  if (!state.grid_active) {
    if (input.IsJustPressed(Button::A) || input.IsJustPressed(Button::Right)) {
      state.grid_active = true;
      state.marquee_focus_index = state.focus_index;
      state.marquee_wait = kAvatarNameMarqueePauseSec;
      state.marquee_offset = 0.0f;
      state.marquee_last_tick_ms = SDL_GetTicks();
      return true;
    }
    return false;
  }

  const int max_index = static_cast<int>(entry_count) - 1;
  const int max_row = max_index / 3;
  int row = state.focus_index / 3;
  int col = state.focus_index % 3;

  if (input.IsJustPressed(Button::B)) {
    state.grid_active = false;
    state.marquee_last_tick_ms = SDL_GetTicks();
    return true;
  }
  if (input.IsJustPressed(Button::A)) {
    if (on_confirm_selection) on_confirm_selection(state.focus_index);
    return true;
  }
  if (input.IsJustPressed(Button::Left) || input.IsRepeated(Button::Left)) {
    if (col > 0) --col;
  } else if (input.IsJustPressed(Button::Right) || input.IsRepeated(Button::Right)) {
    if (col < 2 && state.focus_index + 1 <= max_index) ++col;
  } else if (input.IsJustPressed(Button::Up) || input.IsRepeated(Button::Up)) {
    if (row > 0) --row;
  } else if (input.IsJustPressed(Button::Down) || input.IsRepeated(Button::Down)) {
    if (row < max_row) ++row;
  } else {
    return false;
  }

  state.focus_index = std::min(max_index, row * 3 + col);
  EnsureFocusedVisible(state, entry_count);
  if (state.focus_index != state.marquee_focus_index) {
    state.marquee_focus_index = state.focus_index;
    state.marquee_wait = kAvatarNameMarqueePauseSec;
    state.marquee_offset = 0.0f;
    state.marquee_last_tick_ms = SDL_GetTicks();
  }
  return true;
}

void DrawContributorAvatarPreview(const ContributorAvatarRenderDeps &deps) {
  if (!deps.renderer || deps.preview_rect.w <= 0 || deps.preview_rect.h <= 0 || deps.entries.empty() || !deps.draw_rect) {
    return;
  }

  const int safe_top = deps.preview_rect.y + 35;
  const int safe_bottom = deps.preview_rect.y + deps.preview_rect.h - 35;
  const int safe_left = deps.preview_rect.x + 28;
  const int safe_right = deps.preview_rect.x + deps.preview_rect.w - 28;
  const int safe_w = std::max(0, safe_right - safe_left);
  const int safe_h = std::max(0, safe_bottom - safe_top);
  if (safe_w <= 0 || safe_h <= 0) return;

  const int col_gap = 22;
  const int row_gap = 16;
  const int name_gap = 8;
  const int name_h = 20;
  const int tile_w = std::max(60, (safe_w - col_gap * 2) / 3);
  const int row_pitch =
      std::clamp(static_cast<int>(std::floor((safe_h - row_gap * 1.5f) / 2.5f)), 88, 154);
  const int image_size = std::max(48, std::min(tile_w, row_pitch - name_gap - name_h - row_gap));
  const int x_base = safe_left + std::max(0, (safe_w - (tile_w * 3 + col_gap * 2)) / 2);
  const int top_inner_gap = std::max(12, safe_h / 24);
  const int y_base = safe_top + top_inner_gap;
  const int scroll_y = deps.state.scroll_row * row_pitch;

  SDL_Rect viewport{safe_left, safe_top, safe_w, safe_h};
  SDL_RenderSetClipRect(deps.renderer, &viewport);

  for (size_t i = 0; i < deps.entries.size(); ++i) {
    const int row = static_cast<int>(i) / 3;
    const int col = static_cast<int>(i) % 3;
    const int tile_x = x_base + col * (tile_w + col_gap);
    const int tile_y = y_base + row * row_pitch - scroll_y;
    const bool focused = static_cast<int>(i) == deps.state.focus_index;
    const bool selected = deps.state.grid_active && focused;
    const float scale = selected ? 1.08f : 1.0f;
    const int draw_image_size = static_cast<int>(std::lround(image_size * scale));
    const int image_x = tile_x + (tile_w - draw_image_size) / 2;
    const int image_y = tile_y;
    const int label_w = image_size;
    const int label_x = tile_x + (tile_w - label_w) / 2;
    const int label_y = tile_y + draw_image_size + name_gap;

    if (label_y + name_h < safe_top || image_y > safe_bottom) continue;

    SDL_Texture *avatar_tex = deps.entries[i].texture;
    if (avatar_tex) {
      SDL_Rect dst{image_x, image_y, draw_image_size, draw_image_size};
      SDL_RenderCopy(deps.renderer, avatar_tex, nullptr, &dst);
      if (deps.entries[i].rank_frame) {
        SDL_RenderCopy(deps.renderer, deps.entries[i].rank_frame, nullptr, &dst);
      }
      if (selected) {
        deps.draw_rect(image_x - 3, image_y - 3, draw_image_size + 6, draw_image_size + 6,
                       SDL_Color{120, 205, 255, 255}, false);
      }
    }

#ifdef HAVE_SDL2_TTF
    const std::string display_label =
        deps.entries[i].raw_label.empty() ? deps.entries[i].label
                                          : LocalizeContributionLabel(deps.language_index, deps.entries[i].raw_label);
    if (deps.get_text_texture && !display_label.empty()) {
      TextCacheEntry *label_tex = deps.get_text_texture(display_label, SDL_Color{235, 241, 248, 255});
      if (label_tex && label_tex->texture) {
        SDL_Rect label_clip{label_x, label_y, label_w, name_h};
        SDL_RenderSetClipRect(deps.renderer, &label_clip);
        const int draw_y = label_y + std::max(0, (name_h - label_tex->h) / 2);
        if (label_tex->w <= label_w) {
          const int centered_x = label_x + std::max(0, (label_w - label_tex->w) / 2);
          SDL_Rect td{centered_x, draw_y, label_tex->w, label_tex->h};
          SDL_RenderCopy(deps.renderer, label_tex->texture, nullptr, &td);
        } else if (focused) {
          const float span = static_cast<float>(label_tex->w) + kAvatarNameMarqueeGapPx;
          const float xoff = span > 0.0f ? std::fmod(deps.state.marquee_offset, span) : 0.0f;
          SDL_Rect td1{label_x - static_cast<int>(std::round(xoff)), draw_y, label_tex->w, label_tex->h};
          SDL_Rect td2{td1.x + static_cast<int>(std::round(span)), draw_y, label_tex->w, label_tex->h};
          SDL_RenderCopy(deps.renderer, label_tex->texture, nullptr, &td1);
          SDL_RenderCopy(deps.renderer, label_tex->texture, nullptr, &td2);
        } else {
          SDL_Rect td{label_x, draw_y, label_tex->w, label_tex->h};
          SDL_RenderCopy(deps.renderer, label_tex->texture, nullptr, &td);
        }
        SDL_RenderSetClipRect(deps.renderer, &viewport);
      }
    }
#endif
  }

  SDL_RenderSetClipRect(deps.renderer, nullptr);
}
