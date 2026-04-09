#include "boot_runtime.h"
#include "path_adapter.h"

#include <algorithm>
#include <cmath>
#include <fstream>

namespace {
constexpr float kUpdateReplayDurationSec = 1.8f;

bool StartNextBootCountRoot(BootRuntimeState &state, const std::vector<std::string> &books_roots) {
  const auto opts = std::filesystem::directory_options::skip_permission_denied;
  while (state.count_root_index < books_roots.size()) {
    const std::filesystem::path root_path(books_roots[state.count_root_index++]);
    std::error_code ec;
    if (!std::filesystem::exists(root_path, ec) || !std::filesystem::is_directory(root_path, ec)) continue;
    state.count_it = std::filesystem::recursive_directory_iterator(root_path, opts, ec);
    if (ec) continue;
    state.count_end = std::filesystem::recursive_directory_iterator();
    state.count_iterator_active = true;
    return true;
  }
  state.count_iterator_active = false;
  return false;
}

float BootProgressRatio(const BootRuntimeState &state) {
  switch (state.phase) {
  case BootPhase::UpdateReplay:
    return std::clamp(state.timer / kUpdateReplayDurationSec, 0.0f, 1.0f);
  case BootPhase::CountBooks: {
    const float pulse = std::fmod(state.timer * 0.85f, 1.0f);
    return 0.05f + pulse * 0.15f;
  }
  case BootPhase::ScanBooks:
    return state.total_books == 0
               ? 0.55f
               : (0.20f + 0.35f * (static_cast<float>(state.scan_index) /
                                   static_cast<float>(std::max<size_t>(1, state.total_books))));
  case BootPhase::GenerateCovers:
    return state.cover_generate_queue.empty()
               ? 1.0f
               : (0.55f + 0.45f * (static_cast<float>(state.cover_generate_index) /
                                   static_cast<float>(std::max<size_t>(1, state.cover_generate_queue.size()))));
  case BootPhase::Finalize:
  case BootPhase::Done:
    return 1.0f;
  }
  return 0.0f;
}

std::string MakeBootScanText(size_t current, size_t total) {
  return std::string(u8"资源加载中...（") + std::to_string(current) + "/" + std::to_string(total) + u8"）";
}

std::string MakeBootCoverText(size_t current, size_t total) {
  return std::string(u8"封面缓存生成中...（") + std::to_string(current) + "/" + std::to_string(total) + u8"）";
}
std::string MakeUpdateReplayText(float ratio, bool success, const std::string &version) {
  const std::string version_suffix = version.empty() ? std::string() : (" " + version);
  if (!success) return std::string(u8"\u66f4\u65b0\u5b89\u88c5\u672a\u5b8c\u6210\uff0c\u6b63\u5728\u542f\u52a8\u5f53\u524d\u7248\u672c");
  if (ratio < 0.30f) return std::string(u8"\u6b63\u5728\u5b89\u88c5\u66f4\u65b0\u5305") + version_suffix;
  if (ratio < 0.60f) return std::string(u8"\u6b63\u5728\u89e3\u538b\u66f4\u65b0\u8d44\u6e90") + version_suffix;
  if (ratio < 0.90f) return std::string(u8"\u6b63\u5728\u66ff\u6362\u8fd0\u884c\u65f6\u6587\u4ef6") + version_suffix;
  return std::string(u8"\u66f4\u65b0\u5b89\u88c5\u5b8c\u6210\uff0c\u5373\u5c06\u8fdb\u5165\u4e66\u67b6");
}

bool LoadUpdateReplayStatus(const std::filesystem::path &status_path, bool &out_success, std::string &out_version) {
  std::ifstream in(status_path);
  if (!in) return false;
  std::string line;
  bool saw_result = false;
  while (std::getline(in, line)) {
    const size_t eq = line.find('=');
    if (eq == std::string::npos) continue;
    const std::string key = line.substr(0, eq);
    const std::string value = line.substr(eq + 1);
    if (key == "result") {
      out_success = (value == "success");
      saw_result = true;
    } else if (key == "version") {
      out_version = value;
    }
  }
  return saw_result;
}
}

void InitializeBootRuntimeReplay(BootRuntimeState &state, const std::filesystem::path &status_path) {
  bool success = false;
  std::string version;
  if (!LoadUpdateReplayStatus(status_path, success, version)) return;
  state.phase = BootPhase::UpdateReplay;
  state.timer = 0.0f;
  state.update_replay_success = success;
  state.update_replay_version = std::move(version);
  state.update_status_path = status_path;
  state.status_text = MakeUpdateReplayText(0.0f, state.update_replay_success, state.update_replay_version);
}

