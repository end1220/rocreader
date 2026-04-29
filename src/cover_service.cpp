#include "cover_service.h"

#include "cover_resolver.h"
#include "epub_cover_cache.h"
#include "path_adapter.h"
#include "pdf_reader.h"

#include <algorithm>
#include <cmath>
#include "filesystem_compat.h"
#include <fstream>
#include <sstream>
#include <vector>

namespace {
int ClampIntLocal(int value, int lo, int hi) {
  return std::max(lo, std::min(hi, value));
}

std::filesystem::path SelectCoverCacheDir(const std::string &doc_path, const CoverServiceDeps &deps) {
  if (!deps.removable_cover_thumb_cache_dir.empty() &&
      (doc_path == "/mnt/sdcard" || doc_path.rfind("/mnt/sdcard/", 0) == 0)) {
    return deps.removable_cover_thumb_cache_dir;
  }
  return deps.cover_thumb_cache_dir;
}

std::string MakePdfCoverCacheKey(const std::string &doc_path, const CoverServiceDeps &deps) {
  std::error_code ec;
  const uintmax_t file_size = std::filesystem::file_size(std::filesystem::path(doc_path), ec);
  const auto mtime_raw = std::filesystem::last_write_time(std::filesystem::path(doc_path), ec);
  const long long file_mtime = ec ? 0LL : static_cast<long long>(mtime_raw.time_since_epoch().count());
  return deps.normalize_path_key(doc_path) + "|" + std::to_string(ec ? 0 : file_size) + "|" +
         std::to_string(file_mtime) + "|" + std::to_string(deps.cover_w) + "x" +
         std::to_string(deps.cover_h);
}

std::filesystem::path GetPdfCoverCacheFile(const std::string &doc_path, const CoverServiceDeps &deps) {
  const size_t hash_value = std::hash<std::string>{}(MakePdfCoverCacheKey(doc_path, deps));
  std::ostringstream oss;
  oss << std::hex << hash_value << ".bmp";
  return SelectCoverCacheDir(doc_path, deps) / oss.str();
}

std::string MakeManualCoverCacheKey(const std::string &item_path, const std::string &cover_path,
                                    const CoverServiceDeps &deps) {
  std::error_code ec;
  const std::filesystem::path cover_fs_path(cover_path);
  const uintmax_t file_size = std::filesystem::file_size(cover_fs_path, ec);
  const auto mtime_raw = std::filesystem::last_write_time(cover_fs_path, ec);
  const long long file_mtime = ec ? 0LL : static_cast<long long>(mtime_raw.time_since_epoch().count());
  return deps.normalize_path_key(item_path) + "|" + deps.normalize_path_key(cover_path) + "|" +
         std::to_string(ec ? 0 : file_size) + "|" + std::to_string(file_mtime) + "|" +
         std::to_string(deps.cover_w) + "x" + std::to_string(deps.cover_h);
}

std::filesystem::path GetManualCoverCacheFile(const std::string &item_path, const std::string &cover_path,
                                              const CoverServiceDeps &deps) {
  const size_t hash_value = std::hash<std::string>{}(MakeManualCoverCacheKey(item_path, cover_path, deps));
  std::ostringstream oss;
  oss << "manual_" << std::hex << hash_value << ".bmp";
  return SelectCoverCacheDir(item_path, deps) / oss.str();
}

std::string ManualCoverCacheKey(const BookItem &item, const CoverServiceDeps &deps) {
  const std::string item_path = item.real_path.empty() ? item.path : item.real_path;
  std::ostringstream oss;
  oss << (item.is_dir ? "dir|" : "file|") << deps.normalize_path_key(item_path);
  for (const auto &root : deps.cover_roots) {
    oss << "|" << deps.normalize_path_key(root);
  }
  return oss.str();
}

SDL_Texture *LoadCachedPdfCoverTexture(const std::string &doc_path, CoverServiceDeps &deps) {
  const std::filesystem::path cache_file = GetPdfCoverCacheFile(doc_path, deps);
  std::error_code ec;
  if (!std::filesystem::exists(cache_file, ec) || ec) return nullptr;
  SDL_Surface *cover_surface = deps.load_surface_from_file(cache_file.string());
  if (!cover_surface) return nullptr;
  SDL_Texture *normalized =
      deps.create_normalized_cover_texture(deps.renderer, cover_surface, deps.cover_w, deps.cover_h, deps.cover_aspect);
  if (!normalized) normalized = deps.create_texture_from_surface(deps.renderer, cover_surface);
  SDL_FreeSurface(cover_surface);
  if (normalized) {
    deps.remember_texture_size(normalized, deps.cover_w, deps.cover_h);
  }
  return normalized;
}

SDL_Surface *CreateNormalizedCoverSurface(SDL_Surface *src_surface, int cover_w, int cover_h, float cover_aspect) {
  if (!src_surface || src_surface->w <= 0 || src_surface->h <= 0 || cover_w <= 0 || cover_h <= 0) return nullptr;
  SDL_Surface *dst_surface = SDL_CreateRGBSurfaceWithFormat(0, cover_w, cover_h, 32, SDL_PIXELFORMAT_RGBA32);
  if (!dst_surface) return nullptr;

  const float src_aspect = static_cast<float>(src_surface->w) / static_cast<float>(src_surface->h);
  SDL_Rect src{0, 0, src_surface->w, src_surface->h};
  if (src_aspect > cover_aspect) {
    src.w = std::max(1, static_cast<int>(std::round(static_cast<float>(src_surface->h) * cover_aspect)));
    src.x = (src_surface->w - src.w) / 2;
  } else if (src_aspect < cover_aspect) {
    src.h = std::max(1, static_cast<int>(std::round(static_cast<float>(src_surface->w) / cover_aspect)));
    src.y = (src_surface->h - src.h) / 2;
  }

  if (SDL_BlitScaled(src_surface, &src, dst_surface, nullptr) != 0) {
    SDL_FreeSurface(dst_surface);
    return nullptr;
  }
  return dst_surface;
}

void SavePdfCoverCacheToDisk(const std::string &doc_path, const std::vector<unsigned char> &cover_rgba,
                             const CoverServiceDeps &deps) {
  if (cover_rgba.size() != static_cast<size_t>(deps.cover_w * deps.cover_h * 4)) return;
  std::error_code ec;
  const std::filesystem::path cache_dir = SelectCoverCacheDir(doc_path, deps);
  std::filesystem::create_directories(cache_dir, ec);
  SDL_Surface *surface = SDL_CreateRGBSurfaceWithFormatFrom(
      const_cast<unsigned char *>(cover_rgba.data()), deps.cover_w, deps.cover_h, 32, deps.cover_w * 4,
      SDL_PIXELFORMAT_RGBA32);
  if (!surface) return;
  SDL_SaveBMP(surface, GetPdfCoverCacheFile(doc_path, deps).string().c_str());
  SDL_FreeSurface(surface);
}

SDL_Texture *LoadManualCoverFromPath(const BookItem &item, const std::string &cover_path, CoverServiceDeps &deps) {
  if (cover_path.empty()) return nullptr;
  const std::string item_path = item.real_path.empty() ? item.path : item.real_path;
  const std::filesystem::path cache_file = GetManualCoverCacheFile(item_path, cover_path, deps);
  std::error_code ec;
  if (std::filesystem::exists(cache_file, ec) && !ec) {
    if (SDL_Surface *cached_surface = deps.load_surface_from_file(cache_file.string())) {
      SDL_Texture *cached = deps.create_texture_from_surface(deps.renderer, cached_surface);
      SDL_FreeSurface(cached_surface);
      if (cached) {
        deps.remember_texture_size(cached, deps.cover_w, deps.cover_h);
        return cached;
      }
    }
  }

  SDL_Surface *cover_surface = deps.load_surface_from_file(cover_path);
  if (!cover_surface) return nullptr;
  SDL_Surface *normalized_surface =
      CreateNormalizedCoverSurface(cover_surface, deps.cover_w, deps.cover_h, deps.cover_aspect);
  SDL_FreeSurface(cover_surface);
  if (!normalized_surface) return nullptr;

  std::filesystem::create_directories(cache_file.parent_path(), ec);
  SDL_SaveBMP(normalized_surface, cache_file.string().c_str());

  SDL_Texture *texture = deps.create_texture_from_surface(deps.renderer, normalized_surface);
  SDL_FreeSurface(normalized_surface);
  if (texture) deps.remember_texture_size(texture, deps.cover_w, deps.cover_h);
  return texture;
}

SDL_Texture *LoadManualCoverExactThenFuzzy(const BookItem &item, CoverServiceDeps &deps) {
  if (deps.manual_cover_path_cache) {
    const std::string cache_key = ManualCoverCacheKey(item, deps);
    auto cached = deps.manual_cover_path_cache->find(cache_key);
    if (cached != deps.manual_cover_path_cache->end()) {
      if (cached->second.empty()) return nullptr;
      SDL_Texture *texture = LoadManualCoverFromPath(item, cached->second, deps);
      if (texture) return texture;
      deps.manual_cover_path_cache->erase(cached);
    }

    const std::string exact_cover_path =
        cover_resolver::ResolveCoverPathExact(item.path, item.is_dir, deps.cover_roots);
    if (!exact_cover_path.empty()) {
      (*deps.manual_cover_path_cache)[cache_key] = exact_cover_path;
      SDL_Texture *texture = LoadManualCoverFromPath(item, exact_cover_path, deps);
      if (texture) return texture;
    }
    const std::string fuzzy_cover_path =
        cover_resolver::ResolveCoverPathFuzzy(item.path, item.is_dir, deps.cover_roots);
    if (!fuzzy_cover_path.empty()) {
      (*deps.manual_cover_path_cache)[cache_key] = fuzzy_cover_path;
      SDL_Texture *texture = LoadManualCoverFromPath(item, fuzzy_cover_path, deps);
      if (texture) return texture;
    }
    (*deps.manual_cover_path_cache)[cache_key] = std::string();
    return nullptr;
  }

  const std::string exact_cover_path =
      cover_resolver::ResolveCoverPathExact(item.path, item.is_dir, deps.cover_roots);
  if (!exact_cover_path.empty()) {
    SDL_Texture *texture = LoadManualCoverFromPath(item, exact_cover_path, deps);
    if (texture) return texture;
  }
  const std::string fuzzy_cover_path =
      cover_resolver::ResolveCoverPathFuzzy(item.path, item.is_dir, deps.cover_roots);
  if (!fuzzy_cover_path.empty()) {
    SDL_Texture *texture = LoadManualCoverFromPath(item, fuzzy_cover_path, deps);
    if (texture) return texture;
  }
  return nullptr;
}

std::string FindFirstDocInFolder(const std::string &folder_path, const std::string &wanted_ext,
                                 const CoverServiceDeps &deps) {
  std::error_code ec;
  const std::filesystem::path root(folder_path);
  if (!std::filesystem::exists(root, ec) || !std::filesystem::is_directory(root, ec)) return {};
  const auto opts = std::filesystem::directory_options::skip_permission_denied;
  std::vector<std::string> matches;
  for (std::filesystem::recursive_directory_iterator it(root, opts, ec), end; it != end; it.increment(ec)) {
    if (ec) continue;
    const auto &entry = *it;
    if (!filesystem_compat::IsRegularFile(entry, ec)) continue;
    if (deps.get_lower_ext(entry.path().string()) != wanted_ext) continue;
    matches.push_back(path_adapter::ResolveReadableFilePath(entry));
  }
  std::sort(matches.begin(), matches.end());
  return matches.empty() ? std::string{} : matches.front();
}

std::string FindPreferredFolderCoverDoc(const std::string &folder_path,
                                        const CoverServiceDeps &deps) {
  // Match the existing card1 behavior:
  // 1. Prefer the first PDF found under the folder.
  // 2. If no PDF exists, fallback to the first EPUB found.
  const std::string first_pdf = FindFirstDocInFolder(folder_path, ".pdf", deps);
  if (!first_pdf.empty()) return first_pdf;
  return FindFirstDocInFolder(folder_path, ".epub", deps);
}

SDL_Texture *CreateEpubFirstImageCoverTextureLocalImpl(const std::string &doc_path, CoverServiceDeps &deps) {
  EpubCoverTextureDeps cover_deps{
      deps.renderer,
      deps.cover_w,
      deps.cover_h,
      SelectCoverCacheDir(doc_path, deps),
      deps.normalize_path_key,
      deps.load_surface_from_file,
      deps.load_surface_from_memory,
      deps.create_normalized_cover_texture,
      deps.create_texture_from_surface,
      deps.remember_texture_size,
  };
  return CreateEpubFirstImageCoverTexture(doc_path, cover_deps);
}
}

