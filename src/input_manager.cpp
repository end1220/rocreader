#include "input_manager.h"

#include <cctype>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <cmath>
#include <sstream>

#if !defined(_WIN32)
#include <cerrno>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <linux/input.h>
#include <unistd.h>
#endif

const char *ButtonName(Button b) {
  switch (b) {
  case Button::Up: return "Up";
  case Button::Down: return "Down";
  case Button::Left: return "Left";
  case Button::Right: return "Right";
  case Button::A: return "A";
  case Button::B: return "B";
  case Button::X: return "X";
  case Button::Y: return "Y";
  case Button::Menu: return "Menu";
  case Button::L1: return "L1";
  case Button::L2: return "L2";
  case Button::R1: return "R1";
  case Button::R2: return "R2";
  case Button::Start: return "Start";
  case Button::Select: return "Select";
  case Button::VolUp: return "VolUp";
  case Button::VolDown: return "VolDown";
  case Button::Power: return "Power";
  default: return "Invalid";
  }
}

const char *ButtonNameOrInvalid(Button b) {
  return InputManager::IsValid(b) ? ButtonName(b) : "Invalid";
}

bool FullInputLogEnabled() {
  const char *value = std::getenv("ROCREADER_FULL_INPUT_LOG");
  return value && *value && std::string(value) != "0";
}

const char *InputProfileName(InputProfile profile) {
  switch (profile) {
  case InputProfile::DesktopDefault: return "desktop-default";
  case InputProfile::H700Default: return "h700-default";
  case InputProfile::H70034xxSp: return "h700-34xxsp";
  case InputProfile::H70035xxH: return "h700-35xxh";
  case InputProfile::TrimuiBrick: return "trimui-brick";
  default: return "unknown";
  }
}

const char *SdlEventName(Uint32 type) {
  switch (type) {
  case SDL_KEYDOWN: return "SDL_KEYDOWN";
  case SDL_KEYUP: return "SDL_KEYUP";
  case SDL_CONTROLLERBUTTONDOWN: return "SDL_CONTROLLERBUTTONDOWN";
  case SDL_CONTROLLERBUTTONUP: return "SDL_CONTROLLERBUTTONUP";
  case SDL_CONTROLLERAXISMOTION: return "SDL_CONTROLLERAXISMOTION";
  case SDL_JOYBUTTONDOWN: return "SDL_JOYBUTTONDOWN";
  case SDL_JOYBUTTONUP: return "SDL_JOYBUTTONUP";
  case SDL_JOYHATMOTION: return "SDL_JOYHATMOTION";
  case SDL_JOYAXISMOTION: return "SDL_JOYAXISMOTION";
  case SDL_CONTROLLERDEVICEADDED: return "SDL_CONTROLLERDEVICEADDED";
  case SDL_CONTROLLERDEVICEREMOVED: return "SDL_CONTROLLERDEVICEREMOVED";
  case SDL_JOYDEVICEADDED: return "SDL_JOYDEVICEADDED";
  case SDL_JOYDEVICEREMOVED: return "SDL_JOYDEVICEREMOVED";
  default: return "SDL_EVENT_UNKNOWN";
  }
}

InputManager::InputManager(const std::string &mapping_path, InputProfile input_profile) {
  input_profile_ = input_profile;
  full_input_log_enabled_ = FullInputLogEnabled();
  pad_map_.fill(InvalidButton());
  joy_map_.fill(InvalidButton());
  LoadDefaultPadMap(input_profile);
  LoadDefaultJoyMap(input_profile);
  LoadOverrides(mapping_path);
  OpenLinuxInputDevices();
}

InputManager::~InputManager() {
  CloseLinuxInputDevices();
}

void InputManager::BeginFrame(float dt) {
  dt_ = dt;
  for (auto &s : states_) {
    s.just_pressed = false;
    s.just_released = false;
    s.repeated = false;
    s.long_pressed = false;
  }
}

