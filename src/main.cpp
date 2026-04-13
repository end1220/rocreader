#include <SDL.h>
#ifdef HAVE_SDL2_IMAGE
#include <SDL_image.h>
#endif
#ifdef HAVE_SDL2_TTF
#include <SDL_ttf.h>
#endif
#ifdef HAVE_SDL2_MIXER
#include <SDL_mixer.h>
#endif

#include <algorithm>
#include <atomic>
#include <array>
#include <cmath>
#include <cstdlib>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <codecvt>
#include <limits>
#include <locale>
#if !defined(_WIN32) && __has_include(<iconv.h>)
#include <errno.h>
#include <iconv.h>
#define ROCREADER_HAS_ICONV 1
#else
#define ROCREADER_HAS_ICONV 0
#endif
#include <sstream>
#include <string>
#include <tuple>
#include <unordered_set>
#include <unordered_map>
#include <utility>
#include <vector>

#include "app_runtime.h"
#include "app_stores.h"
#include "audio_runtime.h"
#include "book_scanner.h"
#include "boot_runtime.h"
#include "contributor_avatar_runtime.h"
#include "cover_service.h"
#include "epub_runtime.h"
#include "epub_reader.h"
#include "input_manager.h"
#include "lid_power_control.h"
#include "pdf_reader.h"
#include "pdf_runtime.h"
#include "progress_store.h"
#include "reader_core.h"
#include "reader_session_ops.h"
#include "reader_session_state.h"
#include "screen_profile.h"
#include "system_controls.h"
#include "system_settings_runtime.h"
#include "shelf_runtime.h"
#include "settings_runtime.h"
#include "sdl_utils.h"
#include "storage_paths.h"
#include "system_status.h"
#include "txt_reader_session.h"
#include "txt_reader_runtime.h"
#include "txt_text_service.h"
#include "ui_assets.h"
#include "ui_assets_loader.h"
#include "ui_text_cache.h"
#include "version_update_runtime.h"
#include "animation.h"

namespace {
struct LayoutMetrics {
  int screen_w = 720;
  int screen_h = 480;
  int safe_margin_x = 20;
  int top_bar_y = 0;
  int top_bar_h = 30;
  int nav_bar_y = 30;
  int nav_bar_h = 50;
  int main_grid_y = 80;
  int main_grid_h = 350;
  int bottom_bar_y = 430;
  int bottom_bar_h = 50;
  int cover_w = 140;
  int cover_h = 210;
  int card_frame_w = 180;
  int card_frame_h = 250;
  int grid_gap_x = 33;
  int grid_gap_y = 43;
  int grid_start_x = 33;
  int grid_start_y = 100;
  int title_overlay_h = 36;
  int title_text_pad_x = 2;
  int title_text_pad_bottom = 4;
  int title_marquee_gap_px = 24;
  int settings_sidebar_w = 240;
  int settings_y_offset = 0;
  int settings_content_offset_y = 35;
  int txt_margin_x = 32;
  int txt_margin_y = 20;
  int nav_l1_x = 21;
  int nav_l1_y = 46;
  int nav_r1_x = 667;
  int nav_r1_y = 46;
  int nav_start_x = 90;
  int nav_slot_w = 135;
  int nav_y = 42;
  int reader_progress_panel_margin_x = 18;
  int reader_progress_panel_margin_bottom = 12;
  int reader_progress_bar_margin_x = 34;
  int reader_progress_percent_margin_x = 34;
};

constexpr LayoutMetrics layout_720x480{
    720, 480, 20,
    0, 30,
    30, 50,
    80, 350,
    430, 50,
    140, 210,
    180, 250,
    33, 38,
    33, 100,
    36, 2, 4, 24,
    240, 0, 35,
    32, 20,
    21, 46,
    667, 46,
    90, 135, 42,
    18, 12, 34, 34,
};

constexpr LayoutMetrics layout_640x480{
    640, 480, 16,
    0, 30,
    30, 50,
    80, 350,
    430, 50,
    130, 195,
    167, 232,
    25, 30,
    23, 100,
    36, 2, 4, 24,
    160, 0, 35,
    28, 20,
    14, 46,
    587, 46,
    72, 124, 42,
    18, 12, 34, 34,
};

constexpr int kGridCols = 4;
constexpr int kGridRows = 2;
constexpr int kItemsPerPage = kGridCols * kGridRows;
constexpr float kFocusScaleBase = 1.0f;
constexpr float kFocusScaleCurrent = 1.045f; // reduce current zoomed size by 5%
constexpr float kFocusScale = kFocusScaleCurrent;
constexpr float kCoverAspect = 2.0f / 3.0f;
constexpr Uint8 kUnfocusedAlpha = 255;
constexpr float kTitleMarqueePauseSec = 0.75f;
constexpr float kTitleMarqueeSpeedPx = 48.0f;
constexpr size_t kCoverCacheMaxEntries = 160;
constexpr size_t kCoverCacheMaxBytes = 24u * 1024u * 1024u;
constexpr Uint8 kSidebarMaskMaxAlpha = 84;
constexpr float kSceneFadeFlashAlpha = 0.82f;
constexpr float kSceneFadeFlashDurationSec = 0.18f;
constexpr int kIdleWaitMs = 100;
constexpr uint32_t kActiveFrameBudgetMs = 33;
constexpr uint32_t kAvatarMarqueeFrameBudgetMs = 16;
constexpr uint32_t kPeriodicTickFrameBudgetMs = 50;
constexpr float kCardLerpSpeed = 18.0f;
constexpr float kCardMoveLinearSpeedX = 860.0f;  // px/s for center move transition
constexpr float kCardMoveLinearSpeedY = 860.0f;  // px/s for center move transition
constexpr float kCardMoveTailRatio = 0.52f;      // last 52% enters slow tail
constexpr float kCardMoveTailMinMul = 0.12f;     // tail minimum speed multiplier
constexpr float kCardScaleLinearSpeedW = 140.0f; // px/s for width scale transition
constexpr float kCardScaleLinearSpeedH = 210.0f; // px/s for height scale transition
constexpr float kCardScaleTailRatio = 0.52f;     // last 52% enters slow tail
constexpr float kCardScaleTailMinMul = 0.10f;    // tail minimum speed multiplier
constexpr float kPageSlideDurationSec = 0.52f;
constexpr int kReaderTapStepPx = 56;
constexpr int kTxtProgressOverlayTapStepPct = 1;
constexpr float kTxtProgressOverlayHoldDelaySec = 0.25f;
constexpr float kTxtProgressOverlayHoldSpeedMinPct = 6.0f;
constexpr float kTxtProgressOverlayHoldSpeedMaxPct = 26.0f;
constexpr float kTxtProgressOverlayHoldAccelPct = 36.0f;
constexpr float kSettingsToggleGuardSec = 0.16f;
constexpr float kMenuToggleDebounceSec = 0.12f;
constexpr uint32_t kDeferredSaveDelayMs = 1500;
constexpr uint32_t kTxtResumeSaveDelayMs = 2000;
constexpr uint32_t kShelfScanCacheTtlMs = 3000;
constexpr size_t kShelfScanCacheMaxEntries = 24;
constexpr uint32_t kIdleFlushOnlyWaitMs = 250;
constexpr size_t kBootCountBatchEntries = 96;
constexpr size_t kBootScanBatchEntries = 48;
constexpr size_t kBootCoverGenerateBatchEntries = 1;
constexpr uint32_t kTransientMessageDurationMs = 1800;
constexpr uint32_t kReaderFastFlipThresholdMs = 200;
constexpr uint32_t kReaderPageFlipDebounceMs = 150;
constexpr int kTxtLineSpacing = 8;
constexpr int kTxtLayoutCacheVersion = 3;
constexpr size_t kTxtMaxBytes = 64 * 1024 * 1024;
constexpr size_t kTxtMaxWrappedLines = 250000;
constexpr size_t kTxtLayoutCacheMaxEntries = 4;
#ifdef HAVE_SDL2_TTF
constexpr size_t kTextCacheMaxEntries = 128;
#endif

const LayoutMetrics *g_layout = &layout_720x480;

const LayoutMetrics &Layout() { return *g_layout; }

const LayoutMetrics &SelectLayoutProfile(int screen_w, int screen_h) {
  if (screen_w == layout_640x480.screen_w && screen_h == layout_640x480.screen_h) return layout_640x480;
  return layout_720x480;
}

int FocusedCoverW() { return static_cast<int>(Layout().cover_w * kFocusScale + 0.5f); }
int FocusedCoverH() { return static_cast<int>(Layout().cover_h * kFocusScale + 0.5f); }

std::string NormalizePathKey(const std::string &path);

enum class State { Boot, Shelf, Settings, Reader };
struct CoverCacheEntry {
  SDL_Texture *texture = nullptr;
  int w = 0;
  int h = 0;
  size_t bytes = 0;
  uint32_t last_use = 0;
  bool owned = true;
};

int ClampInt(int v, int lo, int hi) { return std::max(lo, std::min(hi, v)); }

std::string ToLowerAscii(std::string s) {
  std::transform(s.begin(), s.end(), s.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return s;
}

std::string NormalizePathKey(const std::string &path) {
  if (path.empty()) return {};
  try {
    std::filesystem::path p(path);
    p = p.lexically_normal();
    std::string out = p.generic_string();
#ifdef _WIN32
    out = ToLowerAscii(out);
#endif
    return out;
  } catch (...) {
    return path;
  }
}

std::string GetLowerExt(const std::string &path) {
  try {
    std::string ext = std::filesystem::path(path).extension().string();
    return ToLowerAscii(ext);
  } catch (...) {
    return {};
  }
}

bool TryExtractVersionFromArchiveName(const std::string &filename, std::string &out_version) {
  const std::string lower_name = ToLowerAscii(filename);
  const size_t ver_pos = lower_name.rfind("ver");
  const size_t zip_pos = lower_name.rfind(".zip");
  if (ver_pos == std::string::npos || zip_pos == std::string::npos || ver_pos >= zip_pos) return false;
  const size_t version_start = ver_pos;
  const size_t version_len = zip_pos - version_start;
  if (version_len == 0) return false;
  out_version = filename.substr(version_start, version_len);
  return !out_version.empty();
}

std::string DetectVersionFromDownloadsDir(const std::filesystem::path &downloads_dir) {
  std::error_code ec;
  if (downloads_dir.empty() || !std::filesystem::exists(downloads_dir, ec) || ec ||
      !std::filesystem::is_directory(downloads_dir, ec)) {
    return {};
  }

  std::filesystem::file_time_type newest_time{};
  bool found = false;
  std::string best_version;

  for (std::filesystem::directory_iterator it(downloads_dir, ec), end; it != end; it.increment(ec)) {
    if (ec) {
      ec.clear();
      continue;
    }
    if (!it->is_regular_file(ec)) {
      ec.clear();
      continue;
    }
    std::string version;
    if (!TryExtractVersionFromArchiveName(it->path().filename().string(), version)) continue;
    const auto write_time = it->last_write_time(ec);
    if (ec) {
      ec.clear();
      continue;
    }
    if (!found || write_time > newest_time) {
      newest_time = write_time;
      best_version = version;
      found = true;
    }
  }

  return found ? best_version : std::string{};
}

std::string DetectVersionLabel(const std::filesystem::path &runtime_root) {
  std::vector<std::filesystem::path> candidates;
  for (const std::string &root : storage_paths::DetectRocreaderRoots()) {
    if (!root.empty()) candidates.push_back(std::filesystem::path(root) / "version.txt");
  }
  if (!runtime_root.empty()) {
    candidates.push_back(runtime_root / "version.txt");
    candidates.push_back(runtime_root.parent_path() / "version.txt");
    candidates.push_back(runtime_root / "Downloads");
    candidates.push_back(runtime_root.parent_path() / "Downloads");
  }
  std::error_code ec;
  const std::filesystem::path cwd = std::filesystem::current_path(ec);
  if (!ec) {
    candidates.push_back(cwd / "version.txt");
    candidates.push_back(cwd.parent_path() / "version.txt");
    candidates.push_back(cwd / "Downloads");
    candidates.push_back(cwd.parent_path() / "Downloads");
  }

  for (const auto &candidate : candidates) {
    if (candidate.filename() == "version.txt") {
      std::ifstream in(candidate);
      std::string version;
      if (in && std::getline(in, version) && !version.empty()) return version;
      continue;
    }
    const std::string version = DetectVersionFromDownloadsDir(candidate);
    if (!version.empty()) return version;
  }
  return "v0.0.0-ui";
}

std::string Utf8Ellipsize(const std::string &text, size_t max_chars) {
  if (text.empty() || max_chars == 0) return {};
  size_t chars = 0;
  size_t bytes = 0;
  while (bytes < text.size() && chars < max_chars) {
    const unsigned char c = static_cast<unsigned char>(text[bytes]);
    size_t len = 1;
    if ((c & 0x80) == 0x00) len = 1;
    else if ((c & 0xE0) == 0xC0) len = 2;
    else if ((c & 0xF0) == 0xE0) len = 3;
    else if ((c & 0xF8) == 0xF0) len = 4;
    if (bytes + len > text.size()) break;
    bytes += len;
    ++chars;
  }
  if (bytes >= text.size()) return text;
  if (chars <= 1) return "...";
  return text.substr(0, bytes) + "...";
}

size_t Utf8CharLen(unsigned char c) {
  if ((c & 0x80) == 0x00) return 1;
  if ((c & 0xE0) == 0xC0) return 2;
  if ((c & 0xF0) == 0xE0) return 3;
  if ((c & 0xF8) == 0xF0) return 4;
  return 1;
}

bool IsValidUtf8(const std::string &text) {
  size_t i = 0;
  while (i < text.size()) {
    const unsigned char c = static_cast<unsigned char>(text[i]);
    size_t len = 0;
    uint32_t codepoint = 0;
    if ((c & 0x80) == 0x00) {
      len = 1;
      codepoint = c;
    } else if ((c & 0xE0) == 0xC0) {
      len = 2;
      codepoint = c & 0x1F;
      if (codepoint < 0x02) return false;
    } else if ((c & 0xF0) == 0xE0) {
      len = 3;
      codepoint = c & 0x0F;
    } else if ((c & 0xF8) == 0xF0) {
      len = 4;
      codepoint = c & 0x07;
      if (codepoint > 0x04) return false;
    } else {
      return false;
    }
    if (i + len > text.size()) return false;
    for (size_t j = 1; j < len; ++j) {
      const unsigned char cc = static_cast<unsigned char>(text[i + j]);
      if ((cc & 0xC0) != 0x80) return false;
      codepoint = (codepoint << 6) | (cc & 0x3F);
    }
    if ((len == 2 && codepoint < 0x80) ||
        (len == 3 && codepoint < 0x800) ||
        (len == 4 && codepoint < 0x10000) ||
        codepoint > 0x10FFFF ||
        (codepoint >= 0xD800 && codepoint <= 0xDFFF)) {
      return false;
    }
    i += len;
  }
  return true;
}

bool TryConvertUtf16BomToUtf8(const std::string &raw, std::string &out) {
  if (raw.size() < 2) return false;
  const unsigned char b0 = static_cast<unsigned char>(raw[0]);
  const unsigned char b1 = static_cast<unsigned char>(raw[1]);
  if (b0 == 0xFF && b1 == 0xFE) {
    std::u16string u16;
    u16.reserve((raw.size() - 2) / 2);
    for (size_t i = 2; i + 1 < raw.size(); i += 2) {
      char16_t ch = static_cast<char16_t>(
          static_cast<unsigned char>(raw[i]) |
          (static_cast<unsigned char>(raw[i + 1]) << 8));
      u16.push_back(ch);
    }
    std::wstring_convert<std::codecvt_utf8_utf16<char16_t>, char16_t> conv;
    out = conv.to_bytes(u16);
    return true;
  }
  if (b0 == 0xFE && b1 == 0xFF) {
    std::u16string u16;
    u16.reserve((raw.size() - 2) / 2);
    for (size_t i = 2; i + 1 < raw.size(); i += 2) {
      char16_t ch = static_cast<char16_t>(
          (static_cast<unsigned char>(raw[i]) << 8) |
          static_cast<unsigned char>(raw[i + 1]));
      u16.push_back(ch);
    }
    std::wstring_convert<std::codecvt_utf8_utf16<char16_t>, char16_t> conv;
    out = conv.to_bytes(u16);
    return true;
  }
  return false;
}

double ScoreDecodedTextCandidate(const std::string &text) {
  if (text.empty()) return 0.0;
  double score = 0.0;
  size_t total = 0;
  size_t weird = 0;
  for (size_t i = 0; i < text.size();) {
    const unsigned char c = static_cast<unsigned char>(text[i]);
    size_t len = Utf8CharLen(c);
    if (i + len > text.size()) {
      score -= 20.0;
      ++weird;
      ++total;
      break;
    }
    uint32_t codepoint = 0;
    if (len == 1) {
      codepoint = c;
    } else {
      codepoint = c & ((1u << (8 - len - 1)) - 1u);
      bool invalid = false;
      for (size_t j = 1; j < len; ++j) {
        const unsigned char cc = static_cast<unsigned char>(text[i + j]);
        if ((cc & 0xC0) != 0x80) {
          invalid = true;
          break;
        }
        codepoint = (codepoint << 6) | (cc & 0x3F);
      }
      if (invalid) {
        score -= 20.0;
        ++weird;
        ++total;
        i += len;
        continue;
      }
    }

    if (codepoint == 0xFFFD) {
      score -= 18.0;
      ++weird;
    } else if (codepoint == '\r' || codepoint == '\n' || codepoint == '\t') {
      score += 0.4;
    } else if (codepoint >= 0x20 && codepoint <= 0x7E) {
      score += 0.8;
    } else if ((codepoint >= 0x4E00 && codepoint <= 0x9FFF) ||
               (codepoint >= 0x3400 && codepoint <= 0x4DBF) ||
               (codepoint >= 0x3040 && codepoint <= 0x30FF) ||
               (codepoint >= 0xAC00 && codepoint <= 0xD7AF)) {
      score += 2.4;
    } else if ((codepoint >= 0x3000 && codepoint <= 0x303F) ||
               (codepoint >= 0xFF00 && codepoint <= 0xFFEF)) {
      score += 1.6;
    } else if (codepoint < 0x20 || (codepoint >= 0x7F && codepoint <= 0x9F)) {
      score -= 10.0;
      ++weird;
    } else {
      score += 0.2;
    }

    ++total;
    i += len;
  }

  if (total == 0) return -1e9;
  score -= static_cast<double>(weird) * 1.5;
  score /= static_cast<double>(total);
  return score;
}

#if ROCREADER_HAS_ICONV
bool TryConvertEncodingToUtf8Iconv(const std::string &raw, const char *from_encoding, std::string &out) {
  iconv_t cd = iconv_open("UTF-8", from_encoding);
  if (cd == reinterpret_cast<iconv_t>(-1)) return false;
  out.clear();
  size_t in_left = raw.size();
  char *in_buf = const_cast<char *>(raw.data());
  std::vector<char> chunk(std::max<size_t>(4096, raw.size() * 4 + 32));
  while (true) {
    char *out_buf = chunk.data();
    size_t out_left = chunk.size();
    const size_t rc = iconv(cd, &in_buf, &in_left, &out_buf, &out_left);
    out.append(chunk.data(), chunk.size() - out_left);
    if (rc != static_cast<size_t>(-1)) break;
    if (errno == E2BIG) continue;
    iconv_close(cd);
    out.clear();
    return false;
  }
  iconv_close(cd);
  return IsValidUtf8(out);
}
#endif

bool DecodeTextBytesToUtf8(const std::string &raw, std::string &out, std::string *detected_encoding = nullptr) {
  out.clear();
  if (raw.empty()) {
    if (detected_encoding) *detected_encoding = "empty";
    return true;
  }
  if (TryConvertUtf16BomToUtf8(raw, out)) {
    if (detected_encoding) *detected_encoding = "UTF-16";
    return true;
  }
  if (raw.size() >= 3 &&
      static_cast<unsigned char>(raw[0]) == 0xEF &&
      static_cast<unsigned char>(raw[1]) == 0xBB &&
      static_cast<unsigned char>(raw[2]) == 0xBF) {
    out.assign(raw.begin() + 3, raw.end());
    if (IsValidUtf8(out)) {
      if (detected_encoding) *detected_encoding = "UTF-8 BOM";
      return true;
    }
    out.clear();
  }

  struct Candidate {
    std::string text;
    std::string encoding;
    double score = -1e9;
  };
  Candidate best{};
  bool found_candidate = false;

  auto consider_candidate = [&](std::string candidate_text, const std::string &encoding) {
    if (!IsValidUtf8(candidate_text)) return;
    Candidate candidate;
    candidate.text = std::move(candidate_text);
    candidate.encoding = encoding;
    candidate.score = ScoreDecodedTextCandidate(candidate.text);
    if (!found_candidate || candidate.score > best.score + 0.08 ||
        (std::abs(candidate.score - best.score) <= 0.08 && encoding == "UTF-8")) {
      best = std::move(candidate);
      found_candidate = true;
    }
  };

  if (IsValidUtf8(raw)) {
    consider_candidate(raw, "UTF-8");
  }
#if ROCREADER_HAS_ICONV
  static const std::array<const char *, 4> kLegacyEncodings = {"GB18030", "GBK", "GB2312", "BIG5"};
  for (const char *encoding : kLegacyEncodings) {
    std::string converted;
    if (TryConvertEncodingToUtf8Iconv(raw, encoding, converted)) {
      consider_candidate(std::move(converted), encoding);
    }
  }
#endif
  if (!found_candidate) {
    out.clear();
    return false;
  }
  out = std::move(best.text);
  if (detected_encoding) *detected_encoding = best.encoding;
  return true;
}

bool ReadFileBytes(const std::filesystem::path &path, std::string &raw) {
  std::ifstream in(path, std::ios::binary);
  if (!in) return false;
  try {
    std::ostringstream oss;
    oss << in.rdbuf();
    raw = oss.str();
    return in.good() || in.eof();
  } catch (...) {
    raw.clear();
    return false;
  }
}

bool WriteFileBytesAtomically(const std::filesystem::path &path, const std::string &data) {
  const std::filesystem::path temp_path = path.string() + ".rocreader_tmp";
  {
    std::ofstream out(temp_path, std::ios::binary | std::ios::trunc);
    if (!out) return false;
    out.write(data.data(), static_cast<std::streamsize>(data.size()));
    if (!out.good()) {
      out.close();
      std::error_code cleanup_ec;
      std::filesystem::remove(temp_path, cleanup_ec);
      return false;
    }
  }
  std::error_code ec;
  std::filesystem::rename(temp_path, path, ec);
  if (!ec) return true;
  ec.clear();
  std::filesystem::remove(path, ec);
  ec.clear();
  std::filesystem::rename(temp_path, path, ec);
  if (!ec) return true;
  std::filesystem::remove(temp_path, ec);
  return false;
}

} // namespace

int main(int, char **) {
  if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_GAMECONTROLLER | SDL_INIT_JOYSTICK | SDL_INIT_EVENTS) != 0) {
    std::cerr << "[native_h700] SDL init failed: " << SDL_GetError() << "\n";
    return 1;
  }
  SDL_JoystickEventState(SDL_ENABLE);