SDL_Texture *CreateEpubFirstImageCoverTextureLocal(const std::string &doc_path, CoverServiceDeps &deps) {
  return CreateEpubFirstImageCoverTextureLocalImpl(doc_path, deps);
}

bool HasManualCoverExactOrFuzzy(const BookItem &item, const CoverServiceDeps &deps) {
  const std::string exact_cover_path =
      cover_resolver::ResolveCoverPathExact(item.path, item.is_dir, deps.cover_roots);
  if (!exact_cover_path.empty()) return true;
  const std::string fuzzy_cover_path =
      cover_resolver::ResolveCoverPathFuzzy(item.path, item.is_dir, deps.cover_roots);
  return !fuzzy_cover_path.empty();
}

bool HasCachedDocCoverOnDisk(const std::string &doc_path, const CoverServiceDeps &deps) {
  const std::string ext = deps.get_lower_ext ? deps.get_lower_ext(doc_path) : std::string{};
  std::error_code ec;
  if (ext == ".pdf") {
    return std::filesystem::exists(GetPdfCoverCacheFile(doc_path, deps), ec) && !ec;
  }
  if (ext == ".epub") {
    EpubCoverTextureDeps cover_deps{
        deps.renderer,
        deps.cover_w,
        deps.cover_h,
        SelectCoverCacheDir(doc_path, deps),
        deps.normalize_path_key,
        deps.load_surface_from_file,
        deps.load_surface_from_memory,
        deps.create_normalized_cover_texture,
        deps.create_texture_from_surface,
        deps.remember_texture_size,
    };
    return HasCachedEpubCoverOnDisk(doc_path, cover_deps);
  }
  return false;
}