void InputManager::HandleEvent(const SDL_Event &e) {
  if (e.type == SDL_KEYDOWN && !e.key.repeat) {
    const Button mapped = KeyToButton(e.key.keysym.sym);
    if (ShouldLogProbeKey(e.key.keysym.scancode)) {
      std::cout << "[native_h700] input probe: type=" << SdlEventName(e.type)
                << " key=" << static_cast<int>(e.key.keysym.sym)
                << " scancode=" << static_cast<int>(e.key.keysym.scancode)
                << " mapped=" << ButtonNameOrInvalid(mapped) << "\n";
    }
    SetDown(mapped, true);
  } else if (e.type == SDL_KEYUP) {
    const Button mapped = KeyToButton(e.key.keysym.sym);
    SetDown(mapped, false);
  } else if (e.type == SDL_CONTROLLERBUTTONDOWN) {
    const Button mapped = PadToButton(e.cbutton.button);
    if (ShouldLogProbePadButton(e.cbutton.button)) {
      std::cout << "[native_h700] input probe: type=" << SdlEventName(e.type)
                << " pad_button=" << static_cast<int>(e.cbutton.button)
                << " mapped=" << ButtonNameOrInvalid(mapped) << "\n";
    }
    SetDown(PadToButton(e.cbutton.button), true);
  } else if (e.type == SDL_CONTROLLERBUTTONUP) {
    SetDown(PadToButton(e.cbutton.button), false);
  } else if (e.type == SDL_CONTROLLERAXISMOTION) {
    constexpr int kDeadzone = 16000;
    const int axis = e.caxis.axis;
    const int val = static_cast<int>(e.caxis.value);
    const bool valid_axis = axis >= 0 && axis < static_cast<int>(last_pad_axis_values_.size());
    const int old_val = valid_axis ? last_pad_axis_values_[static_cast<size_t>(axis)] : 0;
    const bool should_log_axis = ShouldLogProbePadAxis(axis, val);
    if (should_log_axis) {
      std::cout << "[native_h700] input probe: type=" << SdlEventName(e.type)
                << " which=" << e.caxis.which
                << " pad_axis=" << axis
                << " value=" << val
                << " old=" << old_val << "\n";
    }
    if (valid_axis) {
      last_pad_axis_values_[static_cast<size_t>(axis)] = val;
      pad_axis_seen_[static_cast<size_t>(axis)] = true;
    }
    if (axis == SDL_CONTROLLER_AXIS_LEFTX || axis == SDL_CONTROLLER_AXIS_RIGHTX) {
      SetDown(Button::Left, val < -kDeadzone);
      SetDown(Button::Right, val > kDeadzone);
    } else if (axis == SDL_CONTROLLER_AXIS_LEFTY || axis == SDL_CONTROLLER_AXIS_RIGHTY) {
      SetDown(Button::Up, val < -kDeadzone);
      SetDown(Button::Down, val > kDeadzone);
    } else if (axis == SDL_CONTROLLER_AXIS_TRIGGERLEFT) {
      SetDown(Button::L2, val > kDeadzone);
    } else if (axis == SDL_CONTROLLER_AXIS_TRIGGERRIGHT) {
      SetDown(Button::R2, val > kDeadzone);
    }
  } else if (e.type == SDL_JOYBUTTONDOWN) {
    const Button mapped = JoyButtonToButton(e.jbutton.button);
    if (ShouldLogProbeJoyButton(e.jbutton.button)) {
      std::cout << "[native_h700] input probe: type=" << SdlEventName(e.type)
                << " joy_button=" << static_cast<int>(e.jbutton.button)
                << " mapped=" << ButtonNameOrInvalid(mapped) << "\n";
    }
    SetDown(JoyButtonToButton(e.jbutton.button), true);
  } else if (e.type == SDL_JOYBUTTONUP) {
    SetDown(JoyButtonToButton(e.jbutton.button), false);
  } else if (e.type == SDL_JOYHATMOTION) {
    const uint8_t v = e.jhat.value;
    if (ShouldLogProbeHat(e.jhat.hat, v)) {
      std::cout << "[native_h700] input probe: type=" << SdlEventName(e.type)
                << " hat=" << static_cast<int>(e.jhat.hat)
                << " value=" << static_cast<int>(v) << "\n";
    }
    SetDown(Button::Up, (v & SDL_HAT_UP) != 0);
    SetDown(Button::Down, (v & SDL_HAT_DOWN) != 0);
    SetDown(Button::Left, (v & SDL_HAT_LEFT) != 0);
    SetDown(Button::Right, (v & SDL_HAT_RIGHT) != 0);
  } else if (e.type == SDL_JOYAXISMOTION) {
    constexpr int kDeadzone = 16000;
    const int axis = e.jaxis.axis;
    const int val = static_cast<int>(e.jaxis.value);
    const bool valid_axis = axis >= 0 && axis < static_cast<int>(last_joy_axis_values_.size());
    const int old_val = valid_axis ? last_joy_axis_values_[static_cast<size_t>(axis)] : 0;
    const bool should_log_axis = ShouldLogProbeJoyAxis(axis, val);
    if (should_log_axis) {
      std::cout << "[native_h700] input probe: type=" << SdlEventName(e.type)
                << " which=" << e.jaxis.which
                << " joy_axis=" << axis
                << " value=" << val
                << " old=" << old_val << "\n";
    }
    if (valid_axis) {
      last_joy_axis_values_[static_cast<size_t>(axis)] = val;
      joy_axis_seen_[static_cast<size_t>(axis)] = true;
    }
    if (axis == 0 || axis == 6) {
      SetDown(Button::Left, val < -kDeadzone);
      SetDown(Button::Right, val > kDeadzone);
    } else if (axis == 1 || axis == 7) {
      SetDown(Button::Up, val < -kDeadzone);
      SetDown(Button::Down, val > kDeadzone);
    } else if (input_profile_ == InputProfile::TrimuiBrick && axis == 2) {
      SetDown(Button::L2, val > kDeadzone);
    } else if (input_profile_ == InputProfile::TrimuiBrick && axis == 5) {
      SetDown(Button::R2, val > kDeadzone);
    }
  }
}

