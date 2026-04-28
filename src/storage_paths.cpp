#include "storage_paths.h"
#include "path_adapter.h"

#include <cstdlib>
#include "filesystem_compat.h"
#include <unordered_set>
#include <system_error>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {
std::unordered_set<std::string> g_seen_dir_keys;

std::string CanonicalKey(const fs::path &p) {
  std::error_code ec;
  fs::path canonical = filesystem_compat::WeaklyCanonical(p, ec);
  if (!ec) {
    return path_adapter::StorePathString(filesystem_compat::LexicallyNormal(canonical));
  }
  return path_adapter::StorePathString(filesystem_compat::LexicallyNormal(p));
}

void AddIfDirExists(std::vector<std::string> &out, const fs::path &p) {
  if (fs::exists(p) && fs::is_directory(p)) {
    const std::string s = path_adapter::ResolveReadableDirPath(p);
    const std::string key = CanonicalKey(p);
    if (!g_seen_dir_keys.insert(key).second) {
      return;
    }
    out.push_back(s);
  }
}

std::vector<fs::path> Card1Candidates() {
  std::vector<fs::path> roots;
  if (const char *env = std::getenv("ROCREADER_CARD1_ROOT"); env && *env) {
    roots.emplace_back(env);
  }
  roots.emplace_back("/mnt/mmc");
  roots.emplace_back("/media/mmc");
  roots.emplace_back("/mnt/SDCARD");
  return roots;
}

std::vector<fs::path> Card2Candidates() {
  std::vector<fs::path> roots;
  if (const char *env = std::getenv("ROCREADER_CARD2_ROOT"); env && *env) {
    roots.emplace_back(env);
  }
  roots.emplace_back("/mnt/sdcard");
  roots.emplace_back("/mnt/mmc2");
  roots.emplace_back("/media/mmc2");
  roots.emplace_back("/media/sdcard");
  return roots;
}

void AddRocreaderCandidates(std::vector<std::string> &out, const std::vector<fs::path> &roots) {
  for (const auto &root : roots) {
    AddIfDirExists(out, root / "ROCreader");
    AddIfDirExists(out, root / "Apps" / "ROCreader");
    AddIfDirExists(out, root / "Roms" / "ROCreader");
    AddIfDirExists(out, root / "APPS" / "ROCreader");
    AddIfDirExists(out, root / "Roms" / "APPS" / "ROCreader");
  }
}

void AddRocreaderNamedDirCandidates(std::vector<std::string> &out,
                                    const std::vector<fs::path> &roots,
                                    const char *dir_name) {
  for (const auto &root : roots) {
    AddIfDirExists(out, root / dir_name);
    AddIfDirExists(out, root / "ROCreader" / dir_name);
    AddIfDirExists(out, root / "Apps" / "ROCreader" / dir_name);
    AddIfDirExists(out, root / "Roms" / "ROCreader" / dir_name);
    AddIfDirExists(out, root / "APPS" / "ROCreader" / dir_name);
    AddIfDirExists(out, root / "Roms" / "APPS" / "ROCreader" / dir_name);
  }
}

void AddSiblingNamedDirsForRoots(std::vector<std::string> &out,
                                 const std::vector<std::string> &source_roots,
                                 const char *source_dir_name,
                                 const char *sibling_dir_name) {
  for (const auto &source_root : source_roots) {
    const fs::path source_path(source_root);
    if (source_path.filename() != source_dir_name) continue;
    AddIfDirExists(out, source_path.parent_path() / sibling_dir_name);
  }
}

void AddNamedDirsRecursively(std::vector<std::string> &out,
                             const std::vector<fs::path> &roots,
                             const char *dir_name) {
  const auto opts = fs::directory_options::skip_permission_denied;
  for (const auto &root : roots) {
    std::error_code ec;
    if (!fs::exists(root, ec) || !fs::is_directory(root, ec)) continue;

    fs::path direct = root / dir_name;
    AddIfDirExists(out, direct);

    for (fs::recursive_directory_iterator it(root, opts, ec), end; it != end; it.increment(ec)) {
      if (ec) {
        ec.clear();
        continue;
      }
      if (!filesystem_compat::IsDirectory(*it, ec)) {
        ec.clear();
        continue;
      }
      if (it->path().filename() == dir_name) {
        AddIfDirExists(out, it->path());
      }
    }
  }
}