#ifdef HAVE_SDL2_TTF
  if (TTF_Init() != 0) {
    std::cerr << "[native_h700] SDL2_ttf init warning: " << TTF_GetError() << "\n";
  }
#endif
#ifdef HAVE_SDL2_IMAGE
  const int img_flags = IMG_INIT_PNG | IMG_INIT_JPG | IMG_INIT_WEBP;
  const int img_ok = IMG_Init(img_flags);
  if ((img_ok & img_flags) == 0) {
    std::cerr << "[native_h700] SDL2_image init warning: " << IMG_GetError() << "\n";
  }
#endif
  const char *env_windowed = std::getenv("ROCREADER_WINDOWED");
  const char *env_fullscreen = std::getenv("ROCREADER_FULLSCREEN");
  const bool force_windowed = env_windowed && std::string(env_windowed) == "1";
  const bool force_fullscreen = env_fullscreen && std::string(env_fullscreen) == "1";
  const ScreenProfile screen_profile = DetectScreenProfile();

  uint32_t win_flags = SDL_WINDOW_SHOWN;
#if defined(__arm__) || defined(__aarch64__)
  const bool default_fullscreen = true;
#else
  const bool default_fullscreen = false;
#endif
  if ((default_fullscreen && !force_windowed) || force_fullscreen) {
    win_flags |= SDL_WINDOW_FULLSCREEN;
  }
  g_layout = &SelectLayoutProfile(screen_profile.screen_w, screen_profile.screen_h);
  std::cout << "[native_h700] screen detect: source=" << screen_profile.detection_source
            << " detected=" << screen_profile.detected_w << "x" << screen_profile.detected_h
            << " profile=" << screen_profile.profile_name
            << " layout=" << Layout().screen_w << "x" << Layout().screen_h << "\n";

  SDL_Window *window =
      SDL_CreateWindow("ROCreader Native H700",
                       SDL_WINDOWPOS_CENTERED,
                       SDL_WINDOWPOS_CENTERED,
                       Layout().screen_w,
                       Layout().screen_h,
                       win_flags);
  if (!window) {
    std::cerr << "[native_h700] window failed: " << SDL_GetError() << "\n";
    SDL_Quit();
    return 2;
  }
  SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "0");
  SDL_Renderer *renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
  if (!renderer) renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_SOFTWARE);
  if (!renderer) {
    std::cerr << "[native_h700] renderer failed: " << SDL_GetError() << "\n";
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 3;
  }
  SDL_RendererInfo renderer_info{};
  if (SDL_GetRendererInfo(renderer, &renderer_info) == 0) {
    std::cout << "[native_h700] renderer: " << (renderer_info.name ? renderer_info.name : "unknown")
              << " flags=0x" << std::hex << renderer_info.flags << std::dec
              << " accelerated=" << ((renderer_info.flags & SDL_RENDERER_ACCELERATED) ? "yes" : "no")
              << " vsync=" << ((renderer_info.flags & SDL_RENDERER_PRESENTVSYNC) ? "yes" : "no") << "\n";
  }
  const bool renderer_supports_target_textures = (renderer_info.flags & SDL_RENDERER_TARGETTEXTURE) != 0;

  std::vector<SDL_GameController *> opened_controllers;
  std::vector<SDL_Joystick *> opened_joysticks;
  const int joystick_count = SDL_NumJoysticks();
  std::cout << "[native_h700] joysticks: " << joystick_count << "\n";
  for (int i = 0; i < joystick_count; ++i) {
    if (SDL_IsGameController(i)) {
      SDL_GameController *gc = SDL_GameControllerOpen(i);
      if (gc) {
        opened_controllers.push_back(gc);
        std::cout << "[native_h700] opened gamecontroller idx=" << i << "\n";
        continue;
      }
    }
    SDL_Joystick *js = SDL_JoystickOpen(i);
    if (js) {
      opened_joysticks.push_back(js);
      std::cout << "[native_h700] opened joystick idx=" << i << "\n";
    }
  }

  std::string exe_dir = ".";
  if (char *base = SDL_GetBasePath(); base && *base) {
    exe_dir = base;
    SDL_free(base);
  }
  std::filesystem::path exe_path(exe_dir);
  std::filesystem::path ui_path = exe_path / "ui";
  auto resolve_runtime_file = [&](const std::string &name) -> std::filesystem::path {
    const std::vector<std::filesystem::path> candidates = {
        exe_path / name,
        exe_path / ".." / name,
        std::filesystem::current_path() / name,
    };
    for (const auto &p : candidates) {
      if (std::filesystem::exists(p)) return p.lexically_normal();
    }
    return (exe_path / name).lexically_normal();
  };

  UiAssets ui_assets;
  std::unordered_map<SDL_Texture *, SDL_Point> texture_sizes;
  auto forget_texture_size = [&](SDL_Texture *tex) {
    if (!tex) return;
    texture_sizes.erase(tex);
  };
  auto remember_texture_size = [&](SDL_Texture *tex, int w, int h) {
    if (!tex) return;
    texture_sizes[tex] = SDL_Point{w, h};
  };
  auto get_texture_size = [&](SDL_Texture *tex, int &w, int &h) {
    w = 0;
    h = 0;
    if (!tex) return;
    auto it = texture_sizes.find(tex);
    if (it != texture_sizes.end()) {
      w = it->second.x;
      h = it->second.y;
      return;
    }
    SDL_QueryTexture(tex, nullptr, nullptr, &w, &h);
    remember_texture_size(tex, w, h);
  };
  UiAssetsLoaderDeps ui_assets_loader_deps{
      renderer,
      exe_path,
      screen_profile.profile_name,
      LoadTextureFromFile,
      LoadSurfaceFromMemory,
      CreateTextureFromSurface,
      remember_texture_size,
  };
  UiAssetsLoadResult ui_assets_load_result = LoadUiAssets(ui_assets, ui_assets_loader_deps);
  if (!ui_assets_load_result.ui_pack_hit.empty()) {
    std::cout << "[native_h700] ui pack: " << ui_assets_load_result.ui_pack_hit.string()
              << " assets=" << ui_assets_load_result.packed_asset_count << "\n";
  }
  if (!ui_assets_load_result.ui_root_hit.empty()) {
    std::cout << "[native_h700] ui root: " << ui_assets_load_result.ui_root_hit.string() << "\n";
  }

  std::vector<ContributorAvatarEntry> contributor_avatar_entries;
  auto resolve_ui_root = [&]() -> std::filesystem::path {
    if (!ui_assets_load_result.ui_root_hit.empty()) {
      const std::filesystem::path common_root = ui_assets_load_result.ui_root_hit / "common";
      if (std::filesystem::exists(common_root)) return common_root.lexically_normal();
      return ui_assets_load_result.ui_root_hit;
    }
    const std::vector<std::filesystem::path> candidates = {
        exe_path / "ui" / "common",
        exe_path / ".." / "ui" / "common",
        std::filesystem::current_path() / "ui" / "common",
        exe_path / "ui",
        exe_path / ".." / "ui",
        std::filesystem::current_path() / "ui",
    };
    for (const auto &candidate : candidates) {
      if (std::filesystem::exists(candidate)) return candidate.lexically_normal();
    }
    return {};
  };
  LoadContributorAvatarEntries(contributor_avatar_entries, resolve_ui_root(), exe_path, renderer,
                               LoadSurfaceFromMemory, remember_texture_size, forget_texture_size);
  SDL_Texture *selected_avatar_badge_texture = nullptr;
  auto destroy_selected_avatar_badge_texture = [&]() {
    if (!selected_avatar_badge_texture) return;
    forget_texture_size(selected_avatar_badge_texture);
    SDL_DestroyTexture(selected_avatar_badge_texture);
    selected_avatar_badge_texture = nullptr;
  };
  auto update_selected_avatar_badge_texture = [&](int selected_index) {
    destroy_selected_avatar_badge_texture();
    if (selected_index < 0 || selected_index >= static_cast<int>(contributor_avatar_entries.size())) return;
    SDL_Texture *source = contributor_avatar_entries[selected_index].texture;
    if (!source) return;
    selected_avatar_badge_texture = CreateScaledTextureCache(renderer, source, 28, 28);
    if (!selected_avatar_badge_texture) return;
    remember_texture_size(selected_avatar_badge_texture, 28, 28);
  };
  auto find_default_contributor_avatar_index = [&]() -> int {
    if (contributor_avatar_entries.empty()) return 0;
    int default_index = 0;
    for (size_t i = 0; i < contributor_avatar_entries.size(); ++i) {
      if (contributor_avatar_entries[i].label == u8"BloodROC_贡献值MAX") {
        return static_cast<int>(i);
      }
    }
    for (size_t i = 0; i < contributor_avatar_entries.size(); ++i) {
      if (contributor_avatar_entries[i].label.find(u8"贡献值MAX") != std::string::npos) {
        return static_cast<int>(i);
      }
    }
    return default_index;
  };
  auto initialize_selected_avatar_badge_texture = [&]() {
    if (contributor_avatar_entries.empty()) return;
    update_selected_avatar_badge_texture(find_default_contributor_avatar_index());
  };
  initialize_selected_avatar_badge_texture();

  std::vector<std::string> books_roots = storage_paths::DetectBooksRoots();
  std::vector<std::string> cover_roots = storage_paths::DetectCoverRoots();
  std::filesystem::path txt_layout_cache_dir =
      std::filesystem::path("/mnt/mmc/cache/txt_layouts");
  std::filesystem::path removable_txt_layout_cache_dir =
      std::filesystem::path("/mnt/sdcard/cache/txt_layouts");
  std::filesystem::path cover_thumb_cache_dir =
      std::filesystem::path("/mnt/mmc/cache/cover_thumbs");
  std::filesystem::path removable_cover_thumb_cache_dir =
      std::filesystem::path("/mnt/sdcard/cache/cover_thumbs");
  {
    std::error_code ec;
    std::filesystem::create_directories(txt_layout_cache_dir, ec);
  }
  {
    std::error_code ec;
    std::filesystem::create_directories(removable_txt_layout_cache_dir, ec);
  }
  {
    std::error_code ec;
    std::filesystem::create_directories(cover_thumb_cache_dir, ec);
  }
  {
    std::error_code ec;
    std::filesystem::create_directories(removable_cover_thumb_cache_dir, ec);
  }
  std::cout << "[native_h700] books roots:";
  for (const auto &r : books_roots) std::cout << " " << r;
  std::cout << "\n";
  std::cout << "[native_h700] cover roots:";
  for (const auto &r : cover_roots) std::cout << " " << r;
  std::cout << "\n";
  std::cout << "[native_h700] cover thumb cache dir: "
            << cover_thumb_cache_dir.lexically_normal().string() << "\n";
  std::cout << "[native_h700] removable cover thumb cache dir: "
            << removable_cover_thumb_cache_dir.lexically_normal().string() << "\n";
  std::cout << "[native_h700] txt layout cache dir: "
            << txt_layout_cache_dir.lexically_normal().string() << "\n";
  std::cout << "[native_h700] removable txt layout cache dir: "
            << removable_txt_layout_cache_dir.lexically_normal().string() << "\n";

  const std::filesystem::path keymap_path = resolve_runtime_file("native_keymap.ini");