SDL_Texture *CreatePdfFirstPageCoverTexture(const std::string &doc_path, CoverServiceDeps &deps) {
  if (SDL_Texture *cached = LoadCachedPdfCoverTexture(doc_path, deps)) return cached;

  PdfReader preview_pdf;
  if (!preview_pdf.Open(doc_path) || !preview_pdf.HasRealRenderer()) {
    preview_pdf.Close();
    return nullptr;
  }

  int page_w = 0;
  int page_h = 0;
  if (!preview_pdf.PageSize(0, page_w, page_h) || page_w <= 0 || page_h <= 0) {
    preview_pdf.Close();
    return nullptr;
  }

  const float desired_w = static_cast<float>(deps.cover_w) * 1.6f;
  const float desired_h = static_cast<float>(deps.cover_h) * 1.6f;
  const float scale_w = desired_w / static_cast<float>(std::max(1, page_w));
  const float scale_h = desired_h / static_cast<float>(std::max(1, page_h));
  const float render_scale = std::max(0.1f, std::max(scale_w, scale_h));

  std::vector<unsigned char> rgba;
  int src_w = 0;
  int src_h = 0;
  if (!preview_pdf.RenderPageRGBA(0, render_scale, rgba, src_w, src_h) || src_w <= 0 || src_h <= 0) {
    preview_pdf.Close();
    return nullptr;
  }
  preview_pdf.Close();

  SDL_Rect src_crop{0, 0, src_w, src_h};
  const float src_aspect = static_cast<float>(src_w) / static_cast<float>(src_h);
  if (src_aspect > deps.cover_aspect) {
    src_crop.w = std::max(1, static_cast<int>(std::round(static_cast<float>(src_h) * deps.cover_aspect)));
    src_crop.x = (src_w - src_crop.w) / 2;
  } else if (src_aspect < deps.cover_aspect) {
    src_crop.h = std::max(1, static_cast<int>(std::round(static_cast<float>(src_w) / deps.cover_aspect)));
    src_crop.y = (src_h - src_crop.h) / 2;
  }

  std::vector<unsigned char> cover_rgba(static_cast<size_t>(deps.cover_w * deps.cover_h * 4), 0);
  for (int dy = 0; dy < deps.cover_h; ++dy) {
    const int sy = src_crop.y + ((dy * src_crop.h + (deps.cover_h / 2)) / deps.cover_h);
    const int sy_clamped = ClampIntLocal(sy, 0, src_h - 1);
    for (int dx = 0; dx < deps.cover_w; ++dx) {
      const int sx = src_crop.x + ((dx * src_crop.w + (deps.cover_w / 2)) / deps.cover_w);
      const int sx_clamped = ClampIntLocal(sx, 0, src_w - 1);
      const size_t si = static_cast<size_t>((sy_clamped * src_w + sx_clamped) * 4);
      const size_t di = static_cast<size_t>((dy * deps.cover_w + dx) * 4);
      cover_rgba[di + 0] = rgba[si + 0];
      cover_rgba[di + 1] = rgba[si + 1];
      cover_rgba[di + 2] = rgba[si + 2];
      cover_rgba[di + 3] = rgba[si + 3];
    }
  }

  SDL_Texture *texture =
      SDL_CreateTexture(deps.renderer, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_STATIC, deps.cover_w, deps.cover_h);
  if (!texture) return nullptr;
  SDL_SetTextureBlendMode(texture, SDL_BLENDMODE_BLEND);
  if (SDL_UpdateTexture(texture, nullptr, cover_rgba.data(), deps.cover_w * 4) != 0) {
    SDL_DestroyTexture(texture);
    return nullptr;
  }
  SavePdfCoverCacheToDisk(doc_path, cover_rgba, deps);
  return texture;
}

