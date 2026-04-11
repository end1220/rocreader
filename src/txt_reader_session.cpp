#include "txt_reader_session.h"

#include "reader_core.h"

#include <SDL.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>

namespace {
constexpr size_t kTxtInitialLineReserve = 1024;

int ClampIntLocal(int value, int lo, int hi) {
  return std::max(lo, std::min(hi, value));
}

int ScrollForPercent(int pct, int max_scroll) {
  if (max_scroll <= 0) return 0;
  if (pct <= 0) return 0;
  if (pct >= 100) return max_scroll;
  return static_cast<int>((static_cast<int64_t>(pct) * max_scroll + 99) / 100);
}

size_t TopVisibleWrappedLineIndex(const TxtReaderState &state) {
  const int line_h = std::max(1, state.line_h);
  if (state.lines.empty()) return 0;
  const int index = std::clamp(state.scroll_px / line_h, 0, static_cast<int>(state.lines.size()) - 1);
  return static_cast<size_t>(index);
}

size_t TopVisibleSourceOffset(const TxtReaderState &state) {
  if (state.line_source_offsets.empty()) return 0;
  const size_t top_line = std::min(TopVisibleWrappedLineIndex(state), state.line_source_offsets.size() - 1);
  return state.line_source_offsets[top_line];
}

int ScrollForSourceOffset(const TxtReaderState &state, size_t source_offset) {
  if (state.line_source_offsets.empty()) return 0;
  auto upper = std::upper_bound(state.line_source_offsets.begin(), state.line_source_offsets.end(), source_offset);
  size_t line_index = 0;
  if (upper == state.line_source_offsets.begin()) {
    line_index = 0;
  } else {
    line_index = static_cast<size_t>(std::distance(state.line_source_offsets.begin(), upper - 1));
  }
  return static_cast<int>(line_index) * std::max(1, state.line_h);
}

void SyncScrollToRestoreAnchor(TxtReaderState &state) {
  if (state.restore_source_offset > 0 && !state.line_source_offsets.empty()) {
    state.target_scroll_px = ScrollForSourceOffset(state, state.restore_source_offset);
  }
  const int max_scroll = std::max(0, state.content_h - state.viewport_h);
  state.scroll_px = std::clamp(state.target_scroll_px, 0, max_scroll);
}
}

void FinalizeTextReaderLoading(TxtReaderState &state, const std::string *cache_key, TxtReaderSessionDeps &deps) {
  if ((state.truncated || state.limit_hit) && !state.truncation_notice_added) {
    const size_t notice_source_offset = std::max(state.parse_pos, TopVisibleSourceOffset(state));
    state.lines.push_back("");
    state.line_source_offsets.push_back(notice_source_offset);
    state.lines.push_back("[TXT preview truncated]");
    state.line_source_offsets.push_back(notice_source_offset);
    state.truncation_notice_added = true;
  }
  if (state.lines.empty()) {
    state.lines.emplace_back("");
    state.line_source_offsets.push_back(0);
  }
  state.content_h = static_cast<int>(state.lines.size()) * state.line_h;
  SyncScrollToRestoreAnchor(state);
  state.resume_cache_dirty = true;
  if (cache_key && !cache_key->empty() && !state.loading) {
    TxtLayoutCacheEntry entry;
    entry.lines = state.lines;
    entry.line_source_offsets = state.line_source_offsets;
    entry.viewport_w = state.viewport_w;
    entry.viewport_h = state.viewport_h;
    entry.line_h = state.line_h;
    entry.content_h = state.content_h;
    entry.truncated = state.truncated;
    entry.limit_hit = state.limit_hit;
    entry.last_use = SDL_GetTicks();
    deps.layout_cache[*cache_key] = std::move(entry);
    deps.save_layout_cache_to_disk(*cache_key, deps.ui.current_book, deps.layout_cache[*cache_key]);
    deps.prune_layout_cache();
  }
}