#if defined(__arm__) || defined(__aarch64__)
  const bool use_h700_defaults = true;
  const std::string device_model_token = DetectDeviceModelToken();
  const bool use_h700_34xx_keymap =
      device_model_token.find("34xx") != std::string::npos || screen_profile.screen_w >= 720;
#else
  const bool use_h700_defaults = false;
  const std::string device_model_token;
  const bool use_h700_34xx_keymap = false;
#endif
  const std::filesystem::path config_path = resolve_runtime_file("native_config.ini");
  const std::filesystem::path progress_path = resolve_runtime_file("native_progress.tsv");
  const std::filesystem::path favorites_path = resolve_runtime_file("native_favorites.txt");
  const std::filesystem::path history_path = resolve_runtime_file("native_history.txt");
  const char *env_power_script = std::getenv("ROCREADER_PWR_SCRIPT");
  const std::filesystem::path power_script_path =
      (env_power_script && *env_power_script) ? std::filesystem::path(env_power_script)
                                              : std::filesystem::path("/mnt/mod/ctrl/pwr_new.sh");
  std::cout << "[native_h700] keymap path: " << keymap_path.lexically_normal().string() << "\n";
  std::cout << "[native_h700] config path: " << config_path.lexically_normal().string() << "\n";
  std::cout << "[native_h700] power script path: " << power_script_path.lexically_normal().string() << "\n";
  std::cout << "[native_h700] device model token: "
            << (device_model_token.empty() ? std::string("unknown") : device_model_token) << "\n";
  std::cout << "[native_h700] h700 joy profile: "
            << (use_h700_defaults ? (use_h700_34xx_keymap ? "34xxsp" : "other-h700") : "desktop-default")
            << "\n";

  InputManager input(keymap_path.string(), use_h700_defaults, use_h700_34xx_keymap);
  std::cout << "[native_h700] joy map: " << input.DescribeJoyMap() << "\n";
  std::cout << "[native_h700] pad map: " << input.DescribePadMap() << "\n";
  ConfigStore config(config_path.string());
  if (!config.Get().audio) {
    config.Mutable().audio = true;
    config.MarkDirty();
    config.Save();
  }
  ProgressStore progress(progress_path.string());
  RecentPathStore favorites_store(favorites_path.string(), NormalizePathKey);
  RecentPathStore history_store(history_path.string(), NormalizePathKey);
  VolumeController volume_controller(use_h700_defaults);
  SystemStatusMonitor system_status;
  SystemControlService system_control_service(use_h700_defaults);
  LidPowerController lid_power_controller(power_script_path);
  SystemSettingsState system_settings_state{};
  system_settings_state.auto_sleep_interval_index = std::clamp(config.Get().auto_sleep_interval_index, 0, 4);
  TxtSettingsState txt_settings_state{};
  txt_settings_state.background_color = ClampTxtColorIndex(config.Get().txt_background_color);
  txt_settings_state.font_color = ClampTxtColorIndex(config.Get().txt_font_color);
  txt_settings_state.font_size_level = ClampTxtFontSizeLevel(config.Get().txt_font_size_level);
  std::function<void(int)> apply_txt_font_size_level = [&](int level) {
    const int clamped = ClampTxtFontSizeLevel(level);
    if (config.Mutable().txt_font_size_level != clamped) {
      config.Mutable().txt_font_size_level = clamped;
      config.MarkDirty();
    }
    txt_settings_state.font_size_level = clamped;
  };
  ContributorAvatarState contributor_avatar_state{};
  if (use_h700_defaults) {
    bool changed = false;
    if (system_control_service.ApplyVolumePercent(config.Get().system_volume_percent, system_settings_state.levels.volume) &&
        system_settings_state.levels.volume.available) {
      const int applied_percent =
          std::clamp((system_settings_state.levels.volume.level * 100) /
                         std::max(1, system_settings_state.levels.volume.max_level),
                     0, 100);
      if (config.Mutable().system_volume_percent != applied_percent) {
        config.Mutable().system_volume_percent = applied_percent;
        changed = true;
      }
    } else {
      system_control_service.RefreshVolumeOnly(system_settings_state.levels.volume);
    }

    if (system_control_service.ApplyBrightnessLevel(config.Get().screen_brightness_level,
                                                    system_settings_state.levels.brightness) &&
        system_settings_state.levels.brightness.available) {
      const int applied_level =
          std::clamp(system_settings_state.levels.brightness.level, 0, system_settings_state.levels.brightness.max_level);
      if (config.Mutable().screen_brightness_level != applied_level) {
        config.Mutable().screen_brightness_level = applied_level;
        changed = true;
      }
    } else {
      system_control_service.Refresh(system_settings_state.levels);
    }
    if (changed || config.IsDirty()) {
      config.MarkDirty();
      config.Save();
    }
  } else {
    system_control_service.Refresh(system_settings_state.levels);
  }

  AppUiState app_ui{};
  app_ui.volume_display_percent = ClampInt((config.Get().sfx_volume * 100) / std::max(1, SDL_MIX_MAXVOLUME), 0, 100);
  if (system_settings_state.levels.volume.available) {
    app_ui.volume_display_percent = std::clamp(
        (system_settings_state.levels.volume.level * 100) / std::max(1, system_settings_state.levels.volume.max_level),
        0, 100);
  } else {
    int initial_system_volume_percent = 0;
    if (volume_controller.RefreshPercent(initial_system_volume_percent)) {
      app_ui.volume_display_percent = initial_system_volume_percent;
    }
  }
  SfxBank sfx;
  bool sfx_ready = false;
  bool sfx_init_attempted = false;
  sfx.SetVolume(config.Get().sfx_volume);
  auto ensure_sfx_ready = [&]() -> bool {
    if (sfx_ready) return true;
    if (sfx_init_attempted) return false;
    sfx_init_attempted = true;
    sfx_ready = sfx.Init(exe_path);
    if (sfx_ready) {
      sfx.SetVolume(config.Get().sfx_volume);
    }
    if (!sfx_ready) {
      std::cout << "[native_h700] sound: disabled (all audio backends failed)\n";
    }
    std::cout << "[native_h700] sound init: backend=" << sfx.BackendName()
              << " ready=" << (sfx_ready ? "1" : "0")
              << " volume=" << config.Get().sfx_volume << "\n";
    return sfx_ready;
  };
  std::cout << "[native_h700] sound: config_audio=" << (config.Get().audio ? "1" : "0")
            << " backend=" << sfx.BackendName()
            << " ready=deferred"
            << " volume=" << config.Get().sfx_volume << "\n";
  auto play_sfx = [&](SfxId id) {
    if (!config.Get().audio) return;
    ensure_sfx_ready();
    sfx.Play(id);
  };
  auto flush_deferred_writes = [&](bool force) {
    const uint32_t tick_now = SDL_GetTicks();
    if ((force || config.ShouldFlush(tick_now, kDeferredSaveDelayMs)) && config.IsDirty()) config.Save();
    if ((force || progress.ShouldFlush(tick_now, kDeferredSaveDelayMs)) && progress.IsDirty()) progress.Save();
    if ((force || favorites_store.ShouldFlush(tick_now, kDeferredSaveDelayMs)) && favorites_store.IsDirty()) favorites_store.Save();
    if ((force || history_store.ShouldFlush(tick_now, kDeferredSaveDelayMs)) && history_store.IsDirty()) history_store.Save();
  };
  PdfRuntime pdf_runtime;
  EpubRuntime epub_runtime;
  std::cout << "[native_h700] epub comic backend: " << epub_runtime.BackendName()
            << " (real_renderer=" << (epub_runtime.HasRealRenderer() ? "yes" : "no") << ")\n";
  ShelfRenderCache shelf_render_cache;
  ShelfRuntimeState shelf_runtime;
  uint64_t &shelf_content_version = shelf_runtime.content_version;

  State state = State::Boot;
  State settings_return_state = State::Shelf;
  BootRuntimeState boot_runtime;
  {
    std::error_code ec;
    const std::filesystem::path boot_status_path =
        std::filesystem::current_path(ec) / "cache" / "update_boot_status.txt";
    if (!ec) InitializeBootRuntimeReplay(boot_runtime, boot_status_path);
  }
  std::vector<BookItem> &shelf_items = shelf_runtime.items;
  std::unordered_map<std::string, CoverCacheEntry> cover_textures;
  std::string current_folder;
  std::unordered_map<std::string, int> folder_focus;
  int focus_index = 0;
  std::unordered_map<int, GridItemAnim> grid_item_anims;
  int shelf_page = 0;
  int page_anim_from = 0;
  int page_anim_to = 0;
  int page_anim_dir = 0;
  bool page_animating = false;
  animation::TweenFloat page_slide(0.0f);
  bool any_grid_animating = false;
  int title_focus_index = -1;
  float title_marquee_wait = kTitleMarqueePauseSec;
  float title_marquee_offset = 0.0f;
  bool title_marquee_active = false;

  animation::TweenFloat menu_anim(0.0f);
  animation::TweenFloat scene_flash(0.0f);
  bool menu_closing = false;
  float settings_toggle_guard = 0.0f;
  bool settings_close_armed = true;
  std::vector<SettingId> menu_items = {
      SettingId::KeyGuide,
      SettingId::SystemControls,
      SettingId::TxtToUtf8,
      SettingId::ContributorAvatars,
      SettingId::ContactMe,
      SettingId::VersionUpdate,
      SettingId::ExitApp};
  VersionUpdateState version_update_state{};
  version_update_state.current_version = DetectVersionLabel({});
  InitializeVersionUpdateState(version_update_state);
  int menu_selected = 0;
  bool lid_close_screen_off_enabled = config.Get().lid_close_screen_off;
  lid_power_controller.SetEnabled(lid_close_screen_off_enabled);
  system_settings_state.lid_close_screen_off_enabled = lid_close_screen_off_enabled;
  uint32_t last_user_input_tick = SDL_GetTicks();
  bool auto_sleep_waiting_for_input = false;
  TxtTranscodeJob txt_transcode_job{};
  ReaderUiState reader_ui{};
  std::string &current_book = reader_ui.current_book;
  ReaderProgress &reader = reader_ui.progress;
  ReaderMode &reader_mode = reader_ui.mode;
  TxtReaderState &txt_reader = reader_ui.txt_reader;
  bool &reader_progress_overlay_visible = reader_ui.progress_overlay_visible;
  float &hold_cooldown = reader_ui.hold_cooldown;
  auto &hold_speed = reader_ui.hold_speed;
  auto &long_fired = reader_ui.long_fired;
  int nav_selected_index = 0; // 0: ALL COMICS, 1: ALL BOOKS, 2: COLLECTIONS, 3: HISTORY

  auto current_category = [&]() -> ShelfCategory {
    return ClampShelfCategory(nav_selected_index);
  };
  auto file_exists_fs = [&](const std::filesystem::path &path) -> bool {
    std::error_code ec;
    return !path.empty() && std::filesystem::exists(path, ec) && !ec;
  };
  auto file_exists = [&](const std::string &path) -> bool {
    return !path.empty() && file_exists_fs(std::filesystem::path(path));
  };
  auto item_real_path = [&](const BookItem &item) -> const std::string & {
    return item.real_path.empty() ? item.path : item.real_path;
  };
  auto item_fs_path = [&](const BookItem &item) -> std::filesystem::path {
    if (!item.native_fs_path.empty()) return item.native_fs_path;
    const std::string &real_path = item_real_path(item);
    return real_path.empty() ? std::filesystem::path(item.path) : std::filesystem::path(real_path);
  };
  auto get_compatible_progress = [&](const BookItem &item) -> ReaderProgress {
    const std::string &real_path = item_real_path(item);
    if (progress.Has(real_path)) return progress.Get(real_path);
    if (!item.path.empty() && item.path != real_path && progress.Has(item.path)) {
      return progress.Get(item.path);
    }
    return ReaderProgress{};
  };
  auto make_shelf_runtime_deps = [&]() {
    return ShelfRuntimeDeps{
        NormalizePathKey,
        GetLowerExt,
        [&]() { return boot_runtime.scanned_books; },
        [&](const std::string &path) { return favorites_store.Contains(path); },
        [&](const std::string &path) { return history_store.Contains(path); },
        [&]() { return favorites_store.OrderedPaths(); },
        [&]() { return history_store.OrderedPaths(); },
        kShelfScanCacheTtlMs,
        kShelfScanCacheMaxEntries,
    };
  };

  auto rebuild_shelf_items = [&]() {
    RebuildShelfItems(shelf_runtime, current_category(), current_folder, books_roots, make_shelf_runtime_deps());
  };
  std::string transient_message;
  uint32_t transient_message_until = 0;
  auto show_transient_message = [&](const std::string &message,
                                    uint32_t duration_ms = kTransientMessageDurationMs) {
    transient_message = message;
    transient_message_until = SDL_GetTicks() + duration_ms;
  };

  auto prune_cover_cache = [&]() {
    auto cover_cache_total_bytes = [&]() -> size_t {
      size_t total = 0;
      for (const auto &kv : cover_textures) total += kv.second.bytes;
      return total;
    };
    while (cover_textures.size() > kCoverCacheMaxEntries || cover_cache_total_bytes() > kCoverCacheMaxBytes) {
      auto oldest = cover_textures.end();
      for (auto it = cover_textures.begin(); it != cover_textures.end(); ++it) {
        if (oldest == cover_textures.end() || it->second.last_use < oldest->second.last_use) oldest = it;
      }
      if (oldest == cover_textures.end()) break;
      if (oldest->second.texture && oldest->second.owned) {
        forget_texture_size(oldest->second.texture);
        SDL_DestroyTexture(oldest->second.texture);
      }
      cover_textures.erase(oldest);
    }
  };

  auto clear_cover_cache = [&]() {
    for (auto &kv : cover_textures) {
      if (kv.second.texture && kv.second.owned) {
        forget_texture_size(kv.second.texture);
        SDL_DestroyTexture(kv.second.texture);
      }
    }
    cover_textures.clear();
    ++shelf_content_version;
  };
  auto clear_directory_files = [&](const std::filesystem::path &dir_path) {
    std::error_code ec;
    if (!std::filesystem::exists(dir_path, ec) || ec) return;
    const auto opts = std::filesystem::directory_options::skip_permission_denied;
    for (std::filesystem::directory_iterator it(dir_path, opts, ec), end; it != end; it.increment(ec)) {
      if (ec) {
        ec.clear();
        continue;
      }
      std::filesystem::remove_all(it->path(), ec);
      ec.clear();
    }
  };

  auto make_cover_service_deps = [&]() {
    return CoverServiceDeps{
        renderer,
        Layout().cover_w,
        Layout().cover_h,
        kCoverAspect,
        cover_thumb_cache_dir,
        removable_cover_thumb_cache_dir,
        cover_roots,
        ui_assets.book_cover_txt,
        ui_assets.book_cover_pdf,
        NormalizePathKey,
        GetLowerExt,
        LoadSurfaceFromFile,
        LoadSurfaceFromMemory,
        CreateNormalizedCoverTexture,
        CreateTextureFromSurface,
        remember_texture_size,
    };
  };

  auto has_manual_cover_exact_or_fuzzy = [&](const BookItem &item) -> bool {
    CoverServiceDeps deps = make_cover_service_deps();
    return HasManualCoverExactOrFuzzy(item, deps);
  };

  auto has_cached_doc_cover_on_disk = [&](const std::string &doc_path) -> bool {
    CoverServiceDeps deps = make_cover_service_deps();
    return HasCachedDocCoverOnDisk(doc_path, deps);
  };

  auto create_doc_first_page_cover_texture = [&](const std::string &doc_path) -> SDL_Texture * {
    CoverServiceDeps deps = make_cover_service_deps();
    const std::string ext = GetLowerExt(doc_path);
    SDL_Texture *texture = nullptr;
    if (ext == ".pdf") texture = CreatePdfFirstPageCoverTexture(doc_path, deps);
    else if (ext == ".epub") texture = CreateEpubFirstImageCoverTextureLocal(doc_path, deps);
    if (texture) return texture;
    return nullptr;
  };

  auto get_cover_texture = [&](const BookItem &item) -> SDL_Texture * {
    const std::string &real_path = item_real_path(item);
    auto it = cover_textures.find(real_path);
    if (it != cover_textures.end()) {
      it->second.last_use = SDL_GetTicks();
      return it->second.texture;
    }

    CoverServiceDeps deps = make_cover_service_deps();
    BookItem resolved_item = item;
    resolved_item.path = real_path;
    resolved_item.real_path = real_path;
    resolved_item.native_fs_path = item_fs_path(item);
    SDL_Texture *tex = ResolveBookCoverTexture(resolved_item, current_category(), deps);

    const bool shared_ui_cover = (tex == ui_assets.book_cover_txt ||
                                  tex == ui_assets.book_cover_pdf);
    const std::string ext = item.is_dir ? std::string{} : GetLowerExt(real_path);
    const bool retryable_fallback =
        shared_ui_cover &&
        (item.is_dir || ext == ".pdf" || ext == ".epub");
    if (retryable_fallback) {
      return tex;
    }
    const bool owned = (tex != nullptr && !shared_ui_cover);
    int tw = 0;
    int th = 0;
    if (tex) get_texture_size(tex, tw, th);
    const size_t tex_bytes = (owned && tw > 0 && th > 0) ? (static_cast<size_t>(tw) * static_cast<size_t>(th) * 4u) : 0u;
    cover_textures[real_path] = CoverCacheEntry{tex, tw, th, tex_bytes, SDL_GetTicks(), owned};
    prune_cover_cache();
    return tex;
  };

