#pragma once

#include "input_manager.h"
#include "ui_text_cache.h"

#include <SDL.h>

#include <filesystem>
#include <functional>
#include <string>
#include <vector>

struct ContributorAvatarEntry {
  SDL_Texture *texture = nullptr;
  SDL_Texture *rank_frame = nullptr;
  std::string raw_label;
  std::string label;
};

struct ContributorAvatarState {
  bool grid_active = false;
  int focus_index = 0;
  int scroll_row = 0;
  int marquee_focus_index = -1;
  float marquee_wait = 0.0f;
  float marquee_offset = 0.0f;
  uint32_t marquee_last_tick_ms = 0;
};

struct ContributorAvatarRenderDeps {
  SDL_Renderer *renderer = nullptr;
  SDL_Rect preview_rect{};
  const std::vector<ContributorAvatarEntry> &entries;
  const ContributorAvatarState &state;
  int language_index = 0;
  std::function<void(int, int, int, int, SDL_Color, bool)> draw_rect;
  std::function<TextCacheEntry *(const std::string &, SDL_Color)> get_text_texture;
};

void DestroyContributorAvatarEntries(std::vector<ContributorAvatarEntry> &entries,
                                     const std::function<void(SDL_Texture *)> &before_destroy = {});
void LoadContributorAvatarEntries(std::vector<ContributorAvatarEntry> &entries, const std::filesystem::path &ui_root,
                                  const std::filesystem::path &exe_path, SDL_Renderer *renderer, int language_index,
                                  const std::function<SDL_Surface *(const void *, size_t)> &load_surface_from_memory,
                                  const std::function<void(SDL_Texture *, int, int)> &remember_texture_size,
                                  const std::function<void(SDL_Texture *)> &before_destroy = {});
void SyncContributorAvatarState(ContributorAvatarState &state, size_t entry_count);
bool HandleContributorAvatarInput(const InputManager &input, float dt, ContributorAvatarState &state, size_t entry_count,
                                  const std::function<void(int)> &on_confirm_selection = {});
void DrawContributorAvatarPreview(const ContributorAvatarRenderDeps &deps);
