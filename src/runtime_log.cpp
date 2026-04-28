#include "runtime_log.h"

#include "filesystem_compat.h"

#include <chrono>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <iostream>
#include <mutex>
#include <sstream>
#include <string>
#include <system_error>

namespace fs = std::filesystem;

namespace {
std::mutex g_log_mutex;
fs::path g_log_path;
bool g_echo_stderr = false;

bool EnvEnabled(const char *name) {
  const char *value = std::getenv(name);
  return value && *value && std::string(value) != "0";
}

std::string LowerAscii(std::string text) {
  for (char &ch : text) {
    if (ch >= 'A' && ch <= 'Z') ch = static_cast<char>(ch - 'A' + 'a');
  }
  return text;
}

bool VerboseRuntimeLogEnabled() {
  return EnvEnabled("ROCREADER_RUNTIME_LOG") || EnvEnabled("ROCREADER_VERBOSE_LOG") ||
         EnvEnabled("ROCREADER_DEBUG_LOG");
}

bool DefaultRuntimeLogKeep(const std::string &message) {
  const std::string lower = LowerAscii(message);
  constexpr const char *kNeedles[] = {
      "fatal",
      "crash",
      "signal",
      "exception",
      "failed",
      "error",
      "warning",
      "timeout",
      "blocked",
      "cannot ",
      "invalid",
  };
  for (const char *needle : kNeedles) {
    if (lower.find(needle) != std::string::npos) return true;
  }
  return false;
}

std::string NowText() {
  const auto now = std::chrono::system_clock::now();
  const std::time_t t = std::chrono::system_clock::to_time_t(now);
  std::tm tm{};
#ifdef _WIN32
  localtime_s(&tm, &t);
#else
  localtime_r(&t, &tm);
#endif
  char buf[32]{};
  std::strftime(buf, sizeof(buf), "%F %T", &tm);
  return buf;
}

fs::path ChooseLogPath(const char *argv0) {
  if (const char *env = std::getenv("ROC_NATIVE_RUNTIME_LOG"); env && *env) {
    return fs::path(env);
  }
  if (const char *root = std::getenv("ROCREADER_ROOT"); root && *root) {
    return fs::path(root) / "log.txt";
  }
  std::error_code ec;
  fs::path cwd = fs::current_path(ec);
  if (!ec && !cwd.empty()) {
    return cwd / "log.txt";
  }
  if (argv0 && *argv0) {
    fs::path exe(argv0);
    if (!exe.parent_path().empty()) {
      return exe.parent_path() / "log.txt";
    }
  }
  return fs::path("log.txt");
}
} // namespace

namespace runtime_log {

void Init(const char *argv0) {
  std::lock_guard<std::mutex> lock(g_log_mutex);
  g_log_path = ChooseLogPath(argv0);
  if (const char *env = std::getenv("ROCREADER_LOG_STDERR"); env && std::string(env) == "1") {
    g_echo_stderr = true;
  }
  std::error_code ec;
  fs::create_directories(g_log_path.parent_path(), ec);
  std::ofstream out(g_log_path, std::ios::app);
  if (out) {
    out << "\n===== ROCreader start " << NowText() << " =====\n";
  }
  if (g_echo_stderr) {
    std::cerr << "[runtime] log path: " << filesystem_compat::LexicallyNormal(g_log_path).string() << "\n";
  }
}

void Line(const std::string &message) {
  if (!VerboseRuntimeLogEnabled() && !DefaultRuntimeLogKeep(message)) return;
  std::lock_guard<std::mutex> lock(g_log_mutex);
  const std::string line = "[" + NowText() + "] " + message;
  if (g_echo_stderr) {
    std::cerr << line << "\n";
  }
  if (g_log_path.empty()) {
    g_log_path = ChooseLogPath(nullptr);
  }
  std::ofstream out(g_log_path, std::ios::app);
  if (out) {
    out << line << "\n";
    out.flush();
  }
}

void Line(const char *message) {
  Line(std::string(message ? message : ""));
}

std::string Path() {
  std::lock_guard<std::mutex> lock(g_log_mutex);
  return g_log_path.empty() ? std::string() : filesystem_compat::LexicallyNormal(g_log_path).string();
}

} // namespace runtime_log