void InputManager::EndFrame() {
  PollDeviceInputEvents();
  for (auto &s : states_) {
    if (!s.down) {
      s.hold_time = 0.0f;
      s.repeat_timer = 0.0f;
      s.repeat_active = false;
      continue;
    }

    s.hold_time += dt_;
    if (s.hold_time >= 0.8f) s.long_pressed = true;

    if (!s.repeat_active) {
      s.repeat_active = true;
      s.repeat_timer = 0.4f;
      s.repeated = true;
    } else {
      s.repeat_timer -= dt_;
      if (s.repeat_timer <= 0.0f) {
        s.repeated = true;
        s.repeat_timer = 0.1f;
      }
    }
  }
}

bool InputManager::IsPressed(Button b) const { return Get(b).down; }
bool InputManager::IsJustPressed(Button b) const { return Get(b).just_pressed; }
bool InputManager::IsJustReleased(Button b) const { return Get(b).just_released; }
bool InputManager::IsRepeated(Button b) const { return Get(b).repeated; }
bool InputManager::IsLongPressed(Button b) const { return Get(b).long_pressed; }
float InputManager::HoldTime(Button b) const { return Get(b).hold_time; }

bool InputManager::AnyPressed() const {
  for (const auto &s : states_) {
    if (s.down) return true;
  }
  return false;
}

void InputManager::ResetAll() {
  for (auto &s : states_) {
    s = BtnState{};
  }
}

std::string InputManager::DescribeJoyMap() const { return DescribeMap(joy_map_, "joy"); }

std::string InputManager::DescribePadMap() const { return DescribeMap(pad_map_, "pad"); }

Button InputManager::InvalidButton() { return static_cast<Button>(-1); }

