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
#include <csignal>
#include <cstdlib>
#include <cstdint>
#include <deque>
#include "filesystem_compat.h"
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
#include "app_context.h"
#include "app_layout.h"
#include "app_stores.h"
#include "avatar_badge_runtime.h"
#include "audio_runtime.h"
#include "book_library_service.h"
#include "book_scanner.h"
#include "boot_scene.h"
#include "boot_runtime.h"
#include "contributor_avatar_runtime.h"
#include "cover_cache_runtime.h"
#include "cover_service.h"
#include "epub_runtime.h"
#include "epub_reader.h"
#include "input_manager.h"
#include "lid_power_control.h"
#include "menu_scene.h"
#include "pdf_reader.h"
#include "pdf_reader_module.h"
#include "pdf_runtime.h"
#include "progress_store.h"
#include "epub_reader_module.h"
#include "reader_core.h"
#include "reader_input_router.h"
#include "reader_manager.h"
#include "reader_progress_controller.h"
#include "reader_launch_service.h"
#include "reader_scene.h"
#include "reader_session_ops.h"
#include "reader_session_state.h"
#include "runtime_log.h"
#include "scene_manager.h"
#include "screen_profile.h"
#include "system_controls.h"
#include "system_settings_runtime.h"
#include "shelf_runtime.h"
#include "settings_runtime.h"
#include "sdl_utils.h"
#include "shelf_scene.h"
#include "storage_paths.h"
#include "status_bar_runtime.h"
#include "system_status.h"
#include "txt_reader_session.h"
#include "txt_reader_module.h"
#include "txt_reader_runtime.h"
#include "txt_text_service.h"
#include "txt_session_facade.h"
#include "txt_transcode_service.h"
#include "texture_registry.h"
#include "ui_assets.h"
#include "ui_assets_loader.h"
#include "ui_text_cache.h"
#include "version_update_runtime.h"
#include "volume_overlay.h"
#include "zip_image_reader_module.h"
#include "zip_image_runtime.h"
#include "animation.h"
#include "app_shell.h"

namespace {
bool VerboseLogEnabled() {
  auto enabled = [](const char *value) {
    return value && *value && std::string(value) != "0";
  };
  return enabled(std::getenv("ROCREADER_VERBOSE_LOG")) || enabled(std::getenv("ROCREADER_DEBUG_LOG"));
}

constexpr float kCoverAspect = 2.0f / 3.0f;
constexpr Uint8 kUnfocusedAlpha = 255;
constexpr float kTitleMarqueePauseSec = 0.75f;
constexpr float kTitleMarqueeSpeedPx = 48.0f;
constexpr size_t kCoverCacheMaxEntries = 160;
constexpr size_t kCoverCacheMaxBytes = 24u * 1024u * 1024u;
constexpr Uint8 kSidebarMaskMaxAlpha = 84;
constexpr int kIdleWaitMs = 100;
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
constexpr int kTxtLayoutCacheVersion = 5;
constexpr size_t kTxtMaxBytes = 64 * 1024 * 1024;
constexpr size_t kTxtMaxWrappedLines = 250000;
constexpr size_t kTxtLayoutCacheMaxEntries = 4;
#ifdef HAVE_SDL2_TTF
constexpr size_t kTextCacheMaxEntries = 128;
#endif

void FatalSignalHandler(int sig) {
  runtime_log::Line(std::string("fatal signal: ") + std::to_string(sig));
  std::_Exit(128 + sig);
}

std::string NormalizePathKey(const std::string &path);

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
    p = filesystem_compat::LexicallyNormal(p);
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
    if (!filesystem_compat::IsRegularFile(*it, ec)) {
      ec.clear();
      continue;
    }
    std::string version;
    if (!TryExtractVersionFromArchiveName(it->path().filename().string(), version)) continue;
    const auto write_time = filesystem_compat::LastWriteTime(*it, ec);
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

int main(int, char **argv) {
  std::setvbuf(stdout, nullptr, _IONBF, 0);
  std::setvbuf(stderr, nullptr, _IONBF, 0);
  runtime_log::Init(argv ? argv[0] : nullptr);
  std::signal(SIGSEGV, FatalSignalHandler);
  std::signal(SIGABRT, FatalSignalHandler);
  std::signal(SIGFPE, FatalSignalHandler);
  std::signal(SIGILL, FatalSignalHandler);
  runtime_log::Line("main: start");
  runtime_log::Line("main: SDL_Init begin");
  if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_GAMECONTROLLER | SDL_INIT_JOYSTICK | SDL_INIT_EVENTS) != 0) {
    runtime_log::Line(std::string("main: SDL_Init failed: ") + SDL_GetError());
    std::cerr << "[native_h700] SDL init failed: " << SDL_GetError() << "\n";
    return 1;
  }
  runtime_log::Line("main: SDL_Init ok");
  SDL_JoystickEventState(SDL_ENABLE);
#ifdef HAVE_SDL2_TTF
  runtime_log::Line("main: TTF_Init begin");
  if (TTF_Init() != 0) {
    runtime_log::Line(std::string("main: TTF_Init warning: ") + TTF_GetError());
    std::cerr << "[native_h700] SDL2_ttf init warning: " << TTF_GetError() << "\n";
  } else {
    runtime_log::Line("main: TTF_Init ok");
  }
#endif
#ifdef HAVE_SDL2_IMAGE
  runtime_log::Line("main: IMG_Init begin");
  const int img_flags = IMG_INIT_PNG | IMG_INIT_JPG | IMG_INIT_WEBP;
  const int img_ok = IMG_Init(img_flags);
  if ((img_ok & img_flags) == 0) {
    runtime_log::Line(std::string("main: IMG_Init warning: ") + IMG_GetError());
    std::cerr << "[native_h700] SDL2_image init warning: " << IMG_GetError() << "\n";
  } else {
    runtime_log::Line("main: IMG_Init ok");
  }
#endif
  const char *env_windowed = std::getenv("ROCREADER_WINDOWED");
  const char *env_fullscreen = std::getenv("ROCREADER_FULLSCREEN");
  const bool force_windowed = env_windowed && std::string(env_windowed) == "1";
  const bool force_fullscreen = env_fullscreen && std::string(env_fullscreen) == "1";
  runtime_log::Line("main: DetectScreenProfile begin");
  const ScreenProfile screen_profile = DetectScreenProfile();
  const bool verbose_log = VerboseLogEnabled();

  uint32_t win_flags = SDL_WINDOW_SHOWN;
#if defined(__arm__) || defined(__aarch64__)
  const bool default_fullscreen = true;
#else
  const bool default_fullscreen = false;
