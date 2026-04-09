#pragma once

#include "input_manager.h"
#include "storage_paths.h"
#include "ui_text_cache.h"

#include <SDL.h>

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <thread>

enum class VersionUpdateStatus {
  Idle,
  NoNetwork,
  Downloading,
  Downloaded,
  UpToDate,
  DownloadFailed,
};

struct VersionUpdateState {
  bool panel_active = false;
  int selected_button = 0;
  std::string current_version = "v0.0.0-dev";
  std::string latest_version;
  VersionUpdateStatus status = VersionUpdateStatus::Idle;
  int download_progress_pct = 0;
  bool download_in_progress = false;
  bool has_pending_package = false;
  uint64_t expected_download_bytes = 0;
  std::filesystem::path download_root;
  std::filesystem::path pending_marker_path;
  std::filesystem::path pending_package_path;
  std::filesystem::path temp_package_path;
  std::string active_download_filename;
  std::string active_download_url;
  std::thread download_thread;
  std::atomic<bool> download_thread_done = false;
  std::atomic<bool> download_thread_success = false;
};

struct VersionUpdateCallbacks {
  std::function<void(VersionUpdateState &)> start_check_and_update;
};

struct VersionUpdateRenderDeps {
  SDL_Renderer *renderer = nullptr;
  SDL_Rect preview_rect{};
  const VersionUpdateState &state;
  bool light_theme = false;
  std::function<void(int, int, int, int, SDL_Color, bool)> draw_rect;
  std::function<TextCacheEntry *(const std::string &, SDL_Color)> get_text_texture;
  std::function<TextCacheEntry *(const std::string &, SDL_Color)> get_emphasis_text_texture;
};

bool HandleVersionUpdateInput(const InputManager &input, VersionUpdateState &state,
                              const VersionUpdateCallbacks &callbacks);
bool BeginVersionUpdateDownload(VersionUpdateState &state);
void InitializeVersionUpdateState(VersionUpdateState &state);
void TickVersionUpdateState(VersionUpdateState &state, float dt);
void ShutdownVersionUpdateState(VersionUpdateState &state);
void DrawVersionUpdatePreview(const VersionUpdateRenderDeps &deps);
