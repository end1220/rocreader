#include "version_update_runtime.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include <system_error>
#include <vector>

namespace {
constexpr int kButtonWidth = 168;
constexpr int kButtonHeight = 34;
constexpr int kProgressBarWidth = 228;
constexpr int kProgressBarHeight = 16;
constexpr const char *kGithubContentsApi =
    "https://api.github.com/repos/LPF970915/ROCreader/contents/Downloads?ref=main";
constexpr const char *kPendingMarkerFilename = "ROCreader_update_pending.txt";
constexpr const char *kUserAgent = "ROCreader-Updater";

void DrawCenteredText(SDL_Renderer *renderer, const SDL_Rect &bounds,
                      const std::function<TextCacheEntry *(const std::string &, SDL_Color)> &get_text_texture,
                      const std::string &text, SDL_Color color, int center_y) {
  if (!renderer || !get_text_texture || text.empty()) return;
  TextCacheEntry *entry = get_text_texture(text, color);
  if (!entry || !entry->texture) return;
  SDL_Rect dst{
      bounds.x + std::max(0, (bounds.w - entry->w) / 2),
      center_y - entry->h / 2,
      entry->w,
      entry->h,
  };
  SDL_RenderCopy(renderer, entry->texture, nullptr, &dst);
}

std::string ToLowerAscii(std::string text) {
  std::transform(text.begin(), text.end(), text.begin(),
                 [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
  return text;
}

bool TryExtractVersionToken(const std::string &filename, std::string &out_version) {
  const std::string lower_name = ToLowerAscii(filename);
  const size_t ver_pos = lower_name.rfind("ver");
  const size_t zip_pos = lower_name.rfind(".zip");
  if (ver_pos == std::string::npos || zip_pos == std::string::npos || ver_pos >= zip_pos) return false;
  out_version = filename.substr(ver_pos, zip_pos - ver_pos);
  return !out_version.empty();
}

std::vector<int> ParseVersionParts(const std::string &version_text) {
  std::vector<int> parts;
  int value = -1;
  for (char ch : version_text) {
    if (std::isdigit(static_cast<unsigned char>(ch))) {
      if (value < 0) value = 0;
      value = value * 10 + (ch - '0');
    } else if (value >= 0) {
      parts.push_back(value);
      value = -1;
    }
  }
  if (value >= 0) parts.push_back(value);
  return parts;
}

bool IsVersionNewer(const std::string &candidate, const std::string &baseline) {
  const std::vector<int> lhs = ParseVersionParts(candidate);
  const std::vector<int> rhs = ParseVersionParts(baseline);
  const size_t count = std::max(lhs.size(), rhs.size());
  for (size_t i = 0; i < count; ++i) {
    const int lv = i < lhs.size() ? lhs[i] : 0;
    const int rv = i < rhs.size() ? rhs[i] : 0;
    if (lv != rv) return lv > rv;
  }
  return false;
}

std::string EscapeForPosix(const std::string &value) {
  std::string escaped = "'";
  for (char ch : value) {
    if (ch == '\'') escaped += "'\\''";
    else escaped.push_back(ch);
  }
  escaped += "'";
  return escaped;
}

std::string EscapeForPowerShell(const std::string &value) {
  std::string escaped;
  escaped.reserve(value.size() + 8);
  for (char ch : value) {
    if (ch == '\'') escaped += "''";
    else escaped.push_back(ch);
  }
  return escaped;
}

std::string RunCommandCapture(const std::string &command) {
#if defined(_WIN32)
  FILE *pipe = _popen(command.c_str(), "r");
#else
  FILE *pipe = popen(command.c_str(), "r");
#endif
  if (!pipe) return {};

  std::string output;
  std::array<char, 512> buffer{};
  while (std::fgets(buffer.data(), static_cast<int>(buffer.size()), pipe) != nullptr) {
    output += buffer.data();
  }

#if defined(_WIN32)
  _pclose(pipe);
#else
  pclose(pipe);
#endif
  return output;
}

int RunCommand(const std::string &command) {
  return std::system(command.c_str());
}

std::string HttpGetText(const std::string &url) {
#if defined(_WIN32)
  const std::string escaped_url = EscapeForPowerShell(url);
  const std::string command =
      "powershell -NoProfile -Command \"[Net.ServicePointManager]::SecurityProtocol = "
      "[Net.SecurityProtocolType]::Tls12; "
      "$ProgressPreference='SilentlyContinue'; "
      "(Invoke-WebRequest -UseBasicParsing -Headers @{ 'User-Agent'='" + std::string(kUserAgent) + "' } "
      "-Uri '" + escaped_url + "').Content\"";
  return RunCommandCapture(command);
#else
  const std::string curl_command =
      "curl -LfsS -H " + EscapeForPosix(std::string("User-Agent: ") + kUserAgent) + " "
      + EscapeForPosix(url) + " 2>/dev/null";
  std::string output = RunCommandCapture(curl_command);
  if (!output.empty()) return output;
  const std::string wget_command =
      "wget -qO- --header=" + EscapeForPosix(std::string("User-Agent: ") + kUserAgent) + " "
      + EscapeForPosix(url) + " 2>/dev/null";
  return RunCommandCapture(wget_command);
#endif
}

bool DownloadFile(const std::string &url, const std::filesystem::path &output_path) {
#if defined(_WIN32)
  const std::string escaped_url = EscapeForPowerShell(url);
  const std::string escaped_path = EscapeForPowerShell(output_path.string());
  const std::string command =
      "powershell -NoProfile -Command \"[Net.ServicePointManager]::SecurityProtocol = "
      "[Net.SecurityProtocolType]::Tls12; "
      "$ProgressPreference='SilentlyContinue'; "
      "Invoke-WebRequest -UseBasicParsing -Headers @{ 'User-Agent'='" + std::string(kUserAgent) + "' } "
      "-Uri '" + escaped_url + "' -OutFile '" + escaped_path + "'\"";
  return RunCommand(command) == 0;
#else
  const std::string curl_command =
      "curl -LfsS -H " + EscapeForPosix(std::string("User-Agent: ") + kUserAgent) + " "
      + EscapeForPosix(url) + " -o " + EscapeForPosix(output_path.string()) + " 2>/dev/null";
  if (RunCommand(curl_command) == 0) return true;
  const std::string wget_command =
      "wget -q --header=" + EscapeForPosix(std::string("User-Agent: ") + kUserAgent) + " "
      + EscapeForPosix(url) + " -O " + EscapeForPosix(output_path.string()) + " 2>/dev/null";
  return RunCommand(wget_command) == 0;
#endif
}

bool ExtractJsonStringField(const std::string &object_text, const std::string &field_name, std::string &out_value) {
  const std::string key = "\"" + field_name + "\"";
  const size_t key_pos = object_text.find(key);
  if (key_pos == std::string::npos) return false;
  const size_t colon_pos = object_text.find(':', key_pos + key.size());
  if (colon_pos == std::string::npos) return false;
  const size_t first_quote = object_text.find('"', colon_pos + 1);
  if (first_quote == std::string::npos) return false;
  size_t cursor = first_quote + 1;
  std::string value;
  while (cursor < object_text.size()) {
    const char ch = object_text[cursor++];
    if (ch == '\\' && cursor < object_text.size()) {
      value.push_back(object_text[cursor++]);
      continue;
    }
    if (ch == '"') {
      out_value = value;
      return true;
    }
    value.push_back(ch);
  }
  return false;
}

bool ExtractJsonNumberField(const std::string &object_text, const std::string &field_name, uint64_t &out_value) {
  const std::string key = "\"" + field_name + "\"";
  const size_t key_pos = object_text.find(key);
  if (key_pos == std::string::npos) return false;
  const size_t colon_pos = object_text.find(':', key_pos + key.size());
  if (colon_pos == std::string::npos) return false;
  size_t cursor = colon_pos + 1;
  while (cursor < object_text.size() && std::isspace(static_cast<unsigned char>(object_text[cursor])) != 0) ++cursor;
  size_t end = cursor;
  while (end < object_text.size() && std::isdigit(static_cast<unsigned char>(object_text[end])) != 0) ++end;
  if (end == cursor) return false;
  try {
    out_value = static_cast<uint64_t>(std::stoull(object_text.substr(cursor, end - cursor)));
    return true;
  } catch (...) {
    return false;
  }
}

struct RemoteArchiveInfo {
  std::string filename;
  std::string version;
  std::string download_url;
  uint64_t size_bytes = 0;
};

bool FetchLatestRemoteArchive(RemoteArchiveInfo &out_info) {
  const std::string json = HttpGetText(kGithubContentsApi);
  if (json.empty()) return false;

  size_t search_pos = 0;
  bool found = false;
  RemoteArchiveInfo best{};
  while (true) {
    const size_t name_key_pos = json.find("\"name\"", search_pos);
    if (name_key_pos == std::string::npos) break;
    const size_t object_start = json.rfind('{', name_key_pos);
    const size_t object_end = json.find('}', name_key_pos);
    if (object_start == std::string::npos || object_end == std::string::npos || object_end <= object_start) {
      search_pos = name_key_pos + 6;
      continue;
    }
    const std::string object_text = json.substr(object_start, object_end - object_start + 1);
    search_pos = object_end + 1;

    RemoteArchiveInfo candidate{};
    if (!ExtractJsonStringField(object_text, "name", candidate.filename)) continue;
    std::string lower_name = ToLowerAscii(candidate.filename);
    if (lower_name.size() < 4 || lower_name.substr(lower_name.size() - 4) != ".zip") continue;
    if (!TryExtractVersionToken(candidate.filename, candidate.version)) continue;
    if (!ExtractJsonStringField(object_text, "download_url", candidate.download_url)) continue;
    if (!ExtractJsonNumberField(object_text, "size", candidate.size_bytes)) continue;

    if (!found || IsVersionNewer(candidate.version, best.version)) {
      best = std::move(candidate);
      found = true;
    }
  }

  if (!found) return false;
  out_info = std::move(best);
  return true;
}

std::filesystem::path DetectDownloadRoot() {
  const std::vector<std::string> card_roots = storage_paths::DetectStorageCardRoots();
  for (const std::string &root : card_roots) {
    if (root.empty()) continue;
    return std::filesystem::path(root);
  }
  std::error_code ec;
  const std::filesystem::path cwd = std::filesystem::current_path(ec);
  if (!ec) return cwd;
  return {};
}

std::filesystem::path PendingDownloadsDir(const std::filesystem::path &download_root) {
  return download_root / "Downloads";
}

std::filesystem::path PendingMarkerPath(const std::filesystem::path &download_root) {
  return PendingDownloadsDir(download_root) / kPendingMarkerFilename;
}

bool ReadPendingMarker(const std::filesystem::path &marker_path, VersionUpdateState &state) {
  std::ifstream in(marker_path);
  if (!in) return false;
  std::string line;
  std::filesystem::path package_path;
  std::string version;
  while (std::getline(in, line)) {
    const size_t eq = line.find('=');
    if (eq == std::string::npos) continue;
    const std::string key = line.substr(0, eq);
    const std::string value = line.substr(eq + 1);
    if (key == "filename") package_path = marker_path.parent_path() / value;
    else if (key == "version") version = value;
  }
  std::error_code ec;
  if (package_path.empty() || !std::filesystem::exists(package_path, ec) || ec) return false;
  state.pending_package_path = package_path;
  state.latest_version = version;
  state.has_pending_package = true;
  state.status = VersionUpdateStatus::Downloaded;
  state.download_in_progress = false;
  state.download_progress_pct = 100;
  return true;
}

bool WritePendingMarker(const VersionUpdateState &state) {
  if (state.pending_marker_path.empty() || state.pending_package_path.empty()) return false;
  std::ofstream out(state.pending_marker_path, std::ios::trunc);
  if (!out) return false;
  out << "filename=" << state.pending_package_path.filename().string() << "\n";
  out << "version=" << state.latest_version << "\n";
  out << "size=" << state.expected_download_bytes << "\n";
  return static_cast<bool>(out);
}

void JoinDownloadThread(VersionUpdateState &state) {
  if (state.download_thread.joinable()) state.download_thread.join();
}

bool BeginVersionUpdateDownloadInternal(VersionUpdateState &state) {
  JoinDownloadThread(state);

  state.download_root = DetectDownloadRoot();
  if (state.download_root.empty()) {
    state.status = VersionUpdateStatus::NoNetwork;
    return false;
  }
  state.pending_marker_path = PendingMarkerPath(state.download_root);
  if (ReadPendingMarker(state.pending_marker_path, state)) {
    return true;
  }

  RemoteArchiveInfo remote_info{};
  if (!FetchLatestRemoteArchive(remote_info)) {
    state.status = VersionUpdateStatus::NoNetwork;
    state.download_in_progress = false;
    return false;
  }

  state.latest_version = remote_info.version;
  const bool has_newer_remote = IsVersionNewer(remote_info.version, state.current_version);
  if (!has_newer_remote) {
    state.status = VersionUpdateStatus::UpToDate;
    state.download_in_progress = false;
    state.download_progress_pct = 0;
    return true;
  }

  const std::filesystem::path downloads_dir = PendingDownloadsDir(state.download_root);
  std::error_code ec;
  std::filesystem::create_directories(downloads_dir, ec);
  if (ec) {
    state.status = VersionUpdateStatus::DownloadFailed;
    return false;
  }

  state.active_download_filename = remote_info.filename;
  state.active_download_url = remote_info.download_url;
  state.expected_download_bytes = remote_info.size_bytes;
  state.pending_package_path = downloads_dir / remote_info.filename;
  state.temp_package_path = downloads_dir / (remote_info.filename + ".download");

  if (std::filesystem::exists(state.pending_package_path, ec) && !ec) {
    const uint64_t existing_size = std::filesystem::file_size(state.pending_package_path, ec);
    if (!ec && existing_size == state.expected_download_bytes) {
      state.has_pending_package = true;
      state.status = VersionUpdateStatus::Downloaded;
      state.download_in_progress = false;
      state.download_progress_pct = 100;
      WritePendingMarker(state);
      return true;
    }
  }
  ec.clear();
  std::filesystem::remove(state.temp_package_path, ec);

  state.download_thread_done = false;
  state.download_thread_success = false;
  state.download_progress_pct = 0;
  state.has_pending_package = false;
  state.download_in_progress = true;
  state.status = VersionUpdateStatus::Downloading;

  const std::string download_url = state.active_download_url;
  const std::filesystem::path temp_path = state.temp_package_path;
  const std::filesystem::path final_path = state.pending_package_path;

  state.download_thread = std::thread([download_url, temp_path, final_path, &state]() {
    bool success = DownloadFile(download_url, temp_path);
    if (success) {
      std::error_code move_ec;
      std::filesystem::remove(final_path, move_ec);
      move_ec.clear();
      std::filesystem::rename(temp_path, final_path, move_ec);
      success = !move_ec;
      if (!success) {
        move_ec.clear();
        std::filesystem::remove(temp_path, move_ec);
      }
    }
    state.download_thread_success = success;
    state.download_thread_done = true;
  });

  return true;
}
} // namespace

bool BeginVersionUpdateDownload(VersionUpdateState &state) {
  return BeginVersionUpdateDownloadInternal(state);
}

bool HandleVersionUpdateInput(const InputManager &input, VersionUpdateState &state,
                              const VersionUpdateCallbacks &callbacks) {
  if (!state.panel_active) {
    if (input.IsJustPressed(Button::A) || input.IsJustPressed(Button::Right)) {
      state.panel_active = true;
      return true;
    }
    return false;
  }

  if (input.IsJustPressed(Button::B)) {
    state.panel_active = false;
    return true;
  }

  if (state.download_in_progress) return false;

  if ((input.IsJustPressed(Button::A) || input.IsJustPressed(Button::Right)) && callbacks.start_check_and_update) {
    callbacks.start_check_and_update(state);
    return true;
  }

  return false;
}

void InitializeVersionUpdateState(VersionUpdateState &state) {
  state.download_root = DetectDownloadRoot();
  if (state.download_root.empty()) return;
  state.pending_marker_path = PendingMarkerPath(state.download_root);
  ReadPendingMarker(state.pending_marker_path, state);
}

void TickVersionUpdateState(VersionUpdateState &state, float dt) {
  (void)dt;
  if (state.download_in_progress && !state.temp_package_path.empty()) {
    std::error_code ec;
    const uint64_t size = std::filesystem::exists(state.temp_package_path, ec)
                              ? std::filesystem::file_size(state.temp_package_path, ec)
                              : 0;
    if (!ec && state.expected_download_bytes > 0) {
      state.download_progress_pct = std::clamp(
          static_cast<int>((size * 100ull) / state.expected_download_bytes), 0, 99);
    }
  }

  if (!state.download_thread_done) return;

  JoinDownloadThread(state);
  state.download_in_progress = false;
  if (state.download_thread_success) {
    state.has_pending_package = true;
    state.status = VersionUpdateStatus::Downloaded;
    state.download_progress_pct = 100;
    WritePendingMarker(state);
  } else if (state.status == VersionUpdateStatus::Downloading) {
    state.status = VersionUpdateStatus::DownloadFailed;
    state.download_progress_pct = 0;
  }
  state.download_thread_done = false;
}

void ShutdownVersionUpdateState(VersionUpdateState &state) {
  JoinDownloadThread(state);
}

void DrawVersionUpdatePreview(const VersionUpdateRenderDeps &deps) {
  if (!deps.renderer) return;

  const SDL_Color text_color = deps.light_theme ? SDL_Color{44, 50, 60, 255} : SDL_Color{236, 241, 247, 255};
  const SDL_Color muted_color = deps.light_theme ? SDL_Color{113, 120, 130, 255} : SDL_Color{155, 168, 182, 255};
  const SDL_Color button_fill = deps.light_theme ? SDL_Color{225, 233, 241, 248} : SDL_Color{33, 71, 100, 236};
  const SDL_Color button_border = deps.light_theme ? SDL_Color{86, 117, 146, 255} : SDL_Color{122, 201, 255, 255};
  const SDL_Color button_active = deps.light_theme ? SDL_Color{204, 226, 240, 255} : SDL_Color{46, 96, 132, 255};
  const SDL_Color slot_fill = deps.light_theme ? SDL_Color{206, 212, 220, 255} : SDL_Color{62, 77, 92, 255};
  const SDL_Color slot_active = deps.light_theme ? SDL_Color{72, 122, 164, 255} : SDL_Color{122, 201, 255, 255};
  const SDL_Color success_color = deps.light_theme ? SDL_Color{54, 132, 94, 255} : SDL_Color{116, 224, 165, 255};

  const int center_x = deps.preview_rect.x + deps.preview_rect.w / 2;
  const int content_height = 162;
  const int content_top = deps.preview_rect.y + std::max(0, (deps.preview_rect.h - content_height) / 2);
  const int line1_y = content_top + 18;
  const int line2_y = content_top + 76;
  const int line3_y = content_top + 136;
  const int line4_y = content_top + 162;

  DrawCenteredText(deps.renderer,
                   deps.preview_rect,
                   deps.get_emphasis_text_texture ? deps.get_emphasis_text_texture : deps.get_text_texture,
                   std::string(u8"\u5f53\u524d\u7248\u672c ") + deps.state.current_version,
                   text_color,
                   line1_y);

  const bool button_selected = deps.state.panel_active && !deps.state.download_in_progress;
  const int button_x = center_x - kButtonWidth / 2;
  const int button_y = line2_y - kButtonHeight / 2;
  deps.draw_rect(button_x, button_y, kButtonWidth, kButtonHeight,
                 button_selected ? button_active : button_fill, true);
  deps.draw_rect(button_x, button_y, kButtonWidth, kButtonHeight, button_border, false);
  DrawCenteredText(deps.renderer,
                   SDL_Rect{button_x, button_y, kButtonWidth, kButtonHeight},
                   deps.get_text_texture,
                   std::string(u8"\u68c0\u6d4b\u5e76\u66f4\u65b0"),
                   text_color,
                   line2_y);

  switch (deps.state.status) {
  case VersionUpdateStatus::Idle:
    DrawCenteredText(deps.renderer,
                     deps.preview_rect,
                     deps.get_text_texture,
                     std::string(u8"\u6309 A \u5f00\u59cb\u68c0\u67e5\u66f4\u65b0"),
                     muted_color,
                     line3_y);
    break;
  case VersionUpdateStatus::NoNetwork:
    DrawCenteredText(deps.renderer,
                     deps.preview_rect,
                     deps.get_text_texture,
                     std::string(u8"\u6ca1\u6709\u68c0\u6d4b\u5230\u7f51\u7edc\u8fde\u63a5"),
                     muted_color,
                     line3_y);
    break;
  case VersionUpdateStatus::Downloading: {
    const int bar_x = center_x - kProgressBarWidth / 2;
    const int bar_y = line3_y - kProgressBarHeight / 2 + 8;
    deps.draw_rect(bar_x, bar_y, kProgressBarWidth, kProgressBarHeight, slot_fill, true);
    const int fill_w = std::clamp((kProgressBarWidth * deps.state.download_progress_pct) / 100, 0, kProgressBarWidth);
    if (fill_w > 0) {
      deps.draw_rect(bar_x, bar_y, fill_w, kProgressBarHeight, slot_active, true);
    }
    deps.draw_rect(bar_x, bar_y, kProgressBarWidth, kProgressBarHeight, button_border, false);
    DrawCenteredText(deps.renderer,
                     deps.preview_rect,
                     deps.get_text_texture,
                     std::string(u8"\u68c0\u6d4b\u5230\u66f4\u65b0\uff0c\u6b63\u5728\u4e0b\u8f7d ")
                         + std::to_string(deps.state.download_progress_pct) + "%",
                     text_color,
                     line3_y - 20);
    break;
  }
  case VersionUpdateStatus::Downloaded:
    DrawCenteredText(deps.renderer,
                     deps.preview_rect,
                     deps.get_text_texture,
                     std::string(u8"\u5df2\u4e0b\u8f7d\u5b89\u88c5\u5305"),
                     success_color,
                     line3_y);
    DrawCenteredText(deps.renderer,
                     deps.preview_rect,
                     deps.get_text_texture,
                     std::string(u8"\u91cd\u542f\u5b89\u88c5"),
                     success_color,
                     line4_y);
    break;
  case VersionUpdateStatus::UpToDate:
    DrawCenteredText(deps.renderer,
                     deps.preview_rect,
                     deps.get_text_texture,
                     std::string(u8"\u5df2\u662f\u6700\u65b0\u7248\u672c"),
                     muted_color,
                     line3_y);
    break;
  case VersionUpdateStatus::DownloadFailed:
    DrawCenteredText(deps.renderer,
                     deps.preview_rect,
                     deps.get_text_texture,
                     std::string(u8"\u4e0b\u8f7d\u5931\u8d25\uff0c\u8bf7\u7a0d\u540e\u91cd\u8bd5"),
                     muted_color,
                     line3_y);
    break;
  }
}