#ifdef HAVE_SDL2_TTF
  UiTextCacheState ui_text_cache{};
  ui_text_cache.max_text_cache_entries = kTextCacheMaxEntries;
  int current_reader_font_pt = TxtFontPointSizeForLevel(config.Get().txt_font_size_level);
  TxtTextServiceState txt_text_service{
      {},
      txt_layout_cache_dir,
      removable_txt_layout_cache_dir,
      kTxtLayoutCacheMaxEntries,
      kTxtMaxWrappedLines,
  };

  auto clear_text_cache = [&]() {
    ClearUiTextCache(ui_text_cache, forget_texture_size);
  };

  apply_txt_font_size_level = [&](int level) {
    const int clamped = ClampTxtFontSizeLevel(level);
    if (config.Mutable().txt_font_size_level != clamped) {
      config.Mutable().txt_font_size_level = clamped;
      config.MarkDirty();
      config.Save();
    }
    txt_settings_state.font_size_level = clamped;
    current_reader_font_pt = TxtFontPointSizeForLevel(clamped);
    ShutdownUiTextCache(ui_text_cache, forget_texture_size);
  };

  std::function<void(const std::string &, bool)> persist_current_txt_resume_snapshot;

  auto get_title_ellipsized = [&](const std::string &raw_name, int text_area_w,
                                  const std::function<int(const std::string &)> &measure) -> std::string {
    return GetTitleEllipsized(ui_text_cache, raw_name, text_area_w, measure);
  };

  auto open_ui_font = [&]() {
    OpenUiFonts(ui_text_cache, exe_path, ui_path, current_reader_font_pt);
  };

  auto get_text_texture = [&](const std::string &text, SDL_Color color) -> TextCacheEntry * {
    open_ui_font();
    return GetUiTextTexture(ui_text_cache, renderer, text, color, UiTextRole::Body);
  };

  auto get_title_text_texture = [&](const std::string &text, SDL_Color color) -> TextCacheEntry * {
    open_ui_font();
    return GetUiTextTexture(ui_text_cache, renderer, text, color, UiTextRole::Title);
  };

  auto get_reader_text_texture = [&](const std::string &text, SDL_Color color) -> TextCacheEntry * {
    open_ui_font();
    return GetUiTextTexture(ui_text_cache, renderer, text, color, UiTextRole::Reader);
  };

  auto shelf_title_text = [&](const BookItem &item) -> std::string {
    if (item.is_dir) return item.name;
    try {
      const std::string stem = std::filesystem::path(item.path).stem().string();
      if (!stem.empty()) return stem;
    } catch (...) {
    }
    return item.name;
  };

  auto focused_title_needs_marquee = [&]() -> bool {
    if (state != State::Shelf) return false;
    if (focus_index < 0 || focus_index >= static_cast<int>(shelf_items.size())) return false;
    SDL_Color title_color{248, 250, 255, 255};
    const std::string display = shelf_title_text(shelf_items[focus_index]);
    if (display.empty()) return false;
    TextCacheEntry *te = get_text_texture(display, title_color);
    const int text_area_w = std::max(8, FocusedCoverW() - Layout().title_text_pad_x * 2);
    if (te && te->texture) return te->w > text_area_w;
    return static_cast<int>(display.size()) * 8 > text_area_w;
  };

