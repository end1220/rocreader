#include "lid_power_control.h"

#include <cstdlib>
#include <utility>

std::string EscapeShellArg(const std::filesystem::path &path) {
  std::string text = path.string();
  std::string escaped = "'";
  for (char ch : text) {
    if (ch == '\'') escaped += "'\\''";
    else escaped += ch;
  }
  escaped += "'";
  return escaped;
}

LidPowerController::LidPowerController(std::filesystem::path power_script_path)
    : power_script_path_(std::move(power_script_path)) {}

bool LidPowerController::Enabled() const { return enabled_; }

void LidPowerController::SetEnabled(bool enabled) { enabled_ = enabled; }

bool LidPowerController::ScriptAvailable() const {
  std::error_code ec;
  return !power_script_path_.empty() && std::filesystem::exists(power_script_path_, ec) && !ec;
}

bool LidPowerController::TriggerAutoIfEnabled() const {
  if (!enabled_ || !ScriptAvailable()) return false;
  const std::string command = EscapeShellArg(power_script_path_) + " auto >/dev/null 2>&1";
  const int rc = std::system(command.c_str());
  return rc == 0;
}

std::string LidPowerController::PowerScriptPath() const { return power_script_path_.string(); }