bool InputManager::IsValid(Button b) {
  const int i = static_cast<int>(b);
  return i >= 0 && i < kButtonCount;
}

Button InputManager::KeyToButton(SDL_Keycode k) {
  constexpr SDL_Keycode kVolumeUpFallback = static_cast<SDL_Keycode>(1073741952);
  constexpr SDL_Keycode kVolumeDownFallback = static_cast<SDL_Keycode>(1073741953);
  switch (k) {
  case SDLK_UP: return Button::Up;
  case SDLK_DOWN: return Button::Down;
  case SDLK_LEFT: return Button::Left;
  case SDLK_RIGHT: return Button::Right;
  case SDLK_a: return Button::A;
  case SDLK_b: return Button::B;
  case SDLK_x: return Button::X;
  case SDLK_y: return Button::Y;
  case SDLK_m: return Button::Menu;
  case SDLK_ESCAPE: return Button::B;
  case SDLK_RETURN: return Button::A;
  case SDLK_BACKSPACE: return Button::Select;
  case SDLK_TAB: return Button::Start;
  case SDLK_q: return Button::L1;
  case SDLK_w: return Button::R1;
  case SDLK_e: return Button::L2;
  case SDLK_r: return Button::R2;
  case SDLK_1: return Button::L1;
  case SDLK_2: return Button::L2;
  case SDLK_4: return Button::R1;
  case SDLK_3: return Button::R2;
  case SDLK_z: return Button::Start;
  case SDLK_c: return Button::Select;
#ifdef SDLK_VOLUMEUP
  case SDLK_VOLUMEUP: return Button::VolUp;
#endif
#ifdef SDLK_VOLUMEDOWN
  case SDLK_VOLUMEDOWN: return Button::VolDown;
#endif
#ifdef SDLK_POWER
  case SDLK_POWER: return Button::Power;
#endif
  case kVolumeUpFallback: return Button::VolUp;
  case kVolumeDownFallback: return Button::VolDown;
#ifdef SDLK_PLUS
  case SDLK_PLUS: return Button::VolUp;
#endif
#ifdef SDLK_KP_PLUS
  case SDLK_KP_PLUS: return Button::VolUp;
#endif
#ifdef SDLK_MINUS
  case SDLK_MINUS: return Button::VolDown;
#endif
#ifdef SDLK_KP_MINUS
  case SDLK_KP_MINUS: return Button::VolDown;
#endif
  default: return static_cast<Button>(-1);
  }
}

Button InputManager::PadToButton(uint8_t b) const {
  if (b >= pad_map_.size()) return InvalidButton();
  return pad_map_[b];
}

Button InputManager::JoyButtonToButton(uint8_t b) const {
  if (b >= joy_map_.size()) return InvalidButton();
  return joy_map_[b];
}

void InputManager::PollDeviceInputEvents() {
  PollLinuxInputDevices();
}

void InputManager::LoadDefaultPadMap(InputProfile input_profile) {
  pad_map_[SDL_CONTROLLER_BUTTON_DPAD_UP] = Button::Up;
  pad_map_[SDL_CONTROLLER_BUTTON_DPAD_DOWN] = Button::Down;
  pad_map_[SDL_CONTROLLER_BUTTON_DPAD_LEFT] = Button::Left;
  pad_map_[SDL_CONTROLLER_BUTTON_DPAD_RIGHT] = Button::Right;
  if (input_profile == InputProfile::TrimuiBrick) {
    pad_map_[SDL_CONTROLLER_BUTTON_A] = Button::B;
    pad_map_[SDL_CONTROLLER_BUTTON_B] = Button::A;
    pad_map_[SDL_CONTROLLER_BUTTON_X] = Button::Y;
    pad_map_[SDL_CONTROLLER_BUTTON_Y] = Button::X;
  } else {
    pad_map_[SDL_CONTROLLER_BUTTON_A] = Button::A;
    pad_map_[SDL_CONTROLLER_BUTTON_B] = Button::B;
    pad_map_[SDL_CONTROLLER_BUTTON_X] = Button::X;
    pad_map_[SDL_CONTROLLER_BUTTON_Y] = Button::Y;
  }
  pad_map_[SDL_CONTROLLER_BUTTON_LEFTSHOULDER] = Button::L1;
  pad_map_[SDL_CONTROLLER_BUTTON_RIGHTSHOULDER] = Button::R1;
  pad_map_[SDL_CONTROLLER_BUTTON_LEFTSTICK] = Button::L2;
  pad_map_[SDL_CONTROLLER_BUTTON_RIGHTSTICK] = Button::R2;
  pad_map_[SDL_CONTROLLER_BUTTON_BACK] = Button::Select;
  pad_map_[SDL_CONTROLLER_BUTTON_START] = Button::Start;
}