#endif

  auto clear_runtime_cache_files = [&]() {
    clear_cover_cache();
    clear_directory_files(cover_thumb_cache_dir);
    clear_directory_files(removable_cover_thumb_cache_dir);
#ifdef HAVE_SDL2_TTF
    ClearTxtLayoutCache(txt_text_service);
    clear_text_cache();
#endif
    clear_directory_files(txt_layout_cache_dir);
    clear_directory_files(removable_txt_layout_cache_dir);
    std::cout << "[native_h700] runtime caches cleared: both cards cover thumbs + txt layouts/resume\n";
  };

  auto collect_scanned_txt_files = [&]() {
    std::vector<std::string> out;
    std::unordered_set<std::string> seen;
    for (const auto &root : books_roots) {
      std::error_code ec;
      if (!std::filesystem::exists(root, ec) || ec) continue;
      const auto opts = std::filesystem::directory_options::skip_permission_denied;
      for (std::filesystem::recursive_directory_iterator it(root, opts, ec), end; it != end; it.increment(ec)) {
        if (ec) {
          ec.clear();
          continue;
        }
        if (!it->is_regular_file(ec) || ec) {
          ec.clear();
          continue;
        }
        const std::string path = it->path().string();
        if (GetLowerExt(path) != ".txt") continue;
        const std::string key = NormalizePathKey(path);
        if (seen.insert(key).second) out.push_back(path);
      }
    }
    std::sort(out.begin(), out.end());
    return out;
  };

  auto start_txt_transcode_job = [&]() {
    if (txt_transcode_job.active) return;
    txt_transcode_job = TxtTranscodeJob{};
    txt_transcode_job.files = collect_scanned_txt_files();
    txt_transcode_job.total = txt_transcode_job.files.size();
    txt_transcode_job.active = txt_transcode_job.total > 0;
    txt_transcode_job.current_file.clear();
    std::cout << "[native_h700] txt transcode queued: files=" << txt_transcode_job.total << "\n";
  };

  auto process_txt_transcode_step = [&]() {
    if (!txt_transcode_job.active) return;
    if (txt_transcode_job.processed >= txt_transcode_job.total) {
      txt_transcode_job.active = false;
      txt_transcode_job.current_file.clear();
      clear_runtime_cache_files();
      std::cout << "[native_h700] txt transcode finished: processed=" << txt_transcode_job.processed
                << " converted=" << txt_transcode_job.converted
                << " failed=" << txt_transcode_job.failed << "\n";
      return;
    }

    const size_t idx = txt_transcode_job.processed;
    const std::filesystem::path file_path(txt_transcode_job.files[idx]);
    txt_transcode_job.current_file = file_path.filename().string();

    std::string raw;
    std::string utf8;
    std::string detected_encoding;
    bool success = ReadFileBytes(file_path, raw) && DecodeTextBytesToUtf8(raw, utf8, &detected_encoding);
    bool converted = false;
    if (success) {
      if (utf8 != raw) {
        success = WriteFileBytesAtomically(file_path, utf8);
        converted = success;
      }
    }
    if (!success) {
      ++txt_transcode_job.failed;
      std::cout << "[native_h700] txt transcode failed: " << file_path.string() << "\n";
    } else if (converted) {
      ++txt_transcode_job.converted;
      std::cout << "[native_h700] txt transcoded: " << file_path.string()
                << " encoding=" << detected_encoding << "\n";
    }
    ++txt_transcode_job.processed;
    if (txt_transcode_job.processed >= txt_transcode_job.total) {
      txt_transcode_job.active = false;
      txt_transcode_job.current_file.clear();
      clear_runtime_cache_files();
      std::cout << "[native_h700] txt transcode finished: processed=" << txt_transcode_job.processed
                << " converted=" << txt_transcode_job.converted
                << " failed=" << txt_transcode_job.failed << "\n";
    }
  };

  auto destroy_shelf_render_cache = [&]() {
    DestroyShelfRenderCache(shelf_render_cache, forget_texture_size);
  };

  auto invalidate_shelf_render_cache = [&]() {
    InvalidateShelfRenderCache(shelf_render_cache, forget_texture_size);
  };

  auto invalidate_all_render_cache = [&]() {};

  auto close_text_reader = [&]() {
    txt_reader = TxtReaderState{};
    reader_progress_overlay_visible = false;
    if (reader_mode == ReaderMode::Txt) {
      reader_mode = ReaderMode::None;
    }
  };

  auto clamp_text_scroll = [&]() {
    const int max_scroll = std::max(0, txt_reader.content_h - txt_reader.viewport_h);
    txt_reader.scroll_px = ClampInt(txt_reader.scroll_px, 0, max_scroll);
    if (!txt_reader.loading) {
      reader.scroll_y = txt_reader.scroll_px;
      if (!txt_reader.line_source_offsets.empty()) {
        const size_t top_line = std::min(
            txt_reader.line_source_offsets.size() - 1,
            static_cast<size_t>(std::max(0, txt_reader.scroll_px / std::max(1, txt_reader.line_h))));
        reader.scroll_x = static_cast<int>(std::min<size_t>(
            txt_reader.line_source_offsets[top_line], static_cast<size_t>(std::numeric_limits<int>::max())));
      } else {
        reader.scroll_x = 0;
      }
    }
  };

  auto get_text_viewport_bounds = [&]() -> SDL_Rect {
    open_ui_font();
    const int font_pt = TxtFontPointSizeForLevel(config.Get().txt_font_size_level);
    int font_h = font_pt + 4;
#ifdef HAVE_SDL2_TTF
    if (ui_text_cache.reader_font) font_h = TTF_FontHeight(ui_text_cache.reader_font);
#endif
    return GetTxtViewportBounds(
        renderer,
        TxtViewportRequest{
            Layout().screen_w,
            Layout().screen_h,
            Layout().txt_margin_x,
            Layout().txt_margin_y,
            font_pt,
            font_h + kTxtLineSpacing,
        });
  };

#ifdef HAVE_SDL2_TTF
  const std::string kTxtParagraphIndent = u8"閵嗏偓閵嗏偓";
  const std::string kTxtParagraphIndentAscii = "  ";