#endif
  if ((default_fullscreen && !force_windowed) || force_fullscreen) {
    win_flags |= SDL_WINDOW_FULLSCREEN;
  }
  SetLayoutProfile(SelectLayoutProfile(screen_profile.screen_w, screen_profile.screen_h));
  if (verbose_log) {
    std::cout << "[native_h700] screen detect: source=" << screen_profile.detection_source
              << " detected=" << screen_profile.detected_w << "x" << screen_profile.detected_h
              << " profile=" << screen_profile.profile_name
              << " layout=" << Layout().screen_w << "x" << Layout().screen_h << "\n";
  }

  runtime_log::Line(std::string("main: screen source=") + screen_profile.detection_source + " detected=" + std::to_string(screen_profile.detected_w) + "x" + std::to_string(screen_profile.detected_h) + " profile=" + screen_profile.profile_name);
  runtime_log::Line("main: SDL_CreateWindow begin");
  SDL_Window *window =
      SDL_CreateWindow("ROCreader Native H700",
                       SDL_WINDOWPOS_CENTERED,
                       SDL_WINDOWPOS_CENTERED,
                       Layout().screen_w,
                       Layout().screen_h,
                       win_flags);
  if (!window) {
    runtime_log::Line(std::string("main: SDL_CreateWindow failed: ") + SDL_GetError());
    std::cerr << "[native_h700] window failed: " << SDL_GetError() << "\n";
    SDL_Quit();
    return 2;
  }
  runtime_log::Line("main: SDL_CreateWindow ok");
  SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "0");
  runtime_log::Line("main: SDL_CreateRenderer begin");
  SDL_Renderer *renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
  if (!renderer) renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_SOFTWARE);
  if (!renderer) {
    runtime_log::Line(std::string("main: SDL_CreateRenderer failed: ") + SDL_GetError());
    std::cerr << "[native_h700] renderer failed: " << SDL_GetError() << "\n";
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 3;
  }
  SDL_RendererInfo renderer_info{};
  if (SDL_GetRendererInfo(renderer, &renderer_info) == 0) {
    if (verbose_log) {
      std::cout << "[native_h700] renderer: " << (renderer_info.name ? renderer_info.name : "unknown")
                << " flags=0x" << std::hex << renderer_info.flags << std::dec
                << " accelerated=" << ((renderer_info.flags & SDL_RENDERER_ACCELERATED) ? "yes" : "no")
                << " vsync=" << ((renderer_info.flags & SDL_RENDERER_PRESENTVSYNC) ? "yes" : "no") << "\n";
    }
  }
  runtime_log::Line("main: SDL_CreateRenderer ok");
  const bool renderer_supports_target_textures = (renderer_info.flags & SDL_RENDERER_TARGETTEXTURE) != 0;

  AppContext app_context;
  app_context.window = window;
  app_context.renderer = renderer;
  app_context.screen_profile = screen_profile;
  app_context.layout = &Layout();
  app_context.verbose_log = verbose_log;
  AppShell app_shell;
  app_shell.Initialize(app_context);

  std::vector<SDL_GameController *> opened_controllers;
  std::vector<SDL_Joystick *> opened_joysticks;
  const int joystick_count = SDL_NumJoysticks();
  if (verbose_log) {
    std::cout << "[native_h700] joysticks: " << joystick_count << "\n";
  }
  for (int i = 0; i < joystick_count; ++i) {
    const char *joy_name = SDL_JoystickNameForIndex(i);
    if (verbose_log) {
      std::cout << "[native_h700] joystick info: idx=" << i
                << " name=" << (joy_name ? joy_name : "unknown")
                << " is_gamecontroller=" << (SDL_IsGameController(i) ? "1" : "0") << "\n";
    }
    if (SDL_IsGameController(i)) {
      SDL_GameController *gc = SDL_GameControllerOpen(i);
      if (gc) {
        opened_controllers.push_back(gc);
        SDL_Joystick *js = SDL_GameControllerGetJoystick(gc);
        if (verbose_log) {
          std::cout << "[native_h700] opened gamecontroller idx=" << i
                    << " name=" << (SDL_GameControllerName(gc) ? SDL_GameControllerName(gc) : "unknown")
                    << " joystick_name=" << (js && SDL_JoystickName(js) ? SDL_JoystickName(js) : "unknown")
                    << " instance=" << (js ? SDL_JoystickInstanceID(js) : -1)
                    << " axes=" << (js ? SDL_JoystickNumAxes(js) : -1)
                    << " buttons=" << (js ? SDL_JoystickNumButtons(js) : -1)
                    << " hats=" << (js ? SDL_JoystickNumHats(js) : -1)
                    << " balls=" << (js ? SDL_JoystickNumBalls(js) : -1) << "\n";
        }
        continue;
      }
      if (verbose_log) {
        std::cout << "[native_h700] open gamecontroller failed idx=" << i
                  << " err=" << SDL_GetError() << "\n";
      }
    }
    SDL_Joystick *js = SDL_JoystickOpen(i);
    if (js) {
      opened_joysticks.push_back(js);
      if (verbose_log) {
        std::cout << "[native_h700] opened joystick idx=" << i
                  << " name=" << (SDL_JoystickName(js) ? SDL_JoystickName(js) : "unknown")
                  << " instance=" << SDL_JoystickInstanceID(js)
                  << " axes=" << SDL_JoystickNumAxes(js)
                  << " buttons=" << SDL_JoystickNumButtons(js)
                  << " hats=" << SDL_JoystickNumHats(js)
                  << " balls=" << SDL_JoystickNumBalls(js) << "\n";
      }
    } else {
      if (verbose_log) {
        std::cout << "[native_h700] open joystick failed idx=" << i
                  << " err=" << SDL_GetError() << "\n";
      }
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
      if (std::filesystem::exists(p)) return filesystem_compat::LexicallyNormal(p);
    }
    return filesystem_compat::LexicallyNormal((exe_path / name));
  };

  UiAssets ui_assets;
  TextureRegistry texture_registry;
  auto forget_texture_size = [&](SDL_Texture *tex) { texture_registry.Forget(tex); };
  auto remember_texture_size = [&](SDL_Texture *tex, int w, int h) { texture_registry.Remember(tex, w, h); };
  auto get_texture_size = [&](SDL_Texture *tex, int &w, int &h) { texture_registry.Get(tex, w, h); };
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
  if (!ui_assets_load_result.ui_pack_hit.empty() && verbose_log) {
    std::cout << "[native_h700] ui pack: " << ui_assets_load_result.ui_pack_hit.string()
              << " assets=" << ui_assets_load_result.packed_asset_count << "\n";
  }
  if (!ui_assets_load_result.ui_root_hit.empty() && verbose_log) {
    std::cout << "[native_h700] ui root: " << ui_assets_load_result.ui_root_hit.string() << "\n";
  }

  std::vector<ContributorAvatarEntry> contributor_avatar_entries;
  auto resolve_ui_root = [&]() -> std::filesystem::path {
    if (!ui_assets_load_result.ui_root_hit.empty()) {
      const std::filesystem::path common_root = ui_assets_load_result.ui_root_hit / "common";
      if (std::filesystem::exists(common_root)) return filesystem_compat::LexicallyNormal(common_root);
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
      if (std::filesystem::exists(candidate)) return filesystem_compat::LexicallyNormal(candidate);
    }
    return {};
  };
  const char *env_preload_avatars = std::getenv("ROCREADER_PRELOAD_AVATARS");
  const bool preload_avatars = !env_preload_avatars || std::string(env_preload_avatars) != "0";
  if (preload_avatars) {
    runtime_log::Line("main: contributor avatars preload begin");
    LoadContributorAvatarEntries(contributor_avatar_entries, resolve_ui_root(), exe_path, renderer,
                                 0,
                                 ScalePx(96),
                                 LoadSurfaceFromMemory, remember_texture_size, forget_texture_size);
    runtime_log::Line(std::string("main: contributor avatars preload done count=") +
                      std::to_string(contributor_avatar_entries.size()));
  } else {
    runtime_log::Line("main: contributor avatars preload skipped");
  }
  AvatarBadgeRuntime avatar_badge;
  avatar_badge.Configure(AvatarBadgeRuntimeDeps{
      renderer,
      contributor_avatar_entries,
      [](int value) { return ScalePx(value); },
      CreateScaledTextureCache,
      remember_texture_size,
      forget_texture_size,
  });

  std::vector<std::string> books_roots = storage_paths::DetectBooksRoots();
  runtime_log::Line(std::string("main: DetectBooksRoots count=") + std::to_string(books_roots.size()));
  for (const auto &r : books_roots) runtime_log::Line(std::string("main: books root: ") + r);
  if (books_roots.empty()) runtime_log::Line("main: WARNING no books roots found; expected folder name is lowercase books");
  runtime_log::Line("main: DetectCoverRoots begin");
  std::vector<std::string> cover_roots = storage_paths::DetectCoverRoots();
  runtime_log::Line(std::string("main: DetectCoverRoots count=") + std::to_string(cover_roots.size()));
  for (const auto &r : cover_roots) runtime_log::Line(std::string("main: cover root: ") + r);
  std::filesystem::path txt_layout_cache_dir =
      std::filesystem::path("/mnt/mmc/cache/txt_layouts");
  std::filesystem::path removable_txt_layout_cache_dir =
      std::filesystem::path("/mnt/sdcard/cache/txt_layouts");
  std::filesystem::path cover_thumb_cache_dir =
      std::filesystem::path("/mnt/mmc/cache/cover_thumbs");
  std::filesystem::path removable_cover_thumb_cache_dir =
      std::filesystem::path("/mnt/sdcard/cache/cover_thumbs");
  if (const char *env_cache = std::getenv("ROCREADER_CACHE_ROOT"); env_cache && *env_cache) {
    const std::filesystem::path cache_root(env_cache);
    txt_layout_cache_dir = cache_root / "txt_layouts";
    removable_txt_layout_cache_dir = cache_root / "txt_layouts";
    cover_thumb_cache_dir = cache_root / "cover_thumbs";
    removable_cover_thumb_cache_dir = cache_root / "cover_thumbs";
  }
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
  if (verbose_log) {
    std::cout << "[native_h700] books roots:";
    for (const auto &r : books_roots) std::cout << " " << r;
    std::cout << "\n";
    std::cout << "[native_h700] cover roots:";
    for (const auto &r : cover_roots) std::cout << " " << r;
    std::cout << "\n";
    std::cout << "[native_h700] cover thumb cache dir: "
              << filesystem_compat::LexicallyNormal(cover_thumb_cache_dir).string() << "\n";
    std::cout << "[native_h700] removable cover thumb cache dir: "
              << filesystem_compat::LexicallyNormal(removable_cover_thumb_cache_dir).string() << "\n";
    std::cout << "[native_h700] txt layout cache dir: "
              << filesystem_compat::LexicallyNormal(txt_layout_cache_dir).string() << "\n";
    std::cout << "[native_h700] removable txt layout cache dir: "
              << filesystem_compat::LexicallyNormal(removable_txt_layout_cache_dir).string() << "\n";
  }

  const std::filesystem::path keymap_path = resolve_runtime_file("native_keymap.ini");