void InputManager::LoadDefaultJoyMap(InputProfile input_profile) {
  joy_map_[0] = Button::A;
  joy_map_[1] = Button::B;
  joy_map_[4] = Button::L1;
  joy_map_[5] = Button::R1;
  if (input_profile == InputProfile::H70034xxSp) {
    joy_map_[2] = Button::Y;
    joy_map_[3] = Button::X;
    joy_map_[6] = Button::Select;
    joy_map_[7] = Button::Start;
    joy_map_[8] = Button::Menu;
    joy_map_[9] = Button::Menu;
    joy_map_[10] = Button::L2;
    joy_map_[11] = Button::R2;
    joy_map_[12] = Button::Select;
    joy_map_[13] = Button::Start;
    joy_map_[14] = Button::Power;
    joy_map_[15] = Button::VolDown;
    joy_map_[16] = Button::VolUp;
  } else if (input_profile == InputProfile::TrimuiBrick) {
    joy_map_[0] = Button::B;
    joy_map_[1] = Button::A;
    joy_map_[2] = Button::Y;
    joy_map_[3] = Button::X;
    joy_map_[6] = Button::Select;
    joy_map_[7] = Button::Start;
    joy_map_[8] = Button::Menu;
    joy_map_[9] = Button::L2;
    joy_map_[10] = Button::R2;
    joy_map_[11] = InvalidButton();
    joy_map_[12] = Button::Select;
    joy_map_[13] = InvalidButton();
    joy_map_[14] = Button::Power;
    joy_map_[15] = Button::VolDown;
    joy_map_[16] = Button::VolUp;
  } else if (input_profile == InputProfile::H70035xxH) {
    joy_map_[2] = Button::Y;
    joy_map_[3] = Button::X;
    joy_map_[6] = Button::Select;
    joy_map_[7] = Button::Start;
    joy_map_[8] = Button::Menu;
    joy_map_[9] = Button::Menu;
    joy_map_[10] = Button::L2;
    joy_map_[11] = Button::R2;
    joy_map_[12] = Button::Select;
    joy_map_[13] = Button::Start;
    joy_map_[14] = Button::Power;
    joy_map_[15] = Button::VolDown;
    joy_map_[16] = Button::VolUp;
  } else if (input_profile == InputProfile::H700Default) {
    joy_map_[2] = Button::Y;
    joy_map_[3] = Button::X;
    joy_map_[6] = Button::Select;
    joy_map_[7] = Button::Start;
    joy_map_[8] = Button::Menu;
    joy_map_[9] = Button::L2;
    joy_map_[10] = Button::R2;
    joy_map_[11] = Button::Menu;
    joy_map_[12] = Button::Select;
    joy_map_[13] = Button::Start;
    joy_map_[14] = Button::Power;
    joy_map_[15] = Button::VolDown;
    joy_map_[16] = Button::VolUp;
  } else {
    joy_map_[2] = Button::Y;
    joy_map_[3] = Button::X;
    joy_map_[6] = Button::Select;
    joy_map_[7] = Button::Start;
    joy_map_[8] = Button::Menu;
    joy_map_[9] = Button::L2;
    joy_map_[10] = Button::R2;
    joy_map_[11] = Button::Menu;
    joy_map_[12] = Button::Select;
    joy_map_[13] = Button::Start;
    joy_map_[15] = Button::VolDown;
    joy_map_[16] = Button::VolUp;
  }
}