#endif

  auto append_wrapped_text_line = [&](TxtReaderState &state, const std::string &line,
                                      size_t source_line_offset) -> bool {
#ifndef HAVE_SDL2_TTF
    (void)state;
    (void)line;
    (void)source_line_offset;
    return false;
#else
    return AppendWrappedTextLine(state, line, ui_text_cache.reader_font,
                                 source_line_offset, txt_text_service.max_wrapped_lines);
#endif
  };

  auto make_txt_session_deps = [&]() {
    return TxtReaderSessionDeps{
        reader_ui,
        txt_text_service.layout_cache,
        open_ui_font,
        [&]() -> bool { return ui_text_cache.reader_font != nullptr; },
        [&]() -> int {
#ifdef HAVE_SDL2_TTF
          return ui_text_cache.reader_font ? TTF_FontHeight(ui_text_cache.reader_font) : 0;
#else
          return 0;
#endif
        },
        get_text_viewport_bounds,
        [&](const std::string &path, const SDL_Rect &bounds, int line_h, uintmax_t file_size, long long file_mtime) {
          return MakeTxtLayoutCacheKey(path, bounds, line_h, file_size, file_mtime, NormalizePathKey) +
                 "|v" + std::to_string(kTxtLayoutCacheVersion) +
                 "|bytes=" + std::to_string(kTxtMaxBytes) +
                 "|lines=" + std::to_string(kTxtMaxWrappedLines);
        },
        [&](const std::string &cache_key, const std::string &book_path, TxtLayoutCacheEntry &entry) {
          return LoadTxtLayoutCacheFromDisk(txt_text_service, cache_key, book_path, entry);
        },
        [&](const std::string &cache_key, const std::string &book_path, const TxtLayoutCacheEntry &entry) {
          SaveTxtLayoutCacheToDisk(txt_text_service, cache_key, book_path, entry);
        },
        [&](const std::string &cache_key, const std::string &book_path, TxtResumeCacheEntry &entry) {
          return LoadTxtResumeCacheFromDisk(txt_text_service, cache_key, book_path, entry);
        },
        [&](const std::string &cache_key, const std::string &book_path, const TxtReaderState &state) {
          SaveTxtResumeCacheToDisk(txt_text_service, cache_key, book_path, state);
        },
        [&]() { PruneTxtLayoutCache(txt_text_service); },
        [&](const std::string &raw, std::string &out) { return DecodeTextBytesToUtf8(raw, out); },
        append_wrapped_text_line,
        invalidate_all_render_cache,
        clamp_text_scroll,
        kTxtLineSpacing,
        kTxtMaxBytes,
        kTxtResumeSaveDelayMs,
    };
  };

  persist_current_txt_resume_snapshot = [&](const std::string &book_path, bool force) {
    auto deps = make_txt_session_deps();
    PersistCurrentTxtResumeSnapshot(book_path, force, deps);
  };

  auto open_text_book = [&](const std::string &path) -> bool {
#ifndef HAVE_SDL2_TTF
    (void)path;
    std::cerr << "[reader] txt reader requires SDL_ttf build support.\n";
    return false;
#else
    auto deps = make_txt_session_deps();
    return OpenTextBookSession(path, deps);
#endif
  };

  auto text_scroll_by = [&](int delta_px) {
    auto deps = make_txt_session_deps();
    TextScrollBy(delta_px, current_book, deps);
  };

  auto text_page_by = [&](int dir) {
    auto deps = make_txt_session_deps();
    TextPageBy(dir, current_book, deps);
  };

  auto text_jump_to_percent = [&](int pct) {
    auto deps = make_txt_session_deps();
    TextJumpToPercent(pct, current_book, deps);
  };

  auto page_progress_pct_for_index = [&](int page_index, int page_count) -> int {
    if (page_count <= 1) return 100;
    return ClampInt(static_cast<int>((static_cast<int64_t>(ClampInt(page_index, 0, page_count - 1)) * 100) /
                                     (page_count - 1)),
                    0, 100);
  };

  auto page_index_for_percent = [&](int current_page, int target_pct, int page_count) -> int {
    if (page_count <= 1) return 0;
    const int clamped_current = ClampInt(current_page, 0, page_count - 1);
    const int clamped_target = ClampInt(target_pct, 0, 100);
    const int current_pct = page_progress_pct_for_index(clamped_current, page_count);
    if (clamped_target == current_pct) return clamped_current;

    if (clamped_target > current_pct) {
      for (int page = clamped_current + 1; page < page_count; ++page) {
        if (page_progress_pct_for_index(page, page_count) >= clamped_target) {
          return page;
        }
      }
      return page_count - 1;
    }

    for (int page = clamped_current - 1; page >= 0; --page) {
      if (page_progress_pct_for_index(page, page_count) <= clamped_target) {
        return page;
      }
    }
    return 0;
  };

  auto reader_jump_to_percent = [&](int pct) {
    if (reader_mode == ReaderMode::Txt && txt_reader.open) {
      text_jump_to_percent(pct);
      return;
    }
    if (reader_mode == ReaderMode::Pdf && pdf_runtime.IsOpen()) {
      pdf_runtime.SetPage(page_index_for_percent(pdf_runtime.CurrentPage(),
                                                pct,
                                                std::max(1, pdf_runtime.PageCount())));
      return;
    }
    if (reader_mode == ReaderMode::Epub && epub_runtime.IsOpen()) {
      epub_runtime.SetPage(page_index_for_percent(epub_runtime.CurrentPage(),
                                                 pct,
                                                 std::max(1, epub_runtime.PageCount())));
    }
  };

  auto current_reader_progress_pct = [&]() -> int {
    if (reader_mode == ReaderMode::Txt && txt_reader.open) {
      return TxtReaderProgressPercent(txt_reader);
    }
    if (reader_mode == ReaderMode::Pdf && pdf_runtime.IsOpen()) {
      const int page_count = std::max(1, pdf_runtime.PageCount());
      const int page_idx = ClampInt(pdf_runtime.Progress().page, 0, page_count - 1);
      return (page_count <= 1)
                 ? 100
                 : ClampInt(static_cast<int>((static_cast<int64_t>(page_idx) * 100) / (page_count - 1)), 0, 100);
    }
    if (reader_mode == ReaderMode::Epub && epub_runtime.IsOpen()) {
      const int page_count = std::max(1, epub_runtime.PageCount());
      const int page_idx = ClampInt(epub_runtime.Progress().page, 0, page_count - 1);
      return (page_count <= 1)
                 ? 100
                 : ClampInt(static_cast<int>((static_cast<int64_t>(page_idx) * 100) / (page_count - 1)), 0, 100);
    }
    return 0;
  };

  auto current_txt_layout_progress_pct = [&]() -> int {
    if (!(reader_mode == ReaderMode::Txt && txt_reader.open)) return 0;
    if (!txt_reader.loading) return 100;
    const size_t total = txt_reader.pending_raw.size();
    if (total == 0) return 0;
    return ClampInt(static_cast<int>((static_cast<int64_t>(txt_reader.parse_pos) * 100) / total), 0, 100);
  };

  bool running = true;
  uint32_t prev_ticks = SDL_GetTicks();
  while (running) {
    const uint32_t frame_begin_ticks = SDL_GetTicks();
    uint32_t now = SDL_GetTicks();
    float dt = std::max(0.0f, (now - prev_ticks) / 1000.0f);
    prev_ticks = now;

    hold_cooldown = std::max(0.0f, hold_cooldown - dt);
    settings_toggle_guard = std::max(0.0f, settings_toggle_guard - dt);
    TickAppUiState(app_ui, dt);
    TickVersionUpdateState(version_update_state, dt);

    input.BeginFrame(dt);
    SDL_Event e;
    const bool animate_enabled = config.Get().animations;
    const bool contributor_marquee_active =
        state == State::Settings &&
        !menu_items.empty() &&
        menu_items[std::clamp(menu_selected, 0, static_cast<int>(menu_items.size()) - 1)] ==
            SettingId::ContributorAvatars &&
        !contributor_avatar_entries.empty();
    const bool version_update_download_active =
        state == State::Settings &&
        !menu_items.empty() &&
        menu_items[std::clamp(menu_selected, 0, static_cast<int>(menu_items.size()) - 1)] ==
            SettingId::VersionUpdate &&
        version_update_state.download_in_progress;
    const bool has_active_animation =
        state == State::Boot || input.AnyPressed() ||
        txt_transcode_job.active ||
        (reader_mode == ReaderMode::Txt && txt_reader.open && txt_reader.loading) ||
        version_update_download_active ||
        contributor_marquee_active ||
        (animate_enabled && (menu_anim.IsAnimating() || scene_flash.IsAnimating() || page_animating || any_grid_animating));
    const bool needs_periodic_tick =
        (state == State::Shelf && title_marquee_active) ||
        (state == State::Reader &&
         ((reader_mode == ReaderMode::Pdf && pdf_runtime.IsRenderPending()) ||
          (reader_mode == ReaderMode::Epub && epub_runtime.IsRenderPending())));
    const uint32_t loop_now = SDL_GetTicks();
    const bool has_pending_flush =
        config.ShouldFlush(loop_now, kDeferredSaveDelayMs) ||
        progress.ShouldFlush(loop_now, kDeferredSaveDelayMs) ||
        favorites_store.ShouldFlush(loop_now, kDeferredSaveDelayMs) ||
        history_store.ShouldFlush(loop_now, kDeferredSaveDelayMs);
    const int idle_wait_ms = (!has_active_animation && has_pending_flush && !needs_periodic_tick) ? static_cast<int>(kIdleFlushOnlyWaitMs) : kIdleWaitMs;
    auto is_user_input_event = [](const SDL_Event &event) {
      switch (event.type) {
      case SDL_KEYDOWN:
        return event.key.repeat == 0;
      case SDL_CONTROLLERBUTTONDOWN:
      case SDL_JOYBUTTONDOWN:
        return true;
      case SDL_JOYHATMOTION:
        return event.jhat.value != SDL_HAT_CENTERED;
      default:
        return false;
      }
    };
    auto note_user_input = [&](const SDL_Event &event) {
      if (!is_user_input_event(event)) return;
      last_user_input_tick = SDL_GetTicks();
      auto_sleep_waiting_for_input = false;
    };
    auto maybe_trigger_auto_sleep = [&]() {
      if (!use_h700_defaults || !config.Get().lid_close_screen_off || auto_sleep_waiting_for_input) return;
      const uint32_t now = SDL_GetTicks();
      const uint32_t idle_ms = AutoSleepIntervalMsFromIndex(system_settings_state.auto_sleep_interval_index);
      if (now - last_user_input_tick < idle_ms) return;
      if (lid_power_controller.TriggerAutoIfEnabled()) {
        auto_sleep_waiting_for_input = true;
      }
    };
    if (has_active_animation) {
      while (SDL_PollEvent(&e)) {
        if (e.type == SDL_QUIT) running = false;
        note_user_input(e);
        input.HandleEvent(e);
      }
      maybe_trigger_auto_sleep();
    } else {
      if (SDL_WaitEventTimeout(&e, idle_wait_ms)) {
        if (e.type == SDL_QUIT) running = false;
        note_user_input(e);
        input.HandleEvent(e);
        while (SDL_PollEvent(&e)) {
          if (e.type == SDL_QUIT) running = false;
          note_user_input(e);
          input.HandleEvent(e);
        }
        maybe_trigger_auto_sleep();
      } else {
        maybe_trigger_auto_sleep();
        system_status.Poll(SDL_GetTicks());
        if (has_pending_flush && !needs_periodic_tick) {
        // Wake only to flush deferred IO; keep the current frame untouched.
          input.EndFrame();
          flush_deferred_writes(false);
          prev_ticks = SDL_GetTicks();
          continue;
        }
        if (!needs_periodic_tick && !has_pending_flush) {
        // Fully idle: no input, no animation, no incremental loading.
        // Skip update/render work and keep sleeping until something changes.
          prev_ticks = SDL_GetTicks();
          input.EndFrame();
          continue;
        }
      }
    }
    input.EndFrame();

    system_status.Poll(now);

    if (reader_mode == ReaderMode::Txt && txt_reader.open && txt_reader.loading) {
      auto deps = make_txt_session_deps();
      TickTextBookSession(current_book, deps, 5, 24576);
    }
    process_txt_transcode_step();
    flush_deferred_writes(false);
    HandleVolumeControls(
        app_ui, input, now, volume_controller, config,
        [&](int volume) { sfx.SetVolume(volume); },
        [&]() { play_sfx(SfxId::Change); });

    if (state == State::Shelf || state == State::Settings) {
      if (input.IsJustPressed(Button::Up) || input.IsJustPressed(Button::Down) ||
          input.IsJustPressed(Button::Left) || input.IsJustPressed(Button::Right)) {
        play_sfx(SfxId::Move);
      } else if (input.IsJustPressed(Button::B) || input.IsJustPressed(Button::Y)) {
        play_sfx(SfxId::Back);
      } else if (input.IsJustPressed(Button::A) || input.IsJustPressed(Button::X)) {
        play_sfx(SfxId::Select);
      } else if (input.IsJustPressed(Button::L1) || input.IsJustPressed(Button::L2) ||
                 input.IsJustPressed(Button::R1) || input.IsJustPressed(Button::R2)) {
        play_sfx(SfxId::Change);
      }
    }

    if (input.IsPressed(Button::Start) && input.IsPressed(Button::Select)) running = false;

    // Dedicated settings toggle path (single entry for Start / Select mapping).
    const MenuToggleAction menu_toggle_action =
        HandleMenuToggleInput(app_ui, input, state == State::Settings, state == State::Shelf,
                              state == State::Reader, settings_close_armed, settings_toggle_guard, menu_closing,
                              kMenuToggleDebounceSec);
    if (menu_toggle_action == MenuToggleAction::CloseSettings) {
      const NativeConfig &ui_cfg = config.Get();
      if (ui_cfg.animations) menu_anim.AnimateTo(0.0f, 0.16f, animation::Ease::InOutCubic);
      else menu_anim.Snap(0.0f);
      menu_closing = true;
      play_sfx(SfxId::Back);
    } else if (menu_toggle_action == MenuToggleAction::OpenFromShelf ||
               menu_toggle_action == MenuToggleAction::OpenFromReader) {
      settings_return_state = (menu_toggle_action == MenuToggleAction::OpenFromShelf) ? State::Shelf : State::Reader;
      state = State::Settings;
      menu_anim.Snap(0.0f);
      if (config.Get().animations) menu_anim.AnimateTo(1.0f, 0.20f, animation::Ease::OutCubic);
      else menu_anim.Snap(1.0f);
      settings_toggle_guard = kSettingsToggleGuardSec;
      settings_close_armed = false;
      menu_closing = false;
      play_sfx(SfxId::Back);
    }

    if (state == State::Boot) {
      BootRuntimeTickDeps boot_tick_deps{
          books_roots,
          kBootCountBatchEntries,
          kBootScanBatchEntries,
          kBootCoverGenerateBatchEntries,
          GetLowerExt,
          [&](const std::string &doc_path) {
            const std::string ext = GetLowerExt(doc_path);
            if (ext == ".pdf") return pdf_runtime.HasRealRenderer();
            if (ext == ".epub") return epub_runtime.HasRealRenderer();
            return false;
          },
          has_manual_cover_exact_or_fuzzy,
          has_cached_doc_cover_on_disk,
          create_doc_first_page_cover_texture,
          [&](SDL_Texture *generated) {
            forget_texture_size(generated);
            SDL_DestroyTexture(generated);
          },
          [&](size_t total_books, size_t cover_generate_count) {
            current_folder.clear();
            nav_selected_index = 0;
            rebuild_shelf_items();
            focus_index = 0;
            shelf_page = 0;
            page_animating = false;
            page_slide.Snap(0.0f);
            grid_item_anims.clear();
            title_focus_index = -1;
            title_marquee_active = false;
            title_marquee_offset = 0.0f;
            title_marquee_wait = kTitleMarqueePauseSec;
            std::cout << "[native_h700] boot scan complete: books=" << total_books
                      << " cover_generate=" << cover_generate_count << "\n";
            state = State::Shelf;
          },
      };
      TickBootRuntime(boot_runtime, dt, boot_tick_deps);
    } else if (state == State::Shelf) {
      const NativeConfig &ui_cfg = config.Get();
      ShelfRuntimeInputDeps shelf_input_deps{
          input,
          shelf_runtime,
          folder_focus,
          current_folder,
          focus_index,
          shelf_page,
          nav_selected_index,
          kGridCols,
          dt,
          ui_cfg.animations,
          page_animating,
          page_anim_from,
          page_anim_to,
          page_anim_dir,
          title_focus_index,
          title_marquee_active,
          title_marquee_wait,
          title_marquee_offset,
          kTitleMarqueePauseSec,
          kTitleMarqueeSpeedPx,
          grid_item_anims,
          focused_title_needs_marquee,
          clear_cover_cache,
          rebuild_shelf_items,
          [&]() { page_slide.Snap(0.0f); },
          [&]() { page_slide.AnimateTo(1.0f, kPageSlideDurationSec, animation::Ease::OutCubic); },
          [&](const std::string &path) { favorites_store.Add(path); },
          [&](const std::string &path) { favorites_store.Remove(path); },
          current_category,
          [&](const BookItem &item) {
            const std::string &real_path = item_real_path(item);
            const std::filesystem::path real_fs_path = item_fs_path(item);
            const std::string ext = GetLowerExt(real_path);
            const std::string open_path = real_fs_path.empty() ? real_path : real_fs_path.string();
            reader = get_compatible_progress(item);
            ReaderOpenDeps open_deps{
                renderer,
                Layout().screen_w,
                Layout().screen_h,
                reader_ui,
                pdf_runtime,
                epub_runtime,
                open_text_book,
                close_text_reader,
                file_exists,
            };
            const bool opened = OpenReaderSession(open_path, ext, open_deps);
            if (opened) {
              history_store.Add(open_path);
              reader_ui.current_book = open_path;
              state = State::Reader;
              scene_flash.Snap(kSceneFadeFlashAlpha);
              scene_flash.AnimateTo(0.0f, kSceneFadeFlashDurationSec, animation::Ease::OutCubic);
            } else {
              show_transient_message(u8"鏈壘鍒版枃浠?");
              state = State::Shelf;
            }
            return opened;
          },
      };
      HandleShelfInput(shelf_input_deps);
    } else if (state == State::Settings) {
      const NativeConfig &ui_cfg = config.Get();
      if (!menu_items.empty() &&
          menu_items[std::clamp(menu_selected, 0, static_cast<int>(menu_items.size()) - 1)] == SettingId::SystemControls) {
        system_control_service.Refresh(system_settings_state.levels);
      }
      SyncContributorAvatarState(contributor_avatar_state, contributor_avatar_entries.size());
      SettingsRuntimeInputDeps settings_input_deps{
          input,
          ui_cfg,
          dt,
          menu_closing,
          settings_close_armed,
          settings_toggle_guard,
          menu_selected,
          menu_items,
          menu_anim,
          system_settings_state,
          SystemSettingsCallbacks{
              [&](SystemControlLevels &levels) {
                system_control_service.Refresh(levels);
              },
              [&](int delta, SystemControlLevels &levels) {
                const bool ok = system_control_service.AdjustVolume(delta, levels);
                if (ok && levels.volume.available) {
                  const int saved_percent =
                      std::clamp((levels.volume.level * 100) / std::max(1, levels.volume.max_level), 0, 100);
                  if (config.Mutable().system_volume_percent != saved_percent) {
                    config.Mutable().system_volume_percent = saved_percent;
                    config.MarkDirty();
                  }
                }
                return ok;
              },
              [&](int delta, SystemControlLevels &levels) {
                const bool ok = system_control_service.AdjustBrightness(delta, levels);
                if (ok && levels.brightness.available) {
                  const int saved_level =
                      std::clamp(levels.brightness.level, 0, std::max(1, levels.brightness.max_level));
                  if (config.Mutable().screen_brightness_level != saved_level) {
                    config.Mutable().screen_brightness_level = saved_level;
                    config.MarkDirty();
                  }
                }
                return ok;
              },
              [&](SystemSettingsState &settings_state) {
                settings_state.lid_close_screen_off_enabled = config.Get().lid_close_screen_off;
                settings_state.auto_sleep_interval_index = std::clamp(config.Get().auto_sleep_interval_index, 0, 4);
              },
              [&](bool enabled, SystemSettingsState &settings_state) {
                NativeConfig &cfg = config.Mutable();
                cfg.lid_close_screen_off = enabled;
                config.MarkDirty();
                lid_power_controller.SetEnabled(enabled);
                settings_state.lid_close_screen_off_enabled = enabled;
                last_user_input_tick = SDL_GetTicks();
                auto_sleep_waiting_for_input = false;
                return true;
              },
              [&](int delta, SystemSettingsState &settings_state) {
                const int next_index =
                    ClampAutoSleepIntervalIndex(settings_state.auto_sleep_interval_index + delta);
                if (next_index == settings_state.auto_sleep_interval_index) return false;
                config.Mutable().auto_sleep_interval_index = next_index;
                config.MarkDirty();
                config.Save();
                settings_state.auto_sleep_interval_index = next_index;
                last_user_input_tick = SDL_GetTicks();
                auto_sleep_waiting_for_input = false;
                return true;
              },
              [&]() {
                clear_runtime_cache_files();
                return true;
              },
              [&]() {
                history_store.Clear();
                if (current_category() == ShelfCategory::History) {
                  current_folder.clear();
                  clear_cover_cache();
                  rebuild_shelf_items();
                  focus_index = 0;
                  shelf_page = 0;
                  page_animating = false;
                  page_slide.Snap(0.0f);
                  grid_item_anims.clear();
                }
                return true;
              },
          },
          txt_settings_state,
          TxtSettingsCallbacks{
              [&](TxtSettingsState &settings_state) {
                settings_state.background_color = ClampTxtColorIndex(config.Get().txt_background_color);
                settings_state.font_color = ClampTxtColorIndex(config.Get().txt_font_color);
                settings_state.font_size_level = ClampTxtFontSizeLevel(config.Get().txt_font_size_level);
                if (settings_state.selected_row == 0) settings_state.selected_option = settings_state.background_color;
                else if (settings_state.selected_row == 1) settings_state.selected_option = settings_state.font_color;
                else if (settings_state.selected_row == 2) settings_state.selected_option = 1;
                else settings_state.selected_option = 0;
              },
              [&](int color_index, TxtSettingsState &settings_state) {
                const int clamped = ClampTxtColorIndex(color_index);
                config.Mutable().txt_background_color = clamped;
                config.MarkDirty();
                config.Save();
                settings_state.background_color = clamped;
                settings_state.selected_option = clamped;
                return true;
              },
              [&](int color_index, TxtSettingsState &settings_state) {
                const int clamped = ClampTxtColorIndex(color_index);
                config.Mutable().txt_font_color = clamped;
                config.MarkDirty();
                config.Save();
                settings_state.font_color = clamped;
                settings_state.selected_option = clamped;
                return true;
              },
              [&](int delta, TxtSettingsState &settings_state) {
                const int next_level = ClampTxtFontSizeLevel(settings_state.font_size_level + delta);
                if (next_level == settings_state.font_size_level) return false;
                apply_txt_font_size_level(next_level);
                if (reader_mode == ReaderMode::Txt && txt_reader.open && !current_book.empty()) {
                  if (!txt_reader.line_source_offsets.empty()) {
                    const size_t top_line = std::min(
                        txt_reader.line_source_offsets.size() - 1,
                        static_cast<size_t>(std::max(0, txt_reader.scroll_px / std::max(1, txt_reader.line_h))));
                    reader.scroll_x = static_cast<int>(std::min<size_t>(
                        txt_reader.line_source_offsets[top_line], static_cast<size_t>(std::numeric_limits<int>::max())));
                  } else {
                    reader.scroll_x = 0;
                  }
                  reader.page = (txt_reader.line_h > 0) ? (txt_reader.scroll_px / txt_reader.line_h) : 0;
                  reader.scroll_y = txt_reader.scroll_px;
                  open_text_book(current_book);
                }
                settings_state.font_size_level = next_level;
                settings_state.selected_option = delta < 0 ? 0 : 1;
                return true;
              },
              [&]() {
                start_txt_transcode_job();
                return true;
              },
          },
          contributor_avatar_state,
          contributor_avatar_entries.size(),
          [&](int selected_index) { update_selected_avatar_badge_texture(selected_index); },
          version_update_state,
          VersionUpdateCallbacks{
              [&](VersionUpdateState &update_state) { BeginVersionUpdateDownload(update_state); },
          },
          false,
          [&]() { state = settings_return_state; },
          [&]() { running = false; },
          [&]() {
            history_store.Clear();
            if (current_category() == ShelfCategory::History) {
              current_folder.clear();
              clear_cover_cache();
              rebuild_shelf_items();
              focus_index = 0;
              shelf_page = 0;
              page_animating = false;
              page_slide.Snap(0.0f);
              grid_item_anims.clear();
            }
          },
          clear_runtime_cache_files,
          start_txt_transcode_job,
      };
      HandleSettingsInput(settings_input_deps);
    } else if (state == State::Reader) {
      if (input.IsJustPressed(Button::B)) {
        ReaderCloseDeps close_deps{
            reader_ui,
            progress,
            pdf_runtime,
            epub_runtime,
            close_text_reader,
            persist_current_txt_resume_snapshot,
        };
        CloseReaderSession(close_deps);
        state = State::Shelf;
        scene_flash.Snap(kSceneFadeFlashAlpha);
        scene_flash.AnimateTo(0.0f, kSceneFadeFlashDurationSec, animation::Ease::OutCubic);
      } else {
        if (input.IsJustPressed(Button::X)) {
          reader_progress_overlay_visible = !reader_progress_overlay_visible;
          if (reader_progress_overlay_visible) {
            const int pct = current_reader_progress_pct();
            reader_ui.progress_overlay_preview_pct = pct;
            reader_ui.progress_overlay_preview_pct_f = static_cast<float>(pct);
            reader_ui.progress_overlay_dirty = false;
            reader_ui.progress_overlay_scrubbing = false;
          } else {
            reader_ui.progress_overlay_dirty = false;
            reader_ui.progress_overlay_scrubbing = false;
          }
        }
        if (reader_progress_overlay_visible &&
            ((reader_mode == ReaderMode::Txt && txt_reader.open) ||
             (reader_mode == ReaderMode::Pdf && pdf_runtime.IsOpen()) ||
             (reader_mode == ReaderMode::Epub && epub_runtime.IsOpen()))) {
          TxtProgressOverlayInputDeps txt_overlay_input_deps{
              input,
              reader_ui,
              dt,
              current_reader_progress_pct(),
              !(reader_mode == ReaderMode::Txt && txt_reader.open && txt_reader.loading),
              kTxtProgressOverlayTapStepPct,
              kTxtProgressOverlayHoldDelaySec,
              kTxtProgressOverlayHoldSpeedMinPct,
              kTxtProgressOverlayHoldSpeedMaxPct,
              kTxtProgressOverlayHoldAccelPct,
              reader_jump_to_percent,
          };
          HandleTxtProgressOverlayInput(txt_overlay_input_deps);
        } else if (reader_mode == ReaderMode::Txt && txt_reader.open) {
          {
            TxtReaderInputDeps txt_input_deps{
                input,
                reader_ui,
                dt,
                kReaderTapStepPx,
                text_scroll_by,
                text_page_by,
            };
            HandleTxtReaderInput(txt_input_deps);
          }
        } else if (reader_mode == ReaderMode::Pdf) {
          const int pdf_rotation = pdf_runtime.Progress().rotation;
          if (input.IsJustPressed(Button::L2)) {
            pdf_runtime.RotateLeft();
          }
          if (input.IsJustPressed(Button::R2)) {
            pdf_runtime.RotateRight();
          }
          if (input.IsJustPressed(Button::L1)) {
            pdf_runtime.ZoomOut();
          }
          if (input.IsJustPressed(Button::R1)) {
            pdf_runtime.ZoomIn();
          }
          if (input.IsJustPressed(Button::A)) {
            pdf_runtime.ResetView();
          }

          std::array<Button, 4> dirs = {Button::Up, Button::Down, Button::Left, Button::Right};
          for (Button b : dirs) {
            int bi = static_cast<int>(b);
            int long_dir = PdfScrollDirForButton(pdf_rotation, b);
            if (long_dir == 0) {
              hold_speed[bi] = 0.0f;
              continue;
            }
            if (input.IsPressed(b) && input.HoldTime(b) >= 0.28f) {
              long_fired[bi] = true;
              pdf_runtime.ScrollByPixels(long_dir * 20);
            } else if (!input.IsPressed(b)) {
              hold_speed[bi] = 0.0f;
            }
          }

          for (Button b : dirs) {
            int bi = static_cast<int>(b);
            if (!input.IsJustReleased(b)) continue;
            if (long_fired[bi]) {
              long_fired[bi] = false;
              continue;
            }
            const int tap_dir = PdfScrollDirForButton(pdf_rotation, b);
            if (tap_dir != 0) {
              pdf_runtime.ScrollByPixels(tap_dir * 60);
            } else {
              const int page_action = PdfTapPageActionForButton(pdf_rotation, b);
              if (page_action != 0) {
                pdf_runtime.JumpByScreen(page_action);
              }
            }
          }
        } else {
          const int epub_rotation = epub_runtime.Progress().rotation;
          auto epub_scroll_dir_for_button = [&](Button button) -> int {
            if (epub_rotation == 0) {
              if (button == Button::Down) return 1;
              if (button == Button::Up) return -1;
            } else if (epub_rotation == 90) {
              if (button == Button::Left) return 1;
              if (button == Button::Right) return -1;
            } else if (epub_rotation == 180) {
              if (button == Button::Up) return 1;
              if (button == Button::Down) return -1;
            } else {
              if (button == Button::Left) return -1;
              if (button == Button::Right) return 1;
            }
            return 0;
          };
          auto epub_tap_page_action_for_button = [&](Button button) -> int {
            if (epub_rotation == 0) {
              if (button == Button::Right) return 1;
              if (button == Button::Left) return -1;
            } else if (epub_rotation == 90) {
              if (button == Button::Up) return -1;
              if (button == Button::Down) return 1;
            } else if (epub_rotation == 180) {
              if (button == Button::Left) return 1;
              if (button == Button::Right) return -1;
            } else {
              if (button == Button::Up) return 1;
              if (button == Button::Down) return -1;
            }
            return 0;
          };
          if (input.IsJustPressed(Button::L2)) {
            epub_runtime.RotateLeft();
          }
          if (input.IsJustPressed(Button::R2)) {
            epub_runtime.RotateRight();
          }
          if (input.IsJustPressed(Button::L1)) {
            epub_runtime.ZoomOut();
          }
          if (input.IsJustPressed(Button::R1)) {
            epub_runtime.ZoomIn();
          }
          if (input.IsJustPressed(Button::A)) {
            epub_runtime.ResetView();
          }

          std::array<Button, 4> dirs = {Button::Up, Button::Down, Button::Left, Button::Right};
          for (Button b : dirs) {
            int bi = static_cast<int>(b);
            int long_dir = epub_scroll_dir_for_button(b);
            if (long_dir == 0) {
              hold_speed[bi] = 0.0f;
              continue;
            }
            if (input.IsPressed(b) && input.HoldTime(b) >= 0.28f) {
              long_fired[bi] = true;
              epub_runtime.ScrollByPixels(long_dir * 20);
            } else if (!input.IsPressed(b)) {
              hold_speed[bi] = 0.0f;
            }
          }

          for (Button b : dirs) {
            int bi = static_cast<int>(b);
            if (!input.IsJustReleased(b)) continue;
            if (long_fired[bi]) {
              long_fired[bi] = false;
              continue;
            }
            const int tap_dir = epub_scroll_dir_for_button(b);
            if (tap_dir != 0) {
              epub_runtime.ScrollByPixels(tap_dir * 60);
            } else {
              const int page_action = epub_tap_page_action_for_button(b);
              if (page_action != 0) {
                epub_runtime.JumpByScreen(page_action);
              }
            }
          }
        }
      }
    }

    any_grid_animating = false;
    if (animate_enabled) {
      scene_flash.Update(dt);
      page_slide.Update(dt);
      if (page_animating && !page_slide.IsAnimating() && page_slide.Value() >= 0.999f) {
        page_animating = false;
        page_slide.Snap(0.0f);
      }
    } else {
      page_animating = false;
      page_slide.Snap(0.0f);
      scene_flash.Snap(0.0f);
      if (state != State::Settings) menu_anim.Snap(0.0f);
    }

    // Draw
    SDL_SetRenderDrawColor(renderer, 26, 27, 31, 255);
    SDL_RenderClear(renderer);

    if (state == State::Boot) {
      BootRuntimeRenderDeps boot_render_deps{
          renderer,
          boot_runtime,
          Layout().screen_w,
          Layout().screen_h,
          [&](int x, int y, int w, int h, SDL_Color c, bool filled) { DrawRect(renderer, x, y, w, h, c, filled); },
          get_text_texture,
      };
      DrawBootRuntime(boot_render_deps);
    } else {
      const NativeConfig &cfg = config.Get();
      const SDL_Color bg = (cfg.theme == 0) ? SDL_Color{22, 23, 29, 255} : SDL_Color{238, 237, 233, 255};
      DrawRect(renderer, 0, 0, Layout().screen_w, Layout().screen_h, bg);

      std::function<void()> draw_volume_overlay = []() {};
      std::function<void()> draw_system_status_overlay = []() {};
      if (state == State::Shelf || state == State::Settings) {
        draw_volume_overlay = [&]() {
#ifdef HAVE_SDL2_TTF
          if (now > app_ui.volume_display_until) return;
          SDL_Color volume_text{238, 242, 250, 255};
          const std::string label = std::to_string(app_ui.volume_display_percent);
          TextCacheEntry *te = get_text_texture(label, volume_text);
          if (!te || !te->texture) return;
          const int tx = 18;
          const int ty = Layout().top_bar_y + std::max(0, (Layout().top_bar_h - te->h) / 2);
          SDL_Rect td{tx, ty, te->w, te->h};
          SDL_RenderCopy(renderer, te->texture, nullptr, &td);
#endif
        };
          draw_system_status_overlay = [&]() {
#ifdef HAVE_SDL2_TTF
          const SystemStatusSnapshot &status = system_status.Snapshot();
          SDL_Color text_color{238, 242, 250, 255};
          SDL_Color outline_color{238, 242, 250, 255};
          SDL_Color fill_color = status.charging ? SDL_Color{104, 214, 141, 255} : SDL_Color{238, 242, 250, 255};
          if (config.Get().theme != 0) {
            text_color = SDL_Color{58, 64, 76, 255};
            outline_color = SDL_Color{58, 64, 76, 255};
            fill_color = status.charging ? SDL_Color{76, 170, 98, 255} : SDL_Color{58, 64, 76, 255};
          }

            const int center_y = Layout().top_bar_y + Layout().top_bar_h / 2;
            const int battery_shift_y = 3;
            const int battery_shift_x = (Layout().screen_w <= 640) ? -80 : 0;
            const int battery_icon_x = 552 + battery_shift_x;
            const int battery_text_x = 587 + battery_shift_x;
            const int clock_shift_x = 40;
            const int clock_shift_y = 3;
            int clock_right = Layout().screen_w - 16 - clock_shift_x;

          if (!status.clock_text.empty()) {
            TextCacheEntry *clock_tex = get_text_texture(status.clock_text, text_color);
            if (clock_tex && clock_tex->texture) {
              const int clock_x = clock_right - clock_tex->w;
              const int clock_y = center_y - clock_tex->h / 2 + clock_shift_y;
              SDL_Rect td{clock_x, clock_y, clock_tex->w, clock_tex->h};
              SDL_RenderCopy(renderer, clock_tex->texture, nullptr, &td);
            }
          }

          const int avatar_badge_size = 28;
          const int avatar_badge_x = Layout().screen_w - 12 - avatar_badge_size;
          const int avatar_badge_y = 4;
          DrawRect(renderer, avatar_badge_x, avatar_badge_y, avatar_badge_size, avatar_badge_size,
                   SDL_Color{26, 32, 42, 220}, true);
          DrawRect(renderer, avatar_badge_x, avatar_badge_y, avatar_badge_size, avatar_badge_size,
                   SDL_Color{152, 185, 210, 235}, false);
          if (selected_avatar_badge_texture) {
            SDL_Rect avatar_dst{avatar_badge_x, avatar_badge_y, avatar_badge_size, avatar_badge_size};
            SDL_RenderCopy(renderer, selected_avatar_badge_texture, nullptr, &avatar_dst);
          }

          if (status.battery_available) {
            const std::string battery_text = std::to_string(status.battery_percent) + "%";
            TextCacheEntry *battery_tex = get_text_texture(battery_text, text_color);
            int battery_text_w = battery_tex ? battery_tex->w : 0;
            int battery_text_h = battery_tex ? battery_tex->h : 0;

            const int cap_w = 4;
            const int cap_h = 8;
            const int body_w = 24;
            const int body_h = 12;
            const int icon_x = battery_icon_x;
            const int icon_y = center_y - body_h / 2 + battery_shift_y;

            DrawRect(renderer, icon_x, icon_y, body_w, body_h, outline_color, false);
            DrawRect(renderer, icon_x + body_w, center_y - cap_h / 2 + battery_shift_y, cap_w, cap_h, outline_color, true);

            const int inner_pad = 2;
            const int inner_w = body_w - inner_pad * 2;
            const int inner_h = body_h - inner_pad * 2;
            const int fill_w = std::clamp((inner_w * status.battery_percent) / 100, 0, inner_w);
            if (fill_w > 0) {
              DrawRect(renderer, icon_x + inner_pad, icon_y + inner_pad, fill_w, inner_h, fill_color, true);
            }

            if (battery_tex && battery_tex->texture) {
              SDL_Rect td{battery_text_x, center_y - battery_text_h / 2 + battery_shift_y, battery_text_w, battery_text_h};
              SDL_RenderCopy(renderer, battery_tex->texture, nullptr, &td);
            }
          }
#else
          (void)now;
#endif
        };
        ShelfRuntimeRenderDeps shelf_render_deps{
            renderer,
            ui_assets,
#ifdef HAVE_SDL2_TTF
            &ui_text_cache,
#else
            nullptr,
#endif
            shelf_runtime,
            shelf_render_cache,
            grid_item_anims,
            ShelfLayoutMetrics{
                Layout().screen_w,
                Layout().screen_h,
                Layout().top_bar_y,
                Layout().top_bar_h,
                Layout().bottom_bar_y,
                Layout().bottom_bar_h,
                Layout().cover_w,
                Layout().cover_h,
                Layout().card_frame_w,
                Layout().card_frame_h,
                Layout().grid_gap_x,
                Layout().grid_gap_y,
                Layout().grid_start_x,
                Layout().grid_start_y,
                Layout().title_overlay_h,
                Layout().title_text_pad_x,
                Layout().title_text_pad_bottom,
                Layout().title_marquee_gap_px,
                Layout().nav_l1_x,
                Layout().nav_l1_y,
                Layout().nav_r1_x,
                Layout().nav_r1_y,
                Layout().nav_start_x,
                Layout().nav_slot_w,
                Layout().nav_y,
            },
            kGridCols,
            kItemsPerPage,
            dt,
            animate_enabled,
            any_grid_animating,
            page_animating,
            page_anim_from,
            page_anim_to,
            page_anim_dir,
            page_slide.Value(),
            shelf_page,
            focus_index,
            nav_selected_index,
            title_marquee_offset,
            renderer_supports_target_textures,
            shelf_content_version,
            static_cast<float>(kUnfocusedAlpha),
            static_cast<float>(FocusedCoverW()),
            static_cast<float>(FocusedCoverH()),
            kCoverAspect,
            kCardMoveLinearSpeedX,
            kCardMoveLinearSpeedY,
            kCardMoveTailRatio,
            kCardMoveTailMinMul,
            kCardScaleLinearSpeedW,
            kCardScaleLinearSpeedH,
            kCardScaleTailRatio,
            kCardScaleTailMinMul,
            [&](int x, int y, int w, int h, SDL_Color c, bool fill) { DrawRect(renderer, x, y, w, h, c, fill); },
            [&](SDL_Texture *tex, int &w, int &h) { get_texture_size(tex, w, h); },
            [&](const BookItem &item) { return get_cover_texture(item); },
            [&](const std::string &text, SDL_Color color, int &w, int &h, SDL_Texture *&tex) {
                TextCacheEntry *entry = get_text_texture(text, color);
                tex = entry ? entry->texture : nullptr;
                w = entry ? entry->w : 0;
                h = entry ? entry->h : 0;
            },
            [&](const std::string &raw_name, int text_area_w, const std::function<int(const std::string &)> &measure) {
                return get_title_ellipsized(raw_name, text_area_w, measure);
            },
            [&](const BookItem &item) { return shelf_title_text(item); },
            forget_texture_size,
        };
        DrawShelfRuntime(shelf_render_deps);
        draw_system_status_overlay();

        if (state != State::Settings) {
          draw_volume_overlay();
        }
      }

      if (state == State::Reader) {
        const SDL_Color reader_bg =
            (reader_mode == ReaderMode::Txt && txt_reader.open)
                ? GetTxtBackgroundColor(config.Get().txt_background_color)
                : SDL_Color{12, 12, 12, 255};
        DrawRect(renderer, 0, 0, Layout().screen_w, Layout().screen_h, reader_bg);
        if (reader_mode == ReaderMode::Txt && txt_reader.open) {
          TxtReaderRenderDeps txt_render_deps{
              renderer,
              reader_ui,
              clamp_text_scroll,
              [&](const SDL_Rect &clip) { SDL_RenderSetClipRect(renderer, &clip); },
              [&]() { SDL_RenderSetClipRect(renderer, nullptr); },
              [&](const std::string &text, int x, int y) {
#ifdef HAVE_SDL2_TTF
                const SDL_Color color = GetTxtFontColor(config.Get().txt_font_color);
                TextCacheEntry *te = get_reader_text_texture(text, color);
                if (te && te->texture) {
                  SDL_Rect td{x, y, te->w, te->h};
                  SDL_RenderCopy(renderer, te->texture, nullptr, &td);
                }
#else
                (void)text;
                (void)x;
                (void)y;
#endif
              },
          };
          DrawTxtReaderRuntime(txt_render_deps);
        } else if (reader_mode == ReaderMode::Pdf && pdf_runtime.IsOpen()) {
          pdf_runtime.UpdateViewport(Layout().screen_w, Layout().screen_h);
          pdf_runtime.Tick();
          pdf_runtime.Draw(renderer);
        } else {
          epub_runtime.UpdateViewport(Layout().screen_w, Layout().screen_h);
          epub_runtime.Tick();
          epub_runtime.Draw(renderer);
        }
#ifdef HAVE_SDL2_TTF
        if (reader_progress_overlay_visible) {
          const int actual_pct = current_reader_progress_pct();
          const int txt_layout_pct = current_txt_layout_progress_pct();
          const bool txt_progress_computing =
              (reader_mode == ReaderMode::Txt && txt_reader.open && txt_reader.loading);
          const int pct =
              txt_progress_computing
                  ? txt_layout_pct
                  : (reader_ui.progress_overlay_dirty
                   ? ClampInt(reader_ui.progress_overlay_preview_pct, 0, 100)
                   : actual_pct);
          const int panel_h = 58;
          const int panel_y = Layout().screen_h - panel_h - Layout().reader_progress_panel_margin_bottom;
          DrawRect(renderer,
                   Layout().reader_progress_panel_margin_x,
                   panel_y,
                   Layout().screen_w - Layout().reader_progress_panel_margin_x * 2,
                   panel_h,
                   SDL_Color{0, 0, 0, 178});

          const int bar_x = Layout().reader_progress_bar_margin_x;
          const int bar_y = panel_y + 30;
          const int bar_w = Layout().screen_w - Layout().reader_progress_bar_margin_x * 2;
          const int bar_h = 12;
          DrawRect(renderer, bar_x, bar_y, bar_w, bar_h, SDL_Color{60, 60, 60, 220});
          const int actual_fill_source = txt_progress_computing ? txt_layout_pct : actual_pct;
          const int actual_fill_w = std::max(0, std::min(bar_w, (bar_w * actual_fill_source) / 100));
          if (actual_fill_w > 0) {
            DrawRect(renderer, bar_x, bar_y, actual_fill_w, bar_h, SDL_Color{125, 125, 125, 215});
          }
          const int fill_w = std::max(0, std::min(bar_w, (bar_w * pct) / 100));
          if (fill_w > 0) {
            DrawRect(renderer, bar_x, bar_y, fill_w, bar_h, SDL_Color{230, 230, 230, 235});
          }
          DrawRect(renderer, bar_x, bar_y, bar_w, bar_h, SDL_Color{255, 255, 255, 220}, false);

          SDL_Color tc{245, 245, 245, 255};
          const std::string pct_text = (pct < 10) ? ("0" + std::to_string(pct)) : std::to_string(pct);
          const std::string percent =
              txt_progress_computing ? ("(进度条计算中" + pct_text + "%)")
                                     : (((reader_mode == ReaderMode::Pdf && pdf_runtime.IsRenderPending()) ||
                                         (reader_mode == ReaderMode::Epub && epub_runtime.IsRenderPending()))
                                            ? ("(页面渲染中) " + std::to_string(pct) + "%")
                                            : (std::to_string(pct) + "%"));
          if (TextCacheEntry *te = get_text_texture(percent, tc); te && te->texture) {
            SDL_Rect td{Layout().screen_w - Layout().reader_progress_percent_margin_x - te->w, panel_y + 8, te->w, te->h};
            SDL_RenderCopy(renderer, te->texture, nullptr, &td);
          }
        }
#endif
      }

      if (state == State::Settings) {
        SettingsRuntimeRenderDeps settings_render_deps{
            renderer,
            ui_assets,
            cfg,
            menu_items,
            menu_selected,
            menu_anim,
            kSidebarMaskMaxAlpha,
            txt_transcode_job,
            system_settings_state,
            txt_settings_state,
            contributor_avatar_entries,
            contributor_avatar_state,
            version_update_state,
            SettingsRuntimeLayout{
                Layout().screen_w,
                Layout().screen_h,
                Layout().top_bar_y,
                Layout().top_bar_h,
                Layout().bottom_bar_y,
                Layout().bottom_bar_h,
                Layout().settings_sidebar_w,
                Layout().settings_y_offset,
                Layout().settings_content_offset_y,
            },
            [&](int x, int y, int w, int h, SDL_Color c, bool filled) { DrawRect(renderer, x, y, w, h, c, filled); },
            get_texture_size,
            get_text_texture,
            get_title_text_texture,
            Utf8Ellipsize,
            draw_volume_overlay,
        };
        DrawSettingsRuntime(settings_render_deps);
        draw_system_status_overlay();
      }

#ifdef HAVE_SDL2_TTF
      if (!transient_message.empty() && now < transient_message_until) {
        const SDL_Color text_color{245, 245, 245, 255};
        if (TextCacheEntry *te = get_text_texture(transient_message, text_color); te && te->texture) {
          const int pad_x = 24;
          const int pad_y = 12;
          const int box_w = te->w + pad_x * 2;
          const int box_h = te->h + pad_y * 2;
          const int box_x = std::max(0, (Layout().screen_w - box_w) / 2);
          const int box_y = std::max(0, Layout().screen_h - box_h - 72);
          DrawRect(renderer, box_x, box_y, box_w, box_h, SDL_Color{0, 0, 0, 210});
          DrawRect(renderer, box_x, box_y, box_w, box_h, SDL_Color{255, 255, 255, 40}, false);
          SDL_Rect td{box_x + pad_x, box_y + pad_y, te->w, te->h};
          SDL_RenderCopy(renderer, te->texture, nullptr, &td);
        }
      } else if (now >= transient_message_until) {
        transient_message.clear();
      }
#endif
    }

    const float flash = scene_flash.Value();
    if (flash > 0.001f) {
      DrawRect(renderer, 0, 0, Layout().screen_w, Layout().screen_h,
               SDL_Color{0, 0, 0, static_cast<Uint8>(std::clamp(flash, 0.0f, 1.0f) * 255.0f)});
    }

    SDL_RenderPresent(renderer);

    uint32_t frame_budget_ms = 0;
    if (contributor_marquee_active) frame_budget_ms = kAvatarMarqueeFrameBudgetMs;
    else if (has_active_animation) frame_budget_ms = kActiveFrameBudgetMs;
    else if (needs_periodic_tick) frame_budget_ms = kPeriodicTickFrameBudgetMs;
    if (frame_budget_ms > 0) {
      const uint32_t frame_elapsed = SDL_GetTicks() - frame_begin_ticks;
      if (frame_elapsed < frame_budget_ms) SDL_Delay(frame_budget_ms - frame_elapsed);
    }
  }

  if (!current_book.empty()) {
    if (reader_mode == ReaderMode::Pdf && pdf_runtime.IsOpen()) {
      const PdfRuntimeProgress active_pdf = pdf_runtime.Progress();
      reader.page = active_pdf.page;
      reader.scroll_y = active_pdf.scroll_y;
      reader.zoom = active_pdf.zoom;
      reader.rotation = active_pdf.rotation;
    } else if (reader_mode == ReaderMode::Epub && epub_runtime.IsOpen()) {
      const EpubRuntimeProgress active_epub = epub_runtime.Progress();
      reader.page = active_epub.page;
      reader.scroll_x = 0;
      reader.scroll_y = active_epub.scroll_y;
      reader.zoom = active_epub.zoom;
      reader.rotation = active_epub.rotation;
    } else if (reader_mode == ReaderMode::Txt && txt_reader.open) {
      if (!txt_reader.line_source_offsets.empty()) {
        const size_t top_line = std::min(
            txt_reader.line_source_offsets.size() - 1,
            static_cast<size_t>(std::max(0, txt_reader.scroll_px / std::max(1, txt_reader.line_h))));
        reader.scroll_x = static_cast<int>(std::min<size_t>(
            txt_reader.line_source_offsets[top_line], static_cast<size_t>(std::numeric_limits<int>::max())));
      } else {
        reader.scroll_x = 0;
      }
      reader.page = (txt_reader.line_h > 0) ? (txt_reader.scroll_px / txt_reader.line_h) : 0;
      reader.scroll_y = txt_reader.scroll_px;
      txt_reader.resume_cache_dirty = true;
      persist_current_txt_resume_snapshot(current_book, true);
    } else if (state != State::Reader) {
      // Not actively reading anymore.
      current_book.clear();
    }
    if (!current_book.empty()) {
      ReaderProgress save_reader = reader;
      progress.Set(current_book, save_reader);
      history_store.Add(current_book);
    }
  }
  flush_deferred_writes(true);
  ShutdownVersionUpdateState(version_update_state);
  clear_cover_cache();
  destroy_selected_avatar_badge_texture();
  DestroyContributorAvatarEntries(contributor_avatar_entries, forget_texture_size);
  DestroyUiAssets(ui_assets, forget_texture_size);
#ifdef HAVE_SDL2_TTF
  ShutdownUiTextCache(ui_text_cache, forget_texture_size);
#endif
  destroy_shelf_render_cache();
  pdf_runtime.Close();
  epub_runtime.Close();
  for (SDL_GameController *gc : opened_controllers) {
    if (gc) SDL_GameControllerClose(gc);
  }
  for (SDL_Joystick *js : opened_joysticks) {
    if (js) SDL_JoystickClose(js);
  }
  sfx.Shutdown();
  SDL_DestroyRenderer(renderer);
  SDL_DestroyWindow(window);
#ifdef HAVE_SDL2_IMAGE
  IMG_Quit();
#endif
#ifdef HAVE_SDL2_TTF
  TTF_Quit();
#endif
  SDL_Quit();
  return 0;
}

