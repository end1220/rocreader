#pragma once

#include <string>

struct ScreenProfile {
  int detected_w = 0;
  int detected_h = 0;
  int screen_w = 720;
  int screen_h = 480;
  std::string profile_name = "720x480";
  std::string detection_source = "default";
};

ScreenProfile DetectScreenProfile();
std::string DetectDeviceModelToken();
bool Uses34xxSpKeymap(const std::string &model_token);
bool Uses35xxHKeymap(const std::string &model_token);