void TickBootRuntime(BootRuntimeState &state, float dt, const BootRuntimeTickDeps &deps) {
  state.timer += dt;
  if (state.phase == BootPhase::UpdateReplay) {
    const float replay_ratio = std::clamp(state.timer / kUpdateReplayDurationSec, 0.0f, 1.0f);
    state.status_text = MakeUpdateReplayText(replay_ratio, state.update_replay_success, state.update_replay_version);
    if (replay_ratio >= 1.0f) {
      if (!state.update_status_path.empty()) {
        std::error_code remove_ec;
        std::filesystem::remove(state.update_status_path, remove_ec);
      }
      state.timer = 0.0f;
      state.phase = BootPhase::CountBooks;
      state.status_text = MakeBootScanText(0, 0);
    }
  } else if (state.phase == BootPhase::CountBooks) {
    if (!state.count_iterator_active) {
      StartNextBootCountRoot(state, deps.books_roots);
    }
    size_t processed = 0;
    while (processed < deps.count_batch_entries) {
      if (!state.count_iterator_active) {
        if (!StartNextBootCountRoot(state, deps.books_roots)) break;
      }
      if (state.count_it == state.count_end) {
        state.count_iterator_active = false;
        continue;
      }
      std::error_code ec;
      const auto entry = *state.count_it;
      state.count_it.increment(ec);
      ++processed;
      if (ec || !entry.is_regular_file(ec)) continue;
      const std::string readable_path = path_adapter::ResolveReadableFilePath(entry);
      const std::string ext = deps.get_lower_ext ? deps.get_lower_ext(readable_path) : std::string{};
      if (ext == ".pdf" || ext == ".txt" || ext == ".epub") {
        BookItem item;
        item.name = entry.path().filename().string();
        item.path = readable_path;
        item.real_path = readable_path;
        item.native_fs_path = entry.path();
        item.is_dir = false;
        state.scanned_books.push_back(std::move(item));
      }
    }
    state.status_text = MakeBootScanText(0, 0);
    if (!state.count_iterator_active && state.count_root_index >= deps.books_roots.size()) {
      std::sort(state.scanned_books.begin(), state.scanned_books.end(),
                [](const BookItem &a, const BookItem &b) {
                  if (a.is_dir != b.is_dir) return a.is_dir > b.is_dir;
                  if (a.path != b.path) return a.path < b.path;
                  return a.name < b.name;
                });
      state.total_books = state.scanned_books.size();
      state.scan_index = 0;
      state.cover_generate_queue.clear();
      state.phase = BootPhase::ScanBooks;
      state.status_text = MakeBootScanText(0, state.total_books);
    }
  } else if (state.phase == BootPhase::ScanBooks) {
    size_t processed = 0;
    while (processed < deps.scan_batch_entries && state.scan_index < state.scanned_books.size()) {
      const std::string &book_path = state.scanned_books[state.scan_index].real_path.empty()
                                         ? state.scanned_books[state.scan_index].path
                                         : state.scanned_books[state.scan_index].real_path;
      const std::string ext = deps.get_lower_ext ? deps.get_lower_ext(book_path) : std::string{};
      if (ext == ".pdf" || ext == ".epub") {
        const BookItem &item = state.scanned_books[state.scan_index];
        const bool manual = deps.has_manual_cover_exact_or_fuzzy && deps.has_manual_cover_exact_or_fuzzy(item);
        const bool cached = deps.has_cached_doc_cover_on_disk && deps.has_cached_doc_cover_on_disk(book_path);
        const bool can_render = deps.doc_cover_backend_available && deps.doc_cover_backend_available(book_path);
        if (!manual && can_render && !cached) {
          state.cover_generate_queue.push_back(book_path);
        }
      }
      ++state.scan_index;
      ++processed;
    }
    state.status_text = MakeBootScanText(state.scan_index, state.total_books);
    if (state.scan_index >= state.scanned_books.size()) {
      state.cover_generate_index = 0;
      state.phase = BootPhase::GenerateCovers;
      state.status_text = MakeBootCoverText(0, state.cover_generate_queue.size());
      if (state.cover_generate_queue.empty()) {
        state.phase = BootPhase::Finalize;
      }
    }
  } else if (state.phase == BootPhase::GenerateCovers) {
    size_t processed = 0;
    while (processed < deps.cover_generate_batch_entries &&
           state.cover_generate_index < state.cover_generate_queue.size()) {
      const std::string &doc_path = state.cover_generate_queue[state.cover_generate_index];
      if (deps.create_doc_first_page_cover_texture) {
        if (SDL_Texture *generated = deps.create_doc_first_page_cover_texture(doc_path)) {
          if (deps.destroy_generated_cover_texture) deps.destroy_generated_cover_texture(generated);
        }
      }
      ++state.cover_generate_index;
      ++processed;
    }
    state.status_text = MakeBootCoverText(state.cover_generate_index, state.cover_generate_queue.size());
    if (state.cover_generate_index >= state.cover_generate_queue.size()) {
      state.phase = BootPhase::Finalize;
    }
  }

  if (state.phase == BootPhase::Finalize) {
    if (deps.on_finish) deps.on_finish(state.total_books, state.cover_generate_queue.size());
    state.phase = BootPhase::Done;
  }
}

void DrawBootRuntime(const BootRuntimeRenderDeps &deps) {
  deps.draw_rect(0, 0, deps.screen_w, deps.screen_h, SDL_Color{20, 20, 24, 255}, true);
  const int bar_x = 40;
  const int bar_y = deps.screen_h / 2;
  const int bar_w = deps.screen_w - 80;
  const float progress = std::clamp(BootProgressRatio(deps.state), 0.0f, 1.0f);
  const int fill_w = static_cast<int>(std::round(progress * bar_w));
  deps.draw_rect(bar_x, bar_y, bar_w, 16, SDL_Color{48, 52, 60, 255}, true);
  deps.draw_rect(bar_x, bar_y, fill_w, 16, SDL_Color{210, 210, 210, 255}, true);
  deps.draw_rect(bar_x, bar_y, bar_w, 16, SDL_Color{255, 255, 255, 220}, false);
#ifdef HAVE_SDL2_TTF
  const SDL_Color boot_text_color{232, 236, 244, 255};
  if (!deps.state.status_text.empty() && deps.get_text_texture) {
    if (TextCacheEntry *te = deps.get_text_texture(deps.state.status_text, boot_text_color); te && te->texture) {
      SDL_Rect td{std::max(0, (deps.screen_w - te->w) / 2), bar_y + 28, te->w, te->h};
      SDL_RenderCopy(deps.renderer, te->texture, nullptr, &td);
    }
  }
#endif
}
