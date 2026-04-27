#include "epub_cover_cache.h"

#include "epub_flow_reader.h"
#include "epub_reader.h"
#include "image_decode.h"
#include "runtime_log.h"

#include <SDL.h>
#include "filesystem_compat.h"
#include <sstream>
#include <string>

namespace {
struct FileCacheMeta {
  uintmax_t size = 0;
  long long mtime = 0;
};

FileCacheMeta ReadFileCacheMeta(const std::string &doc_path) {
  FileCacheMeta meta;
  std::error_code ec;
  const std::filesystem::path path(doc_path);
  meta.size = std::filesystem::file_size(path, ec);
  if (ec) meta.size = 0;
  ec.clear();
  const auto mtime_raw = std::filesystem::last_write_time(path, ec);
  meta.mtime = ec ? 0LL : static_cast<long long>(mtime_raw.time_since_epoch().count());
  return meta;
}

std::string MakeEpubCoverCacheKey(const std::string &doc_path, const FileCacheMeta &meta,
                                  int cover_w, int cover_h, const EpubCoverTextureDeps &deps) {
  return deps.normalize_path_key(doc_path) + "|" + std::to_string(meta.size) + "|" +
         std::to_string(meta.mtime) + "|" + std::to_string(cover_w) + "x" +
         std::to_string(cover_h) + "|epub-cover-v2";
}

std::filesystem::path GetEpubCoverCacheFile(const std::string &doc_path, const FileCacheMeta &meta,
                                            const EpubCoverTextureDeps &deps) {
  const std::string cache_key =
      MakeEpubCoverCacheKey(doc_path, meta, deps.cover_w, deps.cover_h, deps);
  const size_t hash_value = std::hash<std::string>{}(cache_key);
  std::ostringstream oss;
  oss << std::hex << hash_value << ".bmp";
  return deps.cover_thumb_cache_dir / oss.str();
}

SDL_Texture *LoadCachedEpubCoverTexture(const std::string &doc_path, const FileCacheMeta &meta,
                                        EpubCoverTextureDeps &deps) {
  const std::filesystem::path cache_file = GetEpubCoverCacheFile(doc_path, meta, deps);
  std::error_code ec;
  if (!std::filesystem::exists(cache_file, ec) || ec) return nullptr;
  SDL_Surface *cover_surface = deps.load_surface_from_file(cache_file.string());
  if (!cover_surface) return nullptr;
  SDL_Texture *normalized =
      deps.create_normalized_cover_texture(deps.renderer, cover_surface, deps.cover_w, deps.cover_h,
                                           static_cast<float>(deps.cover_w) / static_cast<float>(deps.cover_h));
  if (!normalized) normalized = deps.create_texture_from_surface(deps.renderer, cover_surface);
  SDL_FreeSurface(cover_surface);
  if (normalized) {
    deps.remember_texture_size(normalized, deps.cover_w, deps.cover_h);
  }
  return normalized;
}

void SaveEpubCoverCacheToDisk(const std::string &doc_path, const FileCacheMeta &meta, SDL_Surface *cover_surface,
                              const EpubCoverTextureDeps &deps) {
  if (!cover_surface) return;
  std::error_code ec;
  std::filesystem::create_directories(deps.cover_thumb_cache_dir, ec);
  const std::filesystem::path cache_file = GetEpubCoverCacheFile(doc_path, meta, deps);
  SDL_SaveBMP(cover_surface, cache_file.string().c_str());
}
}

SDL_Texture *CreateEpubFirstImageCoverTexture(const std::string &doc_path,
                                              EpubCoverTextureDeps &deps) {
  const FileCacheMeta cache_meta = ReadFileCacheMeta(doc_path);
  if (SDL_Texture *cached = LoadCachedEpubCoverTexture(doc_path, cache_meta, deps)) {
    return cached;
  }

  std::string error;
  std::string cover_bytes;
  if (EpubFlowReader::LooksLikeMixedLayout(doc_path)) {
    if (!EpubFlowReader::ExtractFirstDocumentImage(doc_path, cover_bytes, error)) {
      runtime_log::Line("[epub_cover] flow first image extract failed path=" + doc_path + " error=" + error);
    }
  }
  if (cover_bytes.empty()) {
    EpubReader epub;
    EpubReader::CoverImage cover_image;
    if (!epub.ExtractCoverImage(doc_path, cover_image, error)) {
      runtime_log::Line("[epub_cover] extract failed path=" + doc_path + " error=" + error);
      return nullptr;
    }
    cover_bytes.assign(cover_image.bytes.begin(), cover_image.bytes.end());
  }

  SDL_Surface *cover_surface =
      deps.load_surface_from_memory(cover_bytes.data(), cover_bytes.size());
  if (!cover_surface) cover_surface = DecodeSurfaceFromMemory(cover_bytes.data(), cover_bytes.size());
  if (!cover_surface) {
    runtime_log::Line("[epub_cover] decode surface failed path=" + doc_path);
    return nullptr;
  }
  SDL_Texture *normalized =
      deps.create_normalized_cover_texture(deps.renderer, cover_surface, deps.cover_w, deps.cover_h,
                                           static_cast<float>(deps.cover_w) / static_cast<float>(deps.cover_h));
  if (!normalized) normalized = deps.create_texture_from_surface(deps.renderer, cover_surface);
  if (normalized) {
    deps.remember_texture_size(normalized, deps.cover_w, deps.cover_h);
    SaveEpubCoverCacheToDisk(doc_path, cache_meta, cover_surface, deps);
  }
  SDL_FreeSurface(cover_surface);
  return normalized;
}

bool HasCachedEpubCoverOnDisk(const std::string &doc_path, EpubCoverTextureDeps &deps) {
  const FileCacheMeta cache_meta = ReadFileCacheMeta(doc_path);
  std::error_code ec;
  const std::filesystem::path cache_file = GetEpubCoverCacheFile(doc_path, cache_meta, deps);
  return std::filesystem::exists(cache_file, ec) && !ec;
}