void ProcessTextLayoutChunk(TxtReaderState &state, uint32_t budget_ms, size_t byte_budget,
                            const std::string *cache_key, TxtReaderSessionDeps &deps) {
  if (!state.open || !state.loading) return;
  const uint32_t started = SDL_GetTicks();
  size_t consumed = 0;
  const size_t prev_parse_pos = state.parse_pos;
  const size_t prev_line_count = state.lines.size();
  if (state.pending_line.empty()) state.pending_line_source_offset = state.parse_pos;
  while (state.parse_pos < state.pending_raw.size() && !state.limit_hit) {
    const char ch = state.pending_raw[state.parse_pos++];
    ++consumed;
    if (ch == '\n' || ch == '\r') {
      if (!deps.append_wrapped_text_line(state, state.pending_line, state.pending_line_source_offset)) {
        state.limit_hit = true;
        break;
      }
      state.pending_line.clear();
      if (ch == '\r' && state.parse_pos < state.pending_raw.size() && state.pending_raw[state.parse_pos] == '\n') {
        ++state.parse_pos;
        ++consumed;
      }
      state.pending_line_source_offset = state.parse_pos;
    } else {
      if (state.pending_line.empty()) state.pending_line_source_offset = state.parse_pos - 1;
      state.pending_line.push_back(ch);
    }
    if (consumed >= byte_budget) break;
    if (budget_ms > 0 && SDL_GetTicks() - started >= budget_ms) break;
  }
  if (!state.limit_hit && state.parse_pos >= state.pending_raw.size()) {
    if (!deps.append_wrapped_text_line(state, state.pending_line, state.pending_line_source_offset)) {
      state.limit_hit = true;
    }
    state.pending_line.clear();
  }
  SyncScrollToRestoreAnchor(state);
  if (state.parse_pos != prev_parse_pos || state.lines.size() != prev_line_count) {
    state.resume_cache_dirty = true;
  }
  if (state.limit_hit || state.parse_pos >= state.pending_raw.size()) {
    state.loading = false;
    state.pending_raw.clear();
    state.pending_line.clear();
    state.scroll_px = std::clamp(state.target_scroll_px, 0, std::max(0, state.content_h - state.viewport_h));
    FinalizeTextReaderLoading(state, cache_key, deps);
  }
}

void WarmTextReaderToTarget(TxtReaderState &state, const std::string *cache_key, TxtReaderSessionDeps &deps) {
  if (!state.loading) return;
  if (state.restore_source_offset > 0) {
    while (state.loading && state.parse_pos < state.restore_source_offset) {
      ProcessTextLayoutChunk(state, 0, 262144, cache_key, deps);
    }
    SyncScrollToRestoreAnchor(state);
    if (!state.loading) return;
  }
  const int desired_bottom = state.target_scroll_px + state.viewport_h;
  if (desired_bottom > state.viewport_h) {
    while (state.loading && state.content_h < desired_bottom) {
      ProcessTextLayoutChunk(state, 0, 262144, cache_key, deps);
    }
  }
  SyncScrollToRestoreAnchor(state);
}

void TickTextBookSession(const std::string &book_path, TxtReaderSessionDeps &deps,
                         uint32_t budget_ms, size_t byte_budget) {
  if (deps.ui.mode != ReaderMode::Txt || !deps.ui.txt_reader.open || !deps.ui.txt_reader.loading) return;
  ProcessTextLayoutChunk(deps.ui.txt_reader, budget_ms, byte_budget, &deps.ui.txt_reader.cache_key, deps);
  deps.clamp_text_scroll();
  PersistCurrentTxtResumeSnapshot(book_path, false, deps);
}