void AddRuntimeCandidates(std::vector<std::string> &out) {
  // Explicit override for unusual firmware mount layouts.
  if (const char *env = std::getenv("ROCREADER_ROOT"); env && *env) {
    AddIfDirExists(out, fs::path(env));
  }

  // Common hardcoded locations observed on handheld firmwares.
  AddIfDirExists(out, fs::path("/Roms/APPS/ROCreader"));
  AddIfDirExists(out, fs::path("/Apps/ROCreader"));
  AddIfDirExists(out, fs::path("/mnt/mmc/Roms/APPS/ROCreader"));
  AddIfDirExists(out, fs::path("/mnt/mmc/Apps/ROCreader"));
  AddIfDirExists(out, fs::path("/mnt/SDCARD/Apps/ROCreader"));
  AddIfDirExists(out, fs::path("/mnt/mmc2/Roms/APPS/ROCreader"));
  AddIfDirExists(out, fs::path("/media/mmc/Roms/APPS/ROCreader"));
  AddIfDirExists(out, fs::path("/media/mmc/Apps/ROCreader"));
  AddIfDirExists(out, fs::path("/media/mmc2/Roms/APPS/ROCreader"));

  // Current working directory can be ROCreader or APPS.
  std::error_code ec;
  const fs::path cwd = fs::current_path(ec);
  if (!ec) {
    AddIfDirExists(out, cwd);
    AddIfDirExists(out, cwd / "ROCreader");
    AddIfDirExists(out, cwd.parent_path() / "ROCreader");
  }
}

void CollectAllRocreaderRoots(std::vector<std::string> &out) {
  if (const char *env = std::getenv("ROCREADER_ROOT"); env && *env) {
    AddIfDirExists(out, fs::path(env));
  }

  const std::vector<fs::path> card1 = Card1Candidates();
  const std::vector<fs::path> card2 = Card2Candidates();
  AddRocreaderCandidates(out, card1);
  AddRocreaderCandidates(out, card2);
  AddRuntimeCandidates(out);
}

std::vector<std::string> DetectAllRocreaderRoots() {
  g_seen_dir_keys.clear();
  std::vector<std::string> out;
  CollectAllRocreaderRoots(out);
  return out;
}

void CollectMountedCardRoots(std::vector<std::string> &out, const std::vector<fs::path> &roots) {
  for (const auto &root : roots) AddIfDirExists(out, root);
}

std::string DetectPrimaryRocreaderRoot() {
  std::vector<std::string> candidates;

  // Explicit override always wins.
  if (const char *env = std::getenv("ROCREADER_ROOT"); env && *env) {
    AddIfDirExists(candidates, fs::path(env));
  }

  // Primary runtime data stays on the system card (card1).
  AddIfDirExists(candidates, fs::path("/mnt/mmc/Roms/APPS/ROCreader"));
  AddIfDirExists(candidates, fs::path("/mnt/mmc/Apps/ROCreader"));
  AddIfDirExists(candidates, fs::path("/mnt/SDCARD/Apps/ROCreader"));
  AddIfDirExists(candidates, fs::path("/media/mmc/Roms/APPS/ROCreader"));
  AddIfDirExists(candidates, fs::path("/media/mmc/Apps/ROCreader"));
  AddIfDirExists(candidates, fs::path("/mnt/mmc/ROCreader"));
  AddIfDirExists(candidates, fs::path("/media/mmc/ROCreader"));
  AddIfDirExists(candidates, fs::path("/Roms/APPS/ROCreader"));

  // Last resort: runtime-derived paths (still single-root selection).
  if (candidates.empty()) {
    AddRuntimeCandidates(candidates);
  }

  if (candidates.empty()) {
    return {};
  }
  return candidates.front();
}
} // namespace

