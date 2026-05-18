#include "path_adapter.h"

#include <fstream>
#include <vector>

namespace fs = std::filesystem;

namespace {

std::vector<fs::path> BuildPathCandidates(const fs::path &path) {
  std::vector<fs::path> out;
  auto add = [&](const fs::path &candidate) {
    if (candidate.empty()) return;
    for (const auto &existing : out) {
      if (existing.native() == candidate.native()) return;
    }
    out.push_back(candidate);
  };

  add(path);

#ifdef _WIN32
  add(fs::path(path.string()));
#else
  add(fs::path(path.native()));
  add(fs::path(path.string()));
  add(fs::path(path.generic_string()));
#endif

  return out;
}

bool IsReadableFile(const fs::path &path) {
#ifdef _WIN32
  std::ifstream in(path, std::ios::binary);
  return static_cast<bool>(in);
#else
  std::error_code ec;
  return fs::exists(path, ec) && !ec && fs::is_regular_file(path, ec) && !ec;
#endif
}

bool IsReadableDir(const fs::path &path) {
  std::error_code ec;
  return fs::exists(path, ec) && !ec && fs::is_directory(path, ec) && !ec;
}

}

namespace path_adapter {

std::string StorePathString(const fs::path &path) {
#ifdef _WIN32
  return path.string();
#else
  return path.native();
#endif
}

std::string ResolveReadableFilePath(const fs::directory_entry &entry) {
  const fs::path original = entry.path();
  for (const auto &candidate : BuildPathCandidates(original)) {
    if (IsReadableFile(candidate)) return StorePathString(candidate);
  }
  return StorePathString(original);
}

std::string ResolveReadableDirPath(const fs::path &path) {
  for (const auto &candidate : BuildPathCandidates(path)) {
    if (IsReadableDir(candidate)) return StorePathString(candidate);
  }
  return StorePathString(path);
}

} // namespace path_adapter