bool OpenTextBookSession(const std::string &path, TxtReaderSessionDeps &deps) {
  deps.open_ui_font();
  if (!deps.has_reader_font()) {
    std::cerr << "[reader] txt reader failed: ui font unavailable.\n";
    return false;
  }

  const SDL_Rect text_bounds = deps.get_text_viewport_bounds();
  int font_h = deps.reader_font_height();
  if (font_h <= 0) font_h = 24;
  const int line_h = font_h + deps.txt_line_spacing;
  std::error_code meta_ec;
  const uintmax_t cache_file_size = std::filesystem::file_size(std::filesystem::path(path), meta_ec);
  const auto cache_mtime_raw = std::filesystem::last_write_time(std::filesystem::path(path), meta_ec);
  const long long cache_file_mtime = meta_ec ? 0LL : static_cast<long long>(cache_mtime_raw.time_since_epoch().count());
  const std::string txt_cache_key =
      deps.make_layout_cache_key(path, text_bounds, line_h, meta_ec ? 0 : cache_file_size, cache_file_mtime);

  TxtReaderState next{};
  next.open = true;
  next.viewport_x = text_bounds.x;
  next.viewport_y = text_bounds.y;
  next.viewport_w = text_bounds.w;
  next.viewport_h = text_bounds.h;
  next.line_h = line_h;
  next.cache_key = txt_cache_key;
  const size_t preferred_source_offset = deps.ui.progress.scroll_x > 0
                                             ? static_cast<size_t>(std::max(0, deps.ui.progress.scroll_x))
                                             : 0;

  auto txt_cache_it = deps.layout_cache.find(next.cache_key);
  if (txt_cache_it != deps.layout_cache.end()) {
    txt_cache_it->second.last_use = SDL_GetTicks();
    next.lines = txt_cache_it->second.lines;
    next.line_source_offsets = txt_cache_it->second.line_source_offsets;
    next.content_h = txt_cache_it->second.content_h;
    next.truncated = txt_cache_it->second.truncated;
    next.limit_hit = txt_cache_it->second.limit_hit;
    next.truncation_notice_added = true;
    next.loading = false;
    next.restore_source_offset = preferred_source_offset;
    next.target_scroll_px = std::max(0, deps.ui.progress.scroll_y);
    SyncScrollToRestoreAnchor(next);
    deps.ui.txt_reader = std::move(next);
    deps.ui.mode = ReaderMode::Txt;
    deps.ui.progress_overlay_visible = false;
    deps.invalidate_all_render_cache();
    deps.clamp_text_scroll();
    return true;
  }

  TxtLayoutCacheEntry disk_cache_entry;
  if (deps.load_layout_cache_from_disk(next.cache_key, path, disk_cache_entry)) {
    disk_cache_entry.last_use = SDL_GetTicks();
    deps.layout_cache[next.cache_key] = disk_cache_entry;
    deps.prune_layout_cache();
    next.lines = disk_cache_entry.lines;
    next.line_source_offsets = disk_cache_entry.line_source_offsets;
    next.content_h = disk_cache_entry.content_h;
    next.truncated = disk_cache_entry.truncated;
    next.limit_hit = disk_cache_entry.limit_hit;
    next.truncation_notice_added = true;
    next.loading = false;
    next.restore_source_offset = preferred_source_offset;
    next.target_scroll_px = std::max(0, deps.ui.progress.scroll_y);
    SyncScrollToRestoreAnchor(next);
    deps.ui.txt_reader = std::move(next);
    deps.ui.mode = ReaderMode::Txt;
    deps.ui.progress_overlay_visible = false;
    deps.invalidate_all_render_cache();
    deps.clamp_text_scroll();
    return true;
  }

  TxtResumeCacheEntry resume_cache_entry;
  if (deps.load_resume_cache_from_disk(next.cache_key, path, resume_cache_entry)) {
    const int restored_scroll_px =
        std::max(std::max(0, deps.ui.progress.scroll_y), std::max(0, resume_cache_entry.scroll_px));
    next.lines = std::move(resume_cache_entry.lines);
    next.line_source_offsets = std::move(resume_cache_entry.line_source_offsets);
    next.pending_raw = std::move(resume_cache_entry.pending_raw);
    next.pending_line = std::move(resume_cache_entry.pending_line);
    next.pending_line_source_offset = resume_cache_entry.pending_line_source_offset;
    next.content_h = resume_cache_entry.content_h;
    next.parse_pos = resume_cache_entry.parse_pos;
    next.restore_source_offset = std::max(preferred_source_offset, resume_cache_entry.restore_source_offset);
    next.loading = resume_cache_entry.loading;
    next.truncated = resume_cache_entry.truncated;
    next.limit_hit = resume_cache_entry.limit_hit;
    next.truncation_notice_added = resume_cache_entry.truncation_notice_added;
    next.target_scroll_px = restored_scroll_px;
    SyncScrollToRestoreAnchor(next);
    next.last_resume_cache_save = SDL_GetTicks();
    next.resume_cache_dirty = false;
    deps.ui.txt_reader = std::move(next);
    SyncScrollToRestoreAnchor(deps.ui.txt_reader);
    deps.ui.mode = ReaderMode::Txt;
    deps.ui.progress_overlay_visible = false;
    deps.invalidate_all_render_cache();
    deps.clamp_text_scroll();
    return true;
  }

  std::ifstream in(std::filesystem::path(path), std::ios::binary);
  if (!in) {
    std::cerr << "[reader] txt open failed: " << path << "\n";
    return false;
  }
  std::string raw;
  bool truncated = false;
  try {
    std::error_code ec;
    const auto fsz = std::filesystem::file_size(std::filesystem::path(path), ec);
    if (!ec && fsz > 0) {
      const size_t cap = static_cast<size_t>(std::min<uintmax_t>(fsz, deps.txt_max_bytes));
      truncated = fsz > deps.txt_max_bytes;
      raw.resize(cap);
      in.read(raw.data(), static_cast<std::streamsize>(cap));
      raw.resize(static_cast<size_t>(in.gcount()));
    } else {
      std::ostringstream oss;
      oss << in.rdbuf();
      raw = oss.str();
      if (raw.size() > deps.txt_max_bytes) {
        raw.resize(deps.txt_max_bytes);
        truncated = true;
      }
    }
  } catch (...) {
    std::cerr << "[reader] txt read failed (exception): " << path << "\n";
    return false;
  }

  std::string decoded;
  if (deps.decode_text_bytes_to_utf8(raw, decoded)) {
    raw = std::move(decoded);
  }

  next.pending_raw = std::move(raw);
  next.pending_line.reserve(256);
  next.pending_line_source_offset = 0;
  next.parse_pos = 0;
  next.restore_source_offset = preferred_source_offset;
  next.loading = true;
  next.truncated = truncated;
  next.limit_hit = false;
  next.truncation_notice_added = false;
  next.lines.reserve(kTxtInitialLineReserve);
  next.target_scroll_px = std::max(0, deps.ui.progress.scroll_y);
  next.scroll_px = 0;
  next.last_resume_cache_save = 0;
  next.resume_cache_dirty = true;

  deps.ui.txt_reader = std::move(next);
  ProcessTextLayoutChunk(deps.ui.txt_reader, 8, 32768, &deps.ui.txt_reader.cache_key, deps);
  WarmTextReaderToTarget(deps.ui.txt_reader, &deps.ui.txt_reader.cache_key, deps);
  if (!deps.ui.txt_reader.loading) FinalizeTextReaderLoading(deps.ui.txt_reader, &deps.ui.txt_reader.cache_key, deps);
  SyncScrollToRestoreAnchor(deps.ui.txt_reader);
  deps.ui.mode = ReaderMode::Txt;
  deps.ui.progress_overlay_visible = false;
  deps.invalidate_all_render_cache();
  deps.clamp_text_scroll();
  return true;
}