#if defined(__arm__) || defined(__aarch64__)
  const bool use_h700_defaults = true;
  const std::string device_model_token = DetectDeviceModelToken();
#else
  const bool use_h700_defaults = false;
  const std::string device_model_token = DetectDeviceModelToken();
#endif
  const bool use_trimui_brick_keymap =
      device_model_token == "trimui-brick" || screen_profile.profile_name == "1024x768";
  const InputProfile input_profile =
      use_trimui_brick_keymap
          ? InputProfile::TrimuiBrick
          : (use_h700_defaults
                 ? (Uses34xxSpKeymap(device_model_token)
                        ? InputProfile::H70034xxSp
                        : (Uses35xxHKeymap(device_model_token) ? InputProfile::H70035xxH
                                                               : InputProfile::H700Default))
                 : InputProfile::DesktopDefault);
  const std::filesystem::path config_path = resolve_runtime_file("native_config.ini");
  const std::filesystem::path progress_path = resolve_runtime_file("native_progress.tsv");
  const std::filesystem::path favorites_path = resolve_runtime_file("native_favorites.txt");
  const std::filesystem::path history_path = resolve_runtime_file("native_history.txt");
  const char *env_power_script = std::getenv("ROCREADER_PWR_SCRIPT");
  const std::filesystem::path power_script_path =
      (env_power_script && *env_power_script) ? std::filesystem::path(env_power_script)
                                              : std::filesystem::path("/mnt/mod/ctrl/pwr_new.sh");
  if (verbose_log) {
    std::cout << "[native_h700] keymap path: " << filesystem_compat::LexicallyNormal(keymap_path).string() << "\n";
    std::cout << "[native_h700] config path: " << filesystem_compat::LexicallyNormal(config_path).string() << "\n";
    std::cout << "[native_h700] power script path: " << filesystem_compat::LexicallyNormal(power_script_path).string() << "\n";
    std::cout << "[native_h700] device model token: "
              << (device_model_token.empty() ? std::string("unknown") : device_model_token) << "\n";
    std::cout << "[native_h700] input profile: " << InputProfileName(input_profile) << "\n";
  }

  runtime_log::Line(std::string("main: keymap path: ") + filesystem_compat::LexicallyNormal(keymap_path).string());
  runtime_log::Line(std::string("main: config path: ") + filesystem_compat::LexicallyNormal(config_path).string());
  runtime_log::Line(std::string("main: input profile: ") + InputProfileName(input_profile));
  InputManager input(keymap_path.string(), input_profile);
  runtime_log::Line(std::string("main: joy map: ") + input.DescribeJoyMap());
  if (verbose_log) {
    std::cout << "[native_h700] joy map: " << input.DescribeJoyMap() << "\n";
  }
  runtime_log::Line(std::string("main: pad map: ") + input.DescribePadMap());
  if (verbose_log) {
    std::cout << "[native_h700] pad map: " << input.DescribePadMap() << "\n";
  }
  ConfigStore config(config_path.string());
  app_context.config.config = &config;
  if (!config.Get().audio) {
    config.Mutable().audio = true;
    config.MarkDirty();
    config.Save();
  }
  avatar_badge.SelectSavedOrDefault(config.Get().selected_contributor_avatar_label);
  ProgressStore progress(progress_path.string());
  RecentPathStore favorites_store(favorites_path.string(), NormalizePathKey);
  RecentPathStore history_store(history_path.string(), NormalizePathKey);
  VolumeController volume_controller(use_h700_defaults);
  SystemStatusMonitor system_status;
  SystemControlService system_control_service(use_h700_defaults);
  LidPowerController lid_power_controller(power_script_path);
  SystemSettingsState system_settings_state{};
  system_settings_state.auto_sleep_interval_index = ClampAutoSleepIntervalIndex(config.Get().auto_sleep_interval_index);
  system_settings_state.system_language_index = SystemLanguageIndexFromConfigValue(config.Get().system_language);
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
  if (avatar_badge.SelectedIndex() >= 0) {
    contributor_avatar_state.focus_index = avatar_badge.SelectedIndex();
  }
  if (use_h700_defaults) {
    bool changed = false;
    if (input_profile == InputProfile::TrimuiBrick) {
      system_control_service.RefreshVolumeOnly(system_settings_state.levels.volume);
    } else if (system_control_service.ApplyVolumePercent(config.Get().system_volume_percent, system_settings_state.levels.volume) &&
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
  uint32_t last_system_volume_sync = 0;
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
  bool pending_volume_change_sfx = false;
  uint32_t pending_volume_change_sfx_due = 0;
  const char *system_volume_sfx_env = std::getenv("ROCREADER_SYSTEM_VOLUME_SFX_FOLLOWS_HARDWARE");
  const bool system_volume_sfx_follows_hardware =
      system_volume_sfx_env && (*system_volume_sfx_env == '1' || *system_volume_sfx_env == 'y' ||
                                *system_volume_sfx_env == 'Y' || *system_volume_sfx_env == 't' ||
                                *system_volume_sfx_env == 'T');
  int runtime_sfx_volume = config.Get().sfx_volume;
  if (volume_controller.UsesSystemVolume() && system_volume_sfx_follows_hardware) {
    runtime_sfx_volume = SDL_MIX_MAXVOLUME;
  }
  sfx.SetVolume(runtime_sfx_volume);
  auto ensure_sfx_ready = [&]() -> bool {
    if (sfx_ready) return true;
    if (sfx_init_attempted) return false;
    sfx_init_attempted = true;
    sfx_ready = sfx.Init(exe_path);
    if (sfx_ready) {
      sfx.SetVolume(runtime_sfx_volume);
    }
    if (!sfx_ready && verbose_log) {
      std::cout << "[native_h700] sound: disabled (all audio backends failed)\n";
    }
    if (verbose_log) {
      std::cout << "[native_h700] sound init: backend=" << sfx.BackendName()
                << " ready=" << (sfx_ready ? "1" : "0")
                << " volume=" << runtime_sfx_volume << "\n";
    }
    return sfx_ready;
  };
  if (verbose_log) {
    std::cout << "[native_h700] sound: config_audio=" << (config.Get().audio ? "1" : "0")
              << " backend=" << sfx.BackendName()
              << " ready=deferred"
              << " volume=" << config.Get().sfx_volume << "\n";
  }
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
  ZipImageRuntime zip_image_runtime;
  ReaderManager reader_manager;
  PdfReaderModule pdf_reader_module(pdf_runtime);
  EpubReaderModule epub_reader_module;
  ZipImageReaderModule zip_image_reader_module(zip_image_runtime);
  if (verbose_log) {
    std::cout << "[native_h700] epub comic backend: " << epub_runtime.BackendName()
              << " (real_renderer=" << (epub_runtime.HasRealRenderer() ? "yes" : "no") << ")\n";
    std::cout << "[native_h700] zip image backend: " << zip_image_runtime.BackendName()
              << " (real_renderer=" << (zip_image_runtime.HasRealRenderer() ? "yes" : "no") << ")\n";
  }
  ShelfRenderCache shelf_render_cache;
  ShelfRuntimeState shelf_runtime;
  ShelfScene shelf_scene;
  uint64_t &shelf_content_version = shelf_runtime.content_version;

  AppScene &state = app_shell.Scenes().CurrentRef();
  BootRuntimeState boot_runtime;
  {
    std::error_code ec;
    const std::filesystem::path boot_status_path =
        std::filesystem::current_path(ec) / "cache" / "update_boot_status.txt";
    boot_runtime.language_index = SystemLanguageIndexFromConfigValue(config.Get().system_language);
    const char *boot_install_pending = std::getenv("ROCREADER_BOOT_INSTALL_PENDING_UPDATE");
    if (boot_install_pending && std::string(boot_install_pending) == "1") {
      InitializeBootRuntimePendingUpdate(boot_runtime);
    } else if (!ec) {
      InitializeBootRuntimeReplay(boot_runtime, boot_status_path);
    }
  }
  BootScene boot_scene(boot_runtime, [&]() { app_shell.RequestQuit(); });
  std::vector<BookItem> &shelf_items = shelf_runtime.items;
  CoverCacheRuntime cover_cache(kCoverCacheMaxEntries, kCoverCacheMaxBytes);
  ShelfSceneState shelf_state;
  shelf_state.title_marquee_wait = kTitleMarqueePauseSec;

  MenuSceneState menu_state;
  menu_state.items = {
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
  bool &reader_progress_overlay_visible = reader_ui.progress_overlay_visible;
  float &hold_cooldown = reader_ui.hold_cooldown;
  auto current_category = [&]() -> ShelfCategory {
    return ClampShelfCategory(shelf_state.nav_selected_index);
  };
  auto file_exists_fs = [&](const std::filesystem::path &path) -> bool {
    std::error_code ec;
    return !path.empty() && std::filesystem::exists(path, ec) && !ec;
  };
  auto file_exists = [&](const std::string &path) -> bool {
    return !path.empty() && file_exists_fs(std::filesystem::path(path));
  };
  auto item_real_path = [&](const BookItem &item) -> const std::string & {
    return book_library_service::RealPathForItem(item);
  };
  auto item_fs_path = [&](const BookItem &item) -> std::filesystem::path {
    if (!item.native_fs_path.empty()) return item.native_fs_path;
    const std::string &real_path = item_real_path(item);
    return real_path.empty() ? std::filesystem::path(item.path) : std::filesystem::path(real_path);
  };
  auto get_compatible_progress = [&](const BookItem &item) -> ReaderProgress {
    return book_library_service::CompatibleProgressForItem(item, progress);
  };
  auto make_shelf_runtime_deps = [&]() {
    return book_library_service::MakeShelfRuntimeDeps(
        NormalizePathKey,
        GetLowerExt,
        [&]() { return boot_runtime.scanned_books; },
        favorites_store,
        history_store,
        kShelfScanCacheTtlMs,
        kShelfScanCacheMaxEntries);
  };

  auto rebuild_shelf_items = [&]() {
    RebuildShelfItems(shelf_runtime, current_category(), shelf_state.current_folder, books_roots, make_shelf_runtime_deps());
  };
  std::string transient_message;
  uint32_t transient_message_until = 0;
  uint32_t transient_message_shown_at = 0;
  bool transient_message_dismiss_on_input = false;
  auto show_transient_message = [&](const std::string &message,
                                    uint32_t duration_ms = kTransientMessageDurationMs,
                                    bool dismiss_on_input = false) {
    transient_message = message;
    transient_message_shown_at = SDL_GetTicks();
    transient_message_until = transient_message_shown_at + duration_ms;
    transient_message_dismiss_on_input = dismiss_on_input;
  };
  auto any_button_just_pressed = [&]() {
    for (int i = 0; i < kButtonCount; ++i) {
      if (input.IsJustPressed(static_cast<Button>(i))) return true;
    }
    return false;
  };

  auto clear_cover_cache = [&]() {
    cover_cache.Clear(forget_texture_size);
    ++shelf_content_version;
  };
  auto clear_history_and_refresh_shelf = [&]() {
    history_store.Clear();
    if (current_category() == ShelfCategory::History) {
      shelf_scene.ResetToCategoryRoot(shelf_state);
      clear_cover_cache();
      rebuild_shelf_items();
    }
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

  auto cover_texture_w = [&]() {
    return std::max(FocusedCoverW(), Layout().cover_w * 2);
  };
  auto cover_texture_h = [&]() {
    return std::max(FocusedCoverH(), Layout().cover_h * 2);
  };
  auto make_cover_service_deps = [&]() {
    return CoverServiceDeps{
        renderer,
        cover_texture_w(),
        cover_texture_h(),
        kCoverAspect,
        cover_thumb_cache_dir,
        removable_cover_thumb_cache_dir,
        cover_roots,
        ui_assets.book_cover_txt,
        ui_assets.book_cover_pdf,
        &cover_cache.ManualPathCache(),
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
    else if (ext == ".zip" || ext == ".cbz") texture = CreateZipImageFirstImageCoverTextureLocal(doc_path, deps);
    if (texture) return texture;
    return nullptr;
  };

  auto get_cover_texture = [&](const BookItem &item) -> SDL_Texture * {
    const std::string &real_path = item_real_path(item);
    const std::string cover_cache_key =
        real_path + "|cover|" + std::to_string(cover_texture_w()) + "x" + std::to_string(cover_texture_h());
    if (SDL_Texture *cached = cover_cache.FindTexture(cover_cache_key)) {
      return cached;
    }

    CoverServiceDeps deps = make_cover_service_deps();
    BookItem resolved_item = item;
    resolved_item.path = real_path;
    resolved_item.real_path = real_path;
    resolved_item.native_fs_path = item_fs_path(item);
    SDL_Texture *tex = ResolveBookCoverTexture(resolved_item, current_category(), deps);

    const bool shared_ui_cover = (tex == ui_assets.book_cover_txt ||
                                  tex == ui_assets.book_cover_pdf);
    const bool owned = (tex != nullptr && !shared_ui_cover);
    cover_cache.PutTexture(cover_cache_key, tex, owned, get_texture_size, forget_texture_size);
    return tex;
  };

#ifdef HAVE_SDL2_TTF
  UiTextCacheState ui_text_cache{};
  ui_text_cache.max_text_cache_entries = kTextCacheMaxEntries;
  auto scaled_font_pt = [&](int pt) {
    return std::max(1, static_cast<int>(std::round(static_cast<float>(pt) * Layout().ui_scale)));
  };
  auto reader_font_pt_for_level = [&](int level) {
    return scaled_font_pt(TxtFontPointSizeForLevel(level));
  };
  const int body_font_pt = scaled_font_pt(16);
  const int title_font_pt = scaled_font_pt(24);
  int current_reader_font_pt = reader_font_pt_for_level(config.Get().txt_font_size_level);
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

  auto apply_epub_flow_theme = [&]() {
    epub_reader_module.SetFlowBaseFontPointSize(current_reader_font_pt);
    epub_reader_module.SetFlowColors(GetTxtBackgroundColor(config.Get().txt_background_color),
                                     GetTxtFontColor(config.Get().txt_font_color));
  };

  apply_txt_font_size_level = [&](int level) {
    const int clamped = ClampTxtFontSizeLevel(level);
    if (config.Mutable().txt_font_size_level != clamped) {
      config.Mutable().txt_font_size_level = clamped;
      config.MarkDirty();
      config.Save();
    }
    txt_settings_state.font_size_level = clamped;
    current_reader_font_pt = reader_font_pt_for_level(clamped);
    apply_epub_flow_theme();
    ShutdownUiTextCache(ui_text_cache, forget_texture_size);
  };

  std::function<void(const std::string &, bool)> persist_current_txt_resume_snapshot;

  auto get_title_ellipsized = [&](const std::string &raw_name, int text_area_w,
                                  const std::function<int(const std::string &)> &measure) -> std::string {
    return GetTitleEllipsized(ui_text_cache, raw_name, text_area_w, measure);
  };

  auto open_ui_font = [&]() {
    OpenUiFonts(ui_text_cache, exe_path, ui_path, body_font_pt, title_font_pt, current_reader_font_pt);
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
    if (state != AppScene::Shelf) return false;
    if (shelf_state.focus_index < 0 || shelf_state.focus_index >= static_cast<int>(shelf_items.size())) return false;
    SDL_Color title_color{248, 250, 255, 255};
    const std::string display = shelf_title_text(shelf_items[shelf_state.focus_index]);
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
    if (verbose_log) {
      std::cout << "[native_h700] runtime caches cleared: both cards cover thumbs + txt layouts/resume\n";
    }
  };

  TxtTranscodeServiceDeps txt_transcode_deps{
      books_roots,
      NormalizePathKey,
      GetLowerExt,
      ReadFileBytes,
      DecodeTextBytesToUtf8,
      WriteFileBytesAtomically,
      clear_runtime_cache_files,
      verbose_log,
  };

  auto start_txt_transcode_job = [&]() { StartTxtTranscodeJob(txt_transcode_job, txt_transcode_deps); };
  auto process_txt_transcode_step = [&]() { ProcessTxtTranscodeStep(txt_transcode_job, txt_transcode_deps); };

  auto destroy_shelf_render_cache = [&]() {
    DestroyShelfRenderCache(shelf_render_cache, forget_texture_size);
  };

  auto invalidate_all_render_cache = [&]() {};

  auto close_text_reader = [&]() {
    reader_ui.Txt() = TxtReaderState{};
    reader_progress_overlay_visible = false;
    if (reader_mode == ReaderMode::Txt) {
      reader_mode = ReaderMode::None;
    }
  };

  auto clamp_text_scroll = [&]() {
    const int max_scroll = std::max(0, reader_ui.Txt().content_h - reader_ui.Txt().viewport_h);
    reader_ui.Txt().scroll_px = ClampInt(reader_ui.Txt().scroll_px, 0, max_scroll);
    if (!reader_ui.Txt().loading) {
      reader.scroll_y = reader_ui.Txt().scroll_px;
      if (!reader_ui.Txt().line_source_offsets.empty()) {
        const size_t top_line = std::min(
            reader_ui.Txt().line_source_offsets.size() - 1,
            static_cast<size_t>(std::max(0, reader_ui.Txt().scroll_px / std::max(1, reader_ui.Txt().line_h))));
        reader.scroll_x = static_cast<int>(std::min<size_t>(
            reader_ui.Txt().line_source_offsets[top_line], static_cast<size_t>(std::numeric_limits<int>::max())));
      } else {
        reader.scroll_x = 0;
      }
    }
  };

  auto get_text_viewport_bounds = [&]() -> SDL_Rect {
    open_ui_font();
    const int font_pt = reader_font_pt_for_level(config.Get().txt_font_size_level);
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
            font_h + ScalePx(kTxtLineSpacing),
        });
  };

#ifdef HAVE_SDL2_TTF
  const std::string kTxtParagraphIndent = u8"銆€銆€";
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

  TxtSessionFacade txt_session_facade{
      TxtSessionFacadeDeps{
        reader_ui,
        txt_text_service,
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
        NormalizePathKey,
        [&](const std::string &raw, std::string &out) { return DecodeTextBytesToUtf8(raw, out); },
        append_wrapped_text_line,
        invalidate_all_render_cache,
        clamp_text_scroll,
        txt_layout_cache_dir / "epub_assets",
        ScalePx(kTxtLineSpacing),
        kTxtMaxBytes,
        kTxtMaxWrappedLines,
        kTxtResumeSaveDelayMs,
        kTxtLayoutCacheVersion,
      },
  };

  persist_current_txt_resume_snapshot = [&](const std::string &book_path, bool force) {
    txt_session_facade.PersistResumeSnapshot(book_path, force);
  };

  auto open_text_book = [&](const std::string &path) -> bool {
    return txt_session_facade.OpenTextBook(path);
  };

  auto open_epub_text_book = [&](const std::string &path) -> bool {
    return txt_session_facade.OpenEpubTextFallback(path);
  };

  auto text_scroll_by = [&](int delta_px) {
    txt_session_facade.ScrollBy(current_book, delta_px);
  };

  auto text_page_by = [&](int dir) {
    txt_session_facade.PageBy(current_book, dir);
  };

  auto text_jump_to_percent = [&](int pct) {
    txt_session_facade.JumpToPercent(current_book, pct);
  };

  TxtReaderModule txt_reader_module(
      reader_ui,
      TxtReaderModuleCallbacks{
          open_text_book,
          close_text_reader,
          text_scroll_by,
          text_page_by,
          text_jump_to_percent,
      });
  reader_manager.Register(ReaderMode::Pdf, &pdf_reader_module);
  reader_manager.Register(ReaderMode::Epub, &epub_reader_module);
  reader_manager.Register(ReaderMode::Txt, &txt_reader_module);
  reader_manager.Register(ReaderMode::ZipImage, &zip_image_reader_module);
  ReaderScene reader_scene([&]() {
    app_shell.Scenes().EnterShelf();
    app_shell.StartSceneFlash();
  });
  MenuScene menu_scene;

  ReaderProgressControllerDeps reader_progress_deps{
      reader_ui,
      pdf_runtime,
      epub_runtime,
      zip_image_runtime,
      &reader_manager,
      text_jump_to_percent,
  };

  auto reader_jump_to_percent = [&](int pct) { ReaderJumpToPercent(reader_progress_deps, pct); };
  auto reader_jump_to_txt_source_offset = [&](size_t source_offset) {
    txt_session_facade.JumpToSourceOffset(current_book, source_offset);
  };

  auto current_reader_progress_pct = [&]() -> int {
    return CurrentReaderProgressPercent(reader_progress_deps);
  };
  auto make_reader_progress_input_config = [&]() {
    return MakeReaderSceneProgressInputConfig(ScalePx(kReaderTapStepPx));
  };
  auto make_reader_progress_overlay_metrics = [&]() {
    return MakeReaderSceneProgressOverlayMetrics(Layout());
  };
  auto make_reader_scene_input_services = [&]() {
    return MakeReaderSceneInputServices(
        close_text_reader,
        persist_current_txt_resume_snapshot,
        ReaderSceneInputActions{
            current_reader_progress_pct,
            reader_jump_to_percent,
            reader_jump_to_txt_source_offset,
            text_scroll_by,
            text_page_by,
            show_transient_message,
        });
  };
  auto make_reader_scene_render_services = [&]() {
    return MakeReaderSceneRenderServices(
        renderer,
        [](int value) { return ScalePx(value); },
        [&](int x, int y, int w, int h, SDL_Color c, bool filled) { DrawRect(renderer, x, y, w, h, c, filled); },
        clamp_text_scroll,
        get_text_texture,
        get_reader_text_texture);
  };
  auto make_menu_scene_layout_metrics = [&]() {
    return MakeMenuSceneLayoutMetrics(Layout());
  };
  auto make_system_settings_callbacks = [&]() {
    return SystemSettingsCallbacks{
        [&](SystemControlLevels &levels) {
          system_control_service.Refresh(levels);
        },
        [&](int delta, SystemControlLevels &levels) {
          const bool ok = system_control_service.AdjustVolume(delta, levels);
          system_control_service.Refresh(levels);
          if (levels.volume.available) {
            const int saved_percent =
                std::clamp((levels.volume.level * 100) / std::max(1, levels.volume.max_level), 0, 100);
            app_ui.volume_display_percent = saved_percent;
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
          settings_state.auto_sleep_interval_index = ClampAutoSleepIntervalIndex(config.Get().auto_sleep_interval_index);
          settings_state.system_language_index = SystemLanguageIndexFromConfigValue(config.Get().system_language);
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
          const int next_index = ClampAutoSleepIntervalIndex(settings_state.auto_sleep_interval_index + delta);
          if (next_index == settings_state.auto_sleep_interval_index) return false;
          config.Mutable().auto_sleep_interval_index = next_index;
          config.MarkDirty();
          config.Save();
          settings_state.auto_sleep_interval_index = next_index;
          last_user_input_tick = SDL_GetTicks();
          auto_sleep_waiting_for_input = false;
          return true;
        },
        [&](int delta, SystemSettingsState &settings_state) {
          const int language_count = std::max(1, SystemLanguageCount());
          int next_index = settings_state.system_language_index;
          if (delta > 0) {
            next_index = (next_index + 1) % language_count;
          } else if (delta < 0) {
            next_index = (next_index - 1 + language_count) % language_count;
          }
          if (next_index == settings_state.system_language_index) return false;
          config.Mutable().system_language = SystemLanguageConfigValue(next_index);
          config.MarkDirty();
          config.Save();
          settings_state.system_language_index = next_index;
          last_user_input_tick = SDL_GetTicks();
          return true;
        },
        [&]() {
          clear_runtime_cache_files();
          return true;
        },
        [&]() {
          clear_history_and_refresh_shelf();
          return true;
        },
    };
  };
  auto make_txt_settings_callbacks = [&]() {
    return TxtSettingsCallbacks{
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
          apply_epub_flow_theme();
          return true;
        },
        [&](int color_index, TxtSettingsState &settings_state) {
          const int clamped = ClampTxtColorIndex(color_index);
          config.Mutable().txt_font_color = clamped;
          config.MarkDirty();
          config.Save();
          settings_state.font_color = clamped;
          settings_state.selected_option = clamped;
          apply_epub_flow_theme();
          return true;
        },
        [&](int delta, TxtSettingsState &settings_state) {
          const int next_level = ClampTxtFontSizeLevel(settings_state.font_size_level + delta);
          if (next_level == settings_state.font_size_level) return false;
          apply_txt_font_size_level(next_level);
          if (reader_mode == ReaderMode::Txt && reader_ui.Txt().open && !current_book.empty()) {
            reader = txt_reader_module.Progress();
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
    };
  };
  auto make_settings_input_actions = [&]() {
    return SettingsRuntimeInputActions{
        false,
        [&]() { app_shell.Scenes().ReturnFromSettings(); },
        [&]() { app_shell.RequestQuit(); },
        clear_history_and_refresh_shelf,
        clear_runtime_cache_files,
        start_txt_transcode_job,
        [&](int selected_index) {
          if (selected_index < 0 || selected_index >= static_cast<int>(contributor_avatar_entries.size())) return;
          const std::string &selected_label = contributor_avatar_entries[selected_index].raw_label;
          if (config.Mutable().selected_contributor_avatar_label != selected_label) {
            config.Mutable().selected_contributor_avatar_label = selected_label;
            config.MarkDirty();
            config.Save();
          }
          avatar_badge.SelectIndex(selected_index);
        },
    };
  };
  auto make_settings_render_services = [&](const std::function<void()> &draw_volume_overlay) {
    return MakeMenuSceneRenderServices(MenuSceneRenderServiceCallbacks{
        [&](int x, int y, int w, int h, SDL_Color c, bool filled) {
          DrawRect(renderer, x, y, w, h, c, filled);
        },
        get_texture_size,
        get_text_texture,
        get_title_text_texture,
        get_reader_text_texture,
        Utf8Ellipsize,
        draw_volume_overlay,
    });
  };
  auto make_version_update_callbacks = [&]() {
    return VersionUpdateCallbacks{
        [&](VersionUpdateState &update_state) { BeginVersionUpdateDownload(update_state); },
    };
  };
  auto make_shelf_scene_input_services = [&]() {
    return ShelfSceneInputServices{
        focused_title_needs_marquee,
        clear_cover_cache,
        rebuild_shelf_items,
        [&](const std::string &path) { favorites_store.Add(path); },
        [&](const std::string &path) { favorites_store.Remove(path); },
        current_category,
        MakeShelfReaderLaunchHandler(ShelfReaderLaunchHandlerDeps{
            renderer,
            [&]() { return Layout().screen_w; },
            [&]() { return Layout().screen_h; },
            reader_ui,
            &reader_manager,
            &pdf_runtime,
            &epub_runtime,
            &zip_image_runtime,
            [&]() { return current_reader_font_pt; },
            [&]() { return GetTxtBackgroundColor(config.Get().txt_background_color); },
            [&]() { return GetTxtFontColor(config.Get().txt_font_color); },
            open_text_book,
            close_text_reader,
            file_exists,
            item_real_path,
            get_compatible_progress,
            GetLowerExt,
            open_epub_text_book,
            [&](const std::string &path) { history_store.Add(path); },
            [&]() { app_shell.Scenes().EnterReader(); },
            [&]() { app_shell.Scenes().EnterShelf(); },
            [&]() { app_shell.StartSceneFlash(); },
            [&](const std::string &message) { show_transient_message(message); },
        }),
    };
  };
  auto make_shelf_scene_render_services = [&]() {
    return MakeShelfSceneRenderServices(ShelfSceneRenderServiceCallbacks{
        [&](int x, int y, int w, int h, SDL_Color c, bool fill) { DrawRect(renderer, x, y, w, h, c, fill); },
        [&](SDL_Texture *tex, int &w, int &h) { get_texture_size(tex, w, h); },
        [&](const BookItem &item) { return get_cover_texture(item); },
        get_text_texture,
        [&](const std::string &raw_name, int text_area_w, const std::function<int(const std::string &)> &measure) {
          return get_title_ellipsized(raw_name, text_area_w, measure);
        },
        [&](const BookItem &item) { return shelf_title_text(item); },
        forget_texture_size,
    });
  };
  auto make_menu_scene_input_services = [&]() {
    return MakeMenuSceneInputServices(
        make_system_settings_callbacks(),
        make_txt_settings_callbacks(),
        make_version_update_callbacks(),
        make_settings_input_actions());
  };

  uint32_t prev_ticks = SDL_GetTicks();
  while (app_shell.IsRunning()) {
    const AppFrameTiming frame = app_shell.BeginFrame(prev_ticks);
    const uint32_t frame_begin_ticks = frame.frame_begin_ticks;
    const uint32_t now = frame.now;
    const float dt = frame.dt;

    hold_cooldown = std::max(0.0f, hold_cooldown - dt);
    menu_scene.Tick(menu_state, dt);
    TickAppUiState(app_ui, dt);
    TickVersionUpdateState(version_update_state, dt);

    input.BeginFrame(dt);
    const bool animate_enabled = config.Get().animations;
    const bool contributor_marquee_active =
        state == AppScene::Settings &&
        menu_scene.IsSelected(menu_state, SettingId::ContributorAvatars) &&
        !contributor_avatar_entries.empty();
    const bool version_update_download_active =
        state == AppScene::Settings &&
        menu_scene.IsSelected(menu_state, SettingId::VersionUpdate) &&
        version_update_state.download_in_progress;
    const bool has_active_animation =
        state == AppScene::Boot || input.AnyPressed() ||
        txt_transcode_job.active ||
        (reader_mode == ReaderMode::Txt && reader_ui.Txt().open && reader_ui.Txt().loading) ||
        version_update_download_active ||
        contributor_marquee_active ||
        (animate_enabled &&
         (menu_scene.IsAnimating(menu_state) || app_shell.IsSceneFlashAnimating() || shelf_state.page_animating ||
          shelf_state.any_grid_animating));
    const bool needs_periodic_tick =
        (state == AppScene::Shelf && shelf_state.title_marquee_active) ||
        (state == AppScene::Reader && reader_scene.IsRenderPending(reader_ui, &reader_manager));
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
    const AppEventPumpResult event_pump =
        app_shell.PumpEvents(input, has_active_animation, idle_wait_ms, note_user_input);
    if (has_active_animation) {
      maybe_trigger_auto_sleep();
    } else {
      if (event_pump.had_event) {
        maybe_trigger_auto_sleep();
      } else {
        maybe_trigger_auto_sleep();
        system_status.Poll(SDL_GetTicks());
        if (has_pending_flush && !needs_periodic_tick) {
        // Wake only to flush deferred IO; keep the current frame untouched.
          input.EndFrame();
          flush_deferred_writes(false);
          app_shell.ResetFrameClock(prev_ticks);
          continue;
        }
        if (!needs_periodic_tick && !has_pending_flush) {
        // Fully idle: no input, no animation, no incremental loading.
        // Skip update/render work and keep sleeping until something changes.
          app_shell.ResetFrameClock(prev_ticks);
          input.EndFrame();
          continue;
        }
      }
    }
    input.EndFrame();

    if (input.IsJustPressed(Button::Power)) {
      if (lid_power_controller.TriggerPowerKeyScreenOff(input_profile)) {
        last_user_input_tick = SDL_GetTicks();
        auto_sleep_waiting_for_input = true;
        input.ResetAll();
      }
    }

    system_status.Poll(now);

    if (reader_mode == ReaderMode::Txt && reader_ui.Txt().open && reader_ui.Txt().loading) {
      txt_session_facade.TickLoading(current_book, 5, 24576);
    }
    process_txt_transcode_step();
    flush_deferred_writes(false);
    if (state == AppScene::Settings && volume_controller.UsesSystemVolume() && now - last_system_volume_sync >= 250) {
      int synced_volume_percent = app_ui.volume_display_percent;
      if (volume_controller.RefreshPercent(synced_volume_percent)) {
        app_ui.volume_display_percent = synced_volume_percent;
        config.Mutable().system_volume_percent = synced_volume_percent;
      }
      if (system_control_service.RefreshVolumeOnly(system_settings_state.levels.volume) &&
          system_settings_state.levels.volume.available) {
        const int menu_volume_percent = std::clamp(
            (system_settings_state.levels.volume.level * 100) /
                std::max(1, system_settings_state.levels.volume.max_level),
            0, 100);
        app_ui.volume_display_percent = menu_volume_percent;
        config.Mutable().system_volume_percent = menu_volume_percent;
      }
      last_system_volume_sync = now;
    }
    HandleVolumeControls(
        app_ui, input, now, volume_controller, config,
        [&](int volume) {
          runtime_sfx_volume = std::clamp(volume, 0, SDL_MIX_MAXVOLUME);
          sfx.SetVolume(runtime_sfx_volume);
        },
        [&]() { play_sfx(SfxId::Change); },
        [&](uint32_t due) {
          if (!pending_volume_change_sfx) {
            pending_volume_change_sfx = true;
            pending_volume_change_sfx_due = due;
          }
        });
    if (pending_volume_change_sfx && SDL_TICKS_PASSED(now, pending_volume_change_sfx_due)) {
      pending_volume_change_sfx = false;
      play_sfx(SfxId::Change);
    }

    bool transient_message_dismissed_this_frame = false;
    if (transient_message_dismiss_on_input && !transient_message.empty() &&
        SDL_TICKS_PASSED(now, transient_message_shown_at + 1) && any_button_just_pressed()) {
      transient_message.clear();
      transient_message_until = 0;
      transient_message_dismiss_on_input = false;
      transient_message_dismissed_this_frame = true;
    }

    if (state == AppScene::Shelf || state == AppScene::Settings) {
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

    if (input.IsPressed(Button::Start) && input.IsPressed(Button::Select)) app_shell.RequestQuit();

    // Dedicated settings toggle path (single entry for Start / Select mapping).
    const MenuToggleAction menu_toggle_action =
        HandleMenuToggleInput(app_ui, input, state == AppScene::Settings, state == AppScene::Shelf,
                              state == AppScene::Reader, menu_scene.CanCloseWithToggle(menu_state), 0.0f,
                              false,
                              kMenuToggleDebounceSec, input_profile);
    if (menu_toggle_action == MenuToggleAction::CloseSettings) {
      menu_scene.BeginClose(
          menu_state,
          MenuSceneAnimationConfig{config.Get().animations, 0.16f, 0.20f, kSettingsToggleGuardSec});
      play_sfx(SfxId::Back);
    } else if (menu_toggle_action == MenuToggleAction::OpenFromShelf ||
               menu_toggle_action == MenuToggleAction::OpenFromReader) {
      app_shell.Scenes().OpenSettingsFrom(
          (menu_toggle_action == MenuToggleAction::OpenFromShelf) ? AppScene::Shelf : AppScene::Reader);
      menu_scene.BeginOpen(
          menu_state,
          MenuSceneAnimationConfig{config.Get().animations, 0.16f, 0.20f, kSettingsToggleGuardSec});
      play_sfx(SfxId::Back);
    }

    if (state == AppScene::Boot) {
      BootSceneTickDeps boot_tick_deps{
          books_roots,
          kBootCountBatchEntries,
          kBootScanBatchEntries,
          kBootCoverGenerateBatchEntries,
          GetLowerExt,
          [&](const std::string &doc_path) {
            const std::string ext = GetLowerExt(doc_path);
            if (ext == ".pdf") return pdf_runtime.HasRealRenderer();
            if (ext == ".epub") return true;
            if (ext == ".zip" || ext == ".cbz") return zip_image_runtime.HasRealRenderer();
            return false;
          },
          has_manual_cover_exact_or_fuzzy,
          has_cached_doc_cover_on_disk,
          create_doc_first_page_cover_texture,
          [&](SDL_Texture *generated) {
            forget_texture_size(generated);
            SDL_DestroyTexture(generated);
          },
          [&]() { return boot_scene.InstallPendingUpdateFromEnvironment(); },
          [&]() { boot_scene.RestartAfterInstalledUpdate(); },
          [&](size_t total_books, size_t cover_generate_count) {
            boot_scene.FinishScanAndEnterShelf(
                total_books,
                cover_generate_count,
                BootSceneFinishDeps{
                    BootSceneShelfResetDeps{
                        shelf_state,
                        kTitleMarqueePauseSec,
                    },
                    rebuild_shelf_items,
                    [&]() { app_shell.Scenes().EnterShelf(); },
                    verbose_log,
                });
          },
      };
      boot_scene.Tick(dt, boot_tick_deps);
    } else if (state == AppScene::Shelf) {
      const NativeConfig &ui_cfg = config.Get();
      ShelfSceneInputContext shelf_input_context{
          input,
          shelf_runtime,
          shelf_state,
          ShelfGridCols(),
          dt,
          ui_cfg.animations,
          kPageSlideDurationSec,
          kTitleMarqueePauseSec,
          ScaleFloat(kTitleMarqueeSpeedPx),
          make_shelf_scene_input_services(),
      };
      shelf_scene.HandleInput(shelf_input_context);
    } else if (state == AppScene::Settings) {
      const NativeConfig &ui_cfg = config.Get();
      if (menu_scene.IsSelected(menu_state, SettingId::SystemControls)) {
        system_control_service.Refresh(system_settings_state.levels);
      }
      SyncContributorAvatarState(contributor_avatar_state, contributor_avatar_entries.size());
      MenuSceneInputContext menu_input_context{
          input,
          ui_cfg,
          dt,
          menu_state,
          system_settings_state,
          txt_settings_state,
          contributor_avatar_state,
          contributor_avatar_entries.size(),
          version_update_state,
          make_menu_scene_input_services(),
      };
      menu_scene.HandleInput(menu_input_context);
    } else if (state == AppScene::Reader) {
      ReaderSceneInputDeps reader_input_deps{
          input,
          reader_ui,
          progress,
          &reader_manager,
          pdf_runtime,
          epub_runtime,
          zip_image_runtime,
          dt,
          make_reader_progress_input_config(),
          transient_message_dismissed_this_frame,
          make_reader_scene_input_services(),
      };
      reader_scene.HandleInput(reader_input_deps);
    }

    shelf_state.any_grid_animating = false;
    if (animate_enabled) {
      app_shell.TickSceneFlash(dt, true);
      shelf_state.page_slide.Update(dt);
      if (shelf_state.page_animating && !shelf_state.page_slide.IsAnimating() &&
          shelf_state.page_slide.Value() >= 0.999f) {
        shelf_state.page_animating = false;
        shelf_state.page_slide.Snap(0.0f);
      }
    } else {
      shelf_state.page_animating = false;
      shelf_state.page_slide.Snap(0.0f);
      app_shell.TickSceneFlash(dt, false);
      if (state != AppScene::Settings) menu_scene.SnapClosed(menu_state);
    }

    // Draw
    app_shell.BeginDraw();

    if (state == AppScene::Boot) {
      BootSceneRenderDeps boot_render_deps{
          renderer,
          SystemLanguageIndexFromConfigValue(config.Get().system_language),
          Layout().screen_w,
          Layout().screen_h,
          [&](int x, int y, int w, int h, SDL_Color c, bool filled) { DrawRect(renderer, x, y, w, h, c, filled); },
          get_text_texture,
      };
      boot_scene.Draw(boot_render_deps);
    } else {
      const NativeConfig &cfg = config.Get();
      boot_runtime.language_index = SystemLanguageIndexFromConfigValue(cfg.system_language);
      const SDL_Color bg = (cfg.theme == 0) ? SDL_Color{22, 23, 29, 255} : SDL_Color{238, 237, 233, 255};
      DrawRect(renderer, 0, 0, Layout().screen_w, Layout().screen_h, bg);

      std::function<void()> draw_volume_overlay = []() {};
      std::function<void()> draw_system_status_overlay = []() {};
      if (state == AppScene::Shelf || state == AppScene::Settings) {
        draw_volume_overlay = [&]() {
          VolumeOverlayRenderDeps volume_deps{
              renderer,
              now,
              app_ui.volume_display_until,
              app_ui.volume_display_percent,
              Layout().top_bar_y,
              Layout().top_bar_h,
              [](int value) { return ScalePx(value); },
              get_text_texture,
          };
          DrawVolumeOverlay(volume_deps);
        };
        draw_system_status_overlay = [&]() {
          StatusBarRenderDeps status_deps{
              renderer,
              &system_status.Snapshot(),
              input_profile,
              config.Get().theme,
              Layout().screen_w,
              Layout().top_bar_y,
              Layout().top_bar_h,
              avatar_badge.BadgeTexture(),
              [](int value) { return ScalePx(value); },
              [&](int x, int y, int w, int h, SDL_Color c, bool filled) { DrawRect(renderer, x, y, w, h, c, filled); },
              get_text_texture,
          };
          DrawStatusBarRuntime(status_deps);
        };
        ShelfSceneRenderContext shelf_render_context{
            renderer,
            ui_assets,
#ifdef HAVE_SDL2_TTF
            &ui_text_cache,
#else
            nullptr,
#endif
            shelf_runtime,
            shelf_state,
            shelf_render_cache,
            MakeShelfSceneLayoutMetrics(Layout()),
            ShelfGridCols(),
            ShelfItemsPerPage(),
            dt,
            animate_enabled,
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
            make_shelf_scene_render_services(),
        };
        shelf_scene.Draw(shelf_render_context);
        draw_system_status_overlay();

        if (state != AppScene::Settings) {
          draw_volume_overlay();
        }
      }

      if (state == AppScene::Reader) {
        ReaderSceneRenderDeps reader_render_deps{
            renderer,
            reader_ui,
            &reader_manager,
            reader_progress_deps,
            dt,
            Layout().screen_w,
            Layout().screen_h,
            GetTxtBackgroundColor(config.Get().txt_background_color),
            GetTxtFontColor(config.Get().txt_font_color),
            Layout().settings_sidebar_w,
            make_reader_progress_overlay_metrics(),
            make_reader_scene_render_services(),
        };
        reader_scene.Draw(reader_render_deps);
      }

      if (state == AppScene::Settings) {
        MenuSceneRenderContext menu_render_context{
            renderer,
            ui_assets,
            cfg,
            input_profile,
            menu_state,
            kSidebarMaskMaxAlpha,
            txt_transcode_job,
            system_settings_state,
            txt_settings_state,
            contributor_avatar_entries,
            contributor_avatar_state,
            version_update_state,
            make_menu_scene_layout_metrics(),
            make_settings_render_services(draw_volume_overlay),
        };
        menu_scene.Draw(menu_render_context);
        draw_system_status_overlay();
      }

#ifdef HAVE_SDL2_TTF
      if (!transient_message.empty() && now < transient_message_until) {
        const SDL_Color text_color{245, 245, 245, 255};
        if (TextCacheEntry *te = get_text_texture(transient_message, text_color); te && te->texture) {
          const int pad_x = ScalePx(24);
          const int pad_y = ScalePx(12);
          const int box_w = te->w + pad_x * 2;
          const int box_h = te->h + pad_y * 2;
          const int box_x = std::max(0, (Layout().screen_w - box_w) / 2);
          const int box_y = std::max(0, Layout().screen_h - box_h - ScalePx(72));
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

    app_shell.DrawSceneFlash();

    app_shell.Present();

    app_shell.ThrottleFrame(frame_begin_ticks, contributor_marquee_active, has_active_animation, needs_periodic_tick);
  }

  if (!current_book.empty()) {
    if (reader_mode == ReaderMode::Pdf && pdf_runtime.IsOpen()) {
      const PdfRuntimeProgress active_pdf = pdf_runtime.Progress();
      reader.page = active_pdf.page;
      reader.scroll_x = active_pdf.scroll_x;
      reader.scroll_y = active_pdf.scroll_y;
      reader.zoom = active_pdf.zoom;
      reader.rotation = active_pdf.rotation;
    } else if (reader_mode == ReaderMode::Epub && reader_manager.Module(ReaderMode::Epub)->IsOpen()) {
      const IReaderModule *epub_module = reader_manager.Module(ReaderMode::Epub);
      const ReaderProgress active_epub = epub_module->Progress();
      reader.page = active_epub.page;
      reader.scroll_x = std::string(epub_module->BackendName()) == "epub-flow" ? 0 : active_epub.scroll_x;
      reader.scroll_y = active_epub.scroll_y;
      reader.zoom = active_epub.zoom;
      reader.rotation = active_epub.rotation;
    } else if (reader_mode == ReaderMode::ZipImage && zip_image_runtime.IsOpen()) {
      const ZipImageRuntimeProgress active_zip = zip_image_runtime.Progress();
      reader.page = active_zip.page;
      reader.scroll_x = active_zip.scroll_x;
      reader.scroll_y = active_zip.scroll_y;
      reader.zoom = active_zip.zoom;
      reader.rotation = active_zip.rotation;
    } else if (reader_mode == ReaderMode::Txt && reader_ui.Txt().open) {
      reader = txt_reader_module.Progress();
      reader_ui.Txt().resume_cache_dirty = true;
      persist_current_txt_resume_snapshot(current_book, true);
    } else if (state != AppScene::Reader) {
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
  avatar_badge.Shutdown();
  DestroyContributorAvatarEntries(contributor_avatar_entries, forget_texture_size);
  DestroyUiAssets(ui_assets, forget_texture_size);
#ifdef HAVE_SDL2_TTF
  ShutdownUiTextCache(ui_text_cache, forget_texture_size);
#endif
  destroy_shelf_render_cache();
  pdf_runtime.Close();
  epub_runtime.Close();
  zip_image_runtime.Close();
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