SDL_Texture *ResolveBookCoverTexture(const BookItem &item, ShelfCategory category, CoverServiceDeps &deps) {
  const bool txt_folder = item.is_dir && category == ShelfCategory::AllBooks;
  const bool txt_file = !item.is_dir && deps.get_lower_ext(item.path) == ".txt";
  if (txt_folder || txt_file) {
    if (SDL_Texture *texture = LoadManualCoverExactThenFuzzy(item, deps)) return texture;
    return deps.shared_txt_cover;
  }

  if (item.is_dir) {
    if (SDL_Texture *texture = LoadManualCoverExactThenFuzzy(item, deps)) return texture;
    const std::string preferred_doc = FindPreferredFolderCoverDoc(item.path, deps);
    if (!preferred_doc.empty()) {
      const std::string ext = deps.get_lower_ext(preferred_doc);
      if (ext == ".pdf") {
        if (SDL_Texture *texture = CreatePdfFirstPageCoverTexture(preferred_doc, deps)) return texture;
      } else if (ext == ".epub") {
        if (SDL_Texture *texture = CreateEpubFirstImageCoverTextureLocal(preferred_doc, deps)) return texture;
      }
    }
    return deps.shared_pdf_cover ? deps.shared_pdf_cover : deps.shared_txt_cover;
  }

  const std::string ext = deps.get_lower_ext(item.path);
  if (ext != ".pdf" && ext != ".epub") {
    return LoadManualCoverExactThenFuzzy(item, deps);
  }

  if (SDL_Texture *texture = LoadManualCoverExactThenFuzzy(item, deps)) return texture;
  if (ext == ".pdf") {
    if (SDL_Texture *texture = CreatePdfFirstPageCoverTexture(item.path, deps)) return texture;
  } else {
    if (SDL_Texture *texture = CreateEpubFirstImageCoverTextureLocal(item.path, deps)) return texture;
  }
  return deps.shared_pdf_cover ? deps.shared_pdf_cover : deps.shared_txt_cover;
}