void PersistCurrentTxtResumeSnapshot(const std::string &book_path, bool force, TxtReaderSessionDeps &deps) {
  if (book_path.empty()) return;
  if (deps.ui.mode != ReaderMode::Txt || !deps.ui.txt_reader.open) return;
  if (!force && !deps.ui.txt_reader.resume_cache_dirty) return;
  const uint32_t now = SDL_GetTicks();
  if (!force && deps.ui.txt_reader.last_resume_cache_save != 0 &&
      now - deps.ui.txt_reader.last_resume_cache_save < deps.txt_resume_save_delay_ms) {
    return;
  }

  TxtReaderState snapshot = deps.ui.txt_reader;
  snapshot.scroll_px = deps.ui.txt_reader.scroll_px;
  snapshot.target_scroll_px = deps.ui.txt_reader.scroll_px;
  snapshot.restore_source_offset = TopVisibleSourceOffset(snapshot);
  snapshot.resume_cache_dirty = false;
  snapshot.last_resume_cache_save = now;
  if (snapshot.cache_key.empty()) {
    const SDL_Rect bounds{snapshot.viewport_x, snapshot.viewport_y, snapshot.viewport_w, snapshot.viewport_h};
    std::error_code ec;
    const uintmax_t file_size = std::filesystem::file_size(std::filesystem::path(book_path), ec);
    const auto mtime_raw = std::filesystem::last_write_time(std::filesystem::path(book_path), ec);
    const long long file_mtime = ec ? 0LL : static_cast<long long>(mtime_raw.time_since_epoch().count());
    snapshot.cache_key = deps.make_layout_cache_key(book_path, bounds, snapshot.line_h, ec ? 0 : file_size, file_mtime);
  }
  if (snapshot.cache_key.empty()) return;
  deps.save_resume_cache_to_disk(snapshot.cache_key, book_path, snapshot);
  deps.ui.txt_reader.last_resume_cache_save = now;
  deps.ui.txt_reader.resume_cache_dirty = false;
}

