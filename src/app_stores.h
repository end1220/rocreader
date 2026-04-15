#pragma once

#include <SDL.h>

#include <functional>
#include <string>
#include <unordered_set>
#include <vector>

struct NativeConfig {
  int theme = 0;
  bool animations = true;
  bool audio = true;
  int sfx_volume = 64;
  std::string screen_profile;
  std::string system_language = "zh";
  int system_volume_percent = 50;
  int screen_brightness_level = 8;
  bool lid_close_screen_off = true;
  int auto_sleep_interval_index = 2;
  int txt_background_color = 4;
  int txt_font_color = 0;
  int txt_font_size_level = 2;
};

class ConfigStore {
public:
  explicit ConfigStore(std::string path);

  const NativeConfig &Get() const;
  NativeConfig &Mutable();
  bool IsDirty() const;
  bool ShouldFlush(uint32_t now, uint32_t delay_ms) const;
  void MarkDirty();
  void Save();

private:
  void Load();

  std::string path_;
  NativeConfig cfg_;
  bool dirty_ = false;
  uint32_t last_dirty_tick_ = 0;
};

class PathSetStore {
public:
  PathSetStore(std::string path, std::function<std::string(const std::string &)> normalize_path_key);

  bool Contains(const std::string &book_path) const;
  void Add(const std::string &book_path);
  void Remove(const std::string &book_path);
  bool Toggle(const std::string &book_path);
  void Clear();
  bool IsDirty() const;
  bool ShouldFlush(uint32_t now, uint32_t delay_ms) const;
  void MarkDirty();
  void Save();

private:
  void Load();

  std::string path_;
  std::function<std::string(const std::string &)> normalize_path_key_;
  std::unordered_set<std::string> set_;
  bool dirty_ = false;
  uint32_t last_dirty_tick_ = 0;
};

class RecentPathStore {
public:
  RecentPathStore(std::string path, std::function<std::string(const std::string &)> normalize_path_key);

  bool Contains(const std::string &book_path) const;
  void Add(const std::string &book_path);
  void Remove(const std::string &book_path);
  void Clear();
  const std::vector<std::string> &OrderedPaths() const;
  bool IsDirty() const;
  bool ShouldFlush(uint32_t now, uint32_t delay_ms) const;
  void MarkDirty();
  void Save();

private:
  void Load();

  std::string path_;
  std::function<std::string(const std::string &)> normalize_path_key_;
  std::unordered_set<std::string> set_;
  std::vector<std::string> order_;
  bool dirty_ = false;
  uint32_t last_dirty_tick_ = 0;
};
