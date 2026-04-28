#pragma once

#include <SDL.h>

#include <array>
#include <string>
#include <vector>

constexpr int kButtonCount = 18;

enum class Button {
  Up,
  Down,
  Left,
  Right,
  A,
  B,
  X,
  Y,
  Menu,
  L1,
  L2,
  R1,
  R2,
  Start,
  Select,
  VolUp,
  VolDown,
  Power,
};

enum class InputProfile {
  DesktopDefault,
  H700Default,
  H70034xxSp,
  H70035xxH,
  TrimuiBrick,
};

const char *ButtonName(Button b);
const char *InputProfileName(InputProfile profile);
const char *SdlEventName(Uint32 type);

struct BtnState {
  bool down = false;
  bool just_pressed = false;
  bool just_released = false;
  bool repeated = false;
  bool long_pressed = false;
  float hold_time = 0.0f;
  float repeat_timer = 0.0f;
  bool repeat_active = false;
};

class InputManager {
public:
  InputManager(const std::string &mapping_path, InputProfile input_profile);
  ~InputManager();

  void BeginFrame(float dt);
  void HandleEvent(const SDL_Event &e);
  void EndFrame();

  bool IsPressed(Button b) const;
  bool IsJustPressed(Button b) const;
  bool IsJustReleased(Button b) const;
  bool IsRepeated(Button b) const;
  bool IsLongPressed(Button b) const;
  float HoldTime(Button b) const;
  bool AnyPressed() const;
  void ResetAll();
  std::string DescribeJoyMap() const;
  std::string DescribePadMap() const;

private:
  static Button InvalidButton();
public:
  static bool IsValid(Button b);
private:
  static Button KeyToButton(SDL_Keycode k);
  Button PadToButton(uint8_t b) const;
  Button JoyButtonToButton(uint8_t b) const;
  void PollDeviceInputEvents();
  void LoadDefaultPadMap(InputProfile input_profile);
  void LoadDefaultJoyMap(InputProfile input_profile);
  static std::string Trim(std::string s);
  static bool ParseButtonName(const std::string &raw, Button &out);
  void LoadOverrides(const std::string &mapping_path);
  void SetDown(Button b, bool down);
  bool MarkProbeLogged(std::array<bool, 512> &seen, int index);
  bool MarkProbeLogged(std::array<bool, 16> &seen, int index);
  bool ShouldLogProbeKey(SDL_Scancode scancode);
  bool ShouldLogProbePadButton(uint8_t button);
  bool ShouldLogProbeJoyButton(uint8_t button);
  bool ShouldLogProbeHat(uint8_t hat, uint8_t value);
  bool ShouldLogProbePadAxis(int axis, int value);
  bool ShouldLogProbeJoyAxis(int axis, int value);
  void OpenLinuxInputDevices();
  void CloseLinuxInputDevices();
  void PollLinuxInputDevices();
  const BtnState &Get(Button b) const;
  std::string DescribeMap(const std::array<Button, 32> &map, const char *prefix) const;

  std::array<BtnState, kButtonCount> states_{};
  std::array<Button, 32> pad_map_{};
  std::array<Button, 32> joy_map_{};
  InputProfile input_profile_ = InputProfile::DesktopDefault;
  std::array<int, 16> last_pad_axis_values_{};
  std::array<int, 16> last_joy_axis_values_{};
  std::array<bool, 16> pad_axis_seen_{};
  std::array<bool, 16> joy_axis_seen_{};
  std::array<bool, 512> probe_key_seen_{};
  std::array<bool, 512> probe_pad_button_seen_{};
  std::array<bool, 512> probe_joy_button_seen_{};
  std::array<bool, 512> probe_hat_seen_{};
  std::array<bool, 512> probe_pad_axis_seen_{};
  std::array<bool, 512> probe_joy_axis_seen_{};
  std::array<bool, 512> probe_linux_key_seen_{};
  std::vector<int> linux_input_fds_;
  bool full_input_log_enabled_ = false;
  float dt_ = 1.0f / 60.0f;
};