std::string InputManager::Trim(std::string s) {
  size_t b = 0;
  while (b < s.size() && std::isspace(static_cast<unsigned char>(s[b]))) ++b;
  size_t e = s.size();
  while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) --e;
  return s.substr(b, e - b);
}

bool InputManager::ParseButtonName(const std::string &raw, Button &out) {
  std::string n;
  n.reserve(raw.size());
  for (char c : raw) {
    if (c == ' ' || c == '\t' || c == '-') continue;
    n.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(c))));
  }
  if (n == "UP") out = Button::Up;
  else if (n == "DOWN") out = Button::Down;
  else if (n == "LEFT") out = Button::Left;
  else if (n == "RIGHT") out = Button::Right;
  else if (n == "A") out = Button::A;
  else if (n == "B") out = Button::B;
  else if (n == "X") out = Button::X;
  else if (n == "Y") out = Button::Y;
  else if (n == "MENU") out = Button::Menu;
  else if (n == "L1") out = Button::L1;
  else if (n == "L2") out = Button::L2;
  else if (n == "R1") out = Button::R1;
  else if (n == "R2") out = Button::R2;
  else if (n == "START") out = Button::Start;
  else if (n == "SELECT") out = Button::Select;
  else if (n == "VOLUP" || n == "VOLUMEUP") out = Button::VolUp;
  else if (n == "VOLDOWN" || n == "VOLUMEDOWN") out = Button::VolDown;
  else if (n == "POWER") out = Button::Power;
  else if (n == "NONE" || n == "DISABLED" || n == "INVALID") out = InvalidButton();
  else return false;
  return true;
}

void InputManager::LoadOverrides(const std::string &mapping_path) {
  std::ifstream in(mapping_path);
  if (!in) return;
  std::string line;
  while (std::getline(in, line)) {
    line = Trim(line);
    if (line.empty() || line[0] == '#' || line[0] == ';') continue;
    const size_t eq = line.find('=');
    if (eq == std::string::npos) continue;
    const std::string key = Trim(line.substr(0, eq));
    const std::string val = Trim(line.substr(eq + 1));
    Button mapped = InvalidButton();
    if (!ParseButtonName(val, mapped)) continue;
    if (key.rfind("joy.", 0) == 0) {
      const int idx = std::atoi(key.substr(4).c_str());
      if (idx >= 0 && idx < static_cast<int>(joy_map_.size())) joy_map_[idx] = mapped;
    } else if (key.rfind("pad.", 0) == 0) {
      const int idx = std::atoi(key.substr(4).c_str());
      if (idx >= 0 && idx < static_cast<int>(pad_map_.size())) pad_map_[idx] = mapped;
    }
  }
}

void InputManager::SetDown(Button b, bool down) {
  if (!IsValid(b)) return;
  BtnState &s = states_[static_cast<int>(b)];
  if (down && !s.down) {
    s.down = true;
    s.just_pressed = true;
    s.hold_time = 0.0f;
  } else if (!down && s.down) {
    s.down = false;
    s.just_released = true;
  }
}

bool InputManager::MarkProbeLogged(std::array<bool, 512> &seen, int index) {
  if (!full_input_log_enabled_ || index < 0 || index >= static_cast<int>(seen.size())) return false;
  bool &slot = seen[static_cast<size_t>(index)];
  if (slot) return false;
  slot = true;
  return true;
}

bool InputManager::MarkProbeLogged(std::array<bool, 16> &seen, int index) {
  if (!full_input_log_enabled_ || index < 0 || index >= static_cast<int>(seen.size())) return false;
  bool &slot = seen[static_cast<size_t>(index)];
  if (slot) return false;
  slot = true;
  return true;
}