namespace storage_paths {

std::vector<std::string> DetectRocreaderRoots() {
#ifdef _WIN32
  g_seen_dir_keys.clear();
  std::vector<std::string> out;

  // Allow explicit override first for unusual preview/test layouts.
  if (const char *env = std::getenv("ROCREADER_ROOT"); env && *env) {
    AddIfDirExists(out, fs::path(env));
  }

  std::error_code ec;
  const fs::path cwd = fs::current_path(ec);
  if (!ec) {
    // Flattened project layout: run from ROCreader root directly.
    AddIfDirExists(out, cwd);
    // Keep a couple of compatibility fallbacks for older preview launch styles.
    AddIfDirExists(out, cwd / "ROCreader");
    AddIfDirExists(out, cwd.parent_path() / "ROCreader");
    AddIfDirExists(out, cwd.parent_path());
  }
  return out;
#else
  g_seen_dir_keys.clear();
  const std::string primary = DetectPrimaryRocreaderRoot();
  if (primary.empty()) {
    return {};
  }
  return {primary};
#endif
}

std::vector<std::string> DetectStorageCardRoots() {
  g_seen_dir_keys.clear();
  std::vector<std::string> out;
  CollectMountedCardRoots(out, Card1Candidates());
  CollectMountedCardRoots(out, Card2Candidates());
  return out;
}

std::vector<std::string> DetectBooksRoots() {
  g_seen_dir_keys.clear();
  std::vector<std::string> out;
#ifndef _WIN32
  const std::vector<std::string> app_roots = DetectRocreaderRoots();
  for (const auto &root : app_roots) {
    AddIfDirExists(out, fs::path(root) / "books");
  }
  AddRocreaderNamedDirCandidates(out, Card1Candidates(), "books");
  AddRocreaderNamedDirCandidates(out, Card2Candidates(), "books");
  return out;
#else
  const std::vector<fs::path> card1 = Card1Candidates();
  const std::vector<fs::path> card2 = Card2Candidates();
  AddNamedDirsRecursively(out, card1, "books");
  AddNamedDirsRecursively(out, card2, "books");
  // Local dev fallbacks (Windows only).
  std::error_code ec;
  const fs::path cwd = fs::current_path(ec);
  if (!ec) {
    AddIfDirExists(out, cwd / "books");
    AddIfDirExists(out, cwd / "ROCreader" / "books");
    AddIfDirExists(out, cwd.parent_path() / "books");
    AddIfDirExists(out, cwd.parent_path() / "ROCreader" / "books");
  }
  AddIfDirExists(out, fs::path("../books"));
  return out;
#endif
}

std::vector<std::string> DetectCoverRoots() {
  const std::vector<std::string> books_roots = DetectBooksRoots();
  g_seen_dir_keys.clear();
  std::vector<std::string> out;
  AddSiblingNamedDirsForRoots(out, books_roots, "books", "book_covers");
#ifndef _WIN32
  const std::vector<std::string> app_roots = DetectRocreaderRoots();
  for (const auto &root : app_roots) {
    AddIfDirExists(out, fs::path(root) / "book_covers");
  }
  AddRocreaderNamedDirCandidates(out, Card1Candidates(), "book_covers");
  AddRocreaderNamedDirCandidates(out, Card2Candidates(), "book_covers");
  return out;
#else
  const std::vector<fs::path> card1 = Card1Candidates();
  const std::vector<fs::path> card2 = Card2Candidates();
  AddNamedDirsRecursively(out, card1, "book_covers");
  AddNamedDirsRecursively(out, card2, "book_covers");
  // Local dev fallbacks (Windows only).
  std::error_code ec;
  const fs::path cwd = fs::current_path(ec);
  if (!ec) {
    AddIfDirExists(out, cwd / "book_covers");
    AddIfDirExists(out, cwd / "ROCreader" / "book_covers");
    AddIfDirExists(out, cwd.parent_path() / "book_covers");
    AddIfDirExists(out, cwd.parent_path() / "ROCreader" / "book_covers");
  }
  AddIfDirExists(out, fs::path("../book_covers"));
  return out;
#endif
}

} // namespace storage_paths