void TextScrollBy(int delta_px, const std::string &book_path, TxtReaderSessionDeps &deps) {
  if (deps.ui.mode != ReaderMode::Txt || !deps.ui.txt_reader.open) return;
  deps.ui.txt_reader.restore_source_offset = 0;
  deps.ui.txt_reader.scroll_px += delta_px;
  deps.ui.txt_reader.target_scroll_px = deps.ui.txt_reader.scroll_px;
  deps.clamp_text_scroll();
  deps.ui.txt_reader.resume_cache_dirty = true;
  PersistCurrentTxtResumeSnapshot(book_path, false, deps);
}

void TextPageBy(int dir, const std::string &book_path, TxtReaderSessionDeps &deps) {
  if (deps.ui.mode != ReaderMode::Txt || !deps.ui.txt_reader.open) return;
  const int step = std::max(80, deps.ui.txt_reader.viewport_h - deps.ui.txt_reader.line_h);
  TextScrollBy(dir * step, book_path, deps);
}

void TextJumpToPercent(int pct, const std::string &book_path, TxtReaderSessionDeps &deps) {
  if (deps.ui.mode != ReaderMode::Txt || !deps.ui.txt_reader.open) return;
  TxtReaderState &state = deps.ui.txt_reader;
  const int clamped_pct = ClampIntLocal(pct, 0, 100);
  const int current_max_scroll = std::max(0, state.content_h - state.viewport_h);
  int target_scroll = ScrollForPercent(clamped_pct, current_max_scroll);
  state.restore_source_offset = 0;
  state.target_scroll_px = target_scroll;
  if (state.loading) {
    WarmTextReaderToTarget(state, &state.cache_key, deps);
  }
  const int max_scroll = std::max(0, state.content_h - state.viewport_h);
  target_scroll = ScrollForPercent(clamped_pct, max_scroll);
  state.target_scroll_px = target_scroll;
  state.scroll_px = std::clamp(target_scroll, 0, max_scroll);
  deps.clamp_text_scroll();
  state.resume_cache_dirty = true;
  PersistCurrentTxtResumeSnapshot(book_path, false, deps);
}

int TxtReaderProgressPercent(const TxtReaderState &state) {
  const int max_scroll = std::max(0, state.content_h - state.viewport_h);
  if (max_scroll <= 0) return 100;
  return ClampIntLocal(static_cast<int>((static_cast<int64_t>(state.scroll_px) * 100) / max_scroll), 0, 100);
}