bool InputManager::ShouldLogProbeKey(SDL_Scancode scancode) {
  return MarkProbeLogged(probe_key_seen_, static_cast<int>(scancode));
}

bool InputManager::ShouldLogProbePadButton(uint8_t button) {
  return MarkProbeLogged(probe_pad_button_seen_, static_cast<int>(button));
}

bool InputManager::ShouldLogProbeJoyButton(uint8_t button) {
  return MarkProbeLogged(probe_joy_button_seen_, static_cast<int>(button));
}

bool InputManager::ShouldLogProbeHat(uint8_t hat, uint8_t value) {
  if (!full_input_log_enabled_ || value == SDL_HAT_CENTERED) return false;
  const int index = static_cast<int>(hat) * 16 + static_cast<int>(value);
  return MarkProbeLogged(probe_hat_seen_, index);
}

bool InputManager::ShouldLogProbePadAxis(int axis, int value) {
  constexpr int kDeadzone = 16000;
  if (std::abs(value) <= kDeadzone) return false;
  const int direction = value > 0 ? 1 : 0;
  return MarkProbeLogged(probe_pad_axis_seen_, axis * 2 + direction);
}

bool InputManager::ShouldLogProbeJoyAxis(int axis, int value) {
  constexpr int kDeadzone = 16000;
  if (std::abs(value) <= kDeadzone) return false;
  const int direction = value > 0 ? 1 : 0;
  return MarkProbeLogged(probe_joy_axis_seen_, axis * 2 + direction);
}

void InputManager::OpenLinuxInputDevices() {
#if !defined(_WIN32)
  if (input_profile_ == InputProfile::DesktopDefault) return;
  DIR *dir = opendir("/dev/input");
  if (!dir) return;
  while (dirent *entry = readdir(dir)) {
    if (std::strncmp(entry->d_name, "event", 5) != 0) continue;
    const std::string path = std::string("/dev/input/") + entry->d_name;
    const int fd = open(path.c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) continue;
    linux_input_fds_.push_back(fd);
    if (full_input_log_enabled_) {
      std::cout << "[native_h700] input probe: opened linux_input=" << path << "\n";
    }
  }
  closedir(dir);
#endif
}

void InputManager::CloseLinuxInputDevices() {
#if !defined(_WIN32)
  for (int fd : linux_input_fds_) {
    if (fd >= 0) close(fd);
  }
#endif
  linux_input_fds_.clear();
}

void InputManager::PollLinuxInputDevices() {
#if !defined(_WIN32)
  for (int fd : linux_input_fds_) {
    while (true) {
      input_event event{};
      const ssize_t n = read(fd, &event, sizeof(event));
      if (n == static_cast<ssize_t>(sizeof(event))) {
        if (event.type != EV_KEY) continue;
        const int code = static_cast<int>(event.code);
        const bool down = event.value != 0;
        if (full_input_log_enabled_ && down && MarkProbeLogged(probe_linux_key_seen_, code)) {
          std::cout << "[native_h700] input probe: type=LINUX_EV_KEY"
                    << " code=" << code
                    << " value=" << event.value
                    << " mapped=" << (code == KEY_POWER ? "Power" : "Invalid") << "\n";
        }
        if (code == KEY_POWER) {
          SetDown(Button::Power, down);
        }
        continue;
      }
      if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) break;
      break;
    }
  }
#endif
}


const BtnState &InputManager::Get(Button b) const {
  static BtnState empty;
  if (!IsValid(b)) return empty;
  return states_[static_cast<int>(b)];
}

std::string InputManager::DescribeMap(const std::array<Button, 32> &map, const char *prefix) const {
  std::ostringstream out;
  bool first = true;
  for (size_t i = 0; i < map.size(); ++i) {
    const Button mapped = map[i];
    if (!IsValid(mapped)) continue;
    if (!first) out << ' ';
    out << prefix << '.' << i << '=' << ButtonName(mapped);
    first = false;
  }
  return out.str();
}
