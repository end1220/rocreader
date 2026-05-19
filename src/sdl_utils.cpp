#include "sdl_utils.h"
#include "image_decode.h"

#include <SDL.h>
#ifdef HAVE_SDL2_IMAGE
#include <SDL_image.h>
#endif

#include <algorithm>
#include <cmath>
#include <fstream>
#include <vector>

namespace {
SDL_Surface *LoadSurfaceFromFileBytes(const std::string &path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) return nullptr;
  std::vector<unsigned char> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  if (bytes.empty()) return nullptr;
  return DecodeSurfaceFromMemory(bytes.data(), bytes.size());
}

void ApplyImageTextureFiltering(SDL_Texture *texture) {
#if SDL_VERSION_ATLEAST(2, 0, 12)
  if (texture) SDL_SetTextureScaleMode(texture, SDL_ScaleModeLinear);
#else
  (void)texture;
#endif
}

bool ResampleRgbaSurface(SDL_Surface *src, const SDL_Rect &src_rect, SDL_Surface *dst) {
  if (!src || !dst || src_rect.w <= 0 || src_rect.h <= 0 || dst->w <= 0 || dst->h <= 0) return false;
  SDL_Surface *rgba = SDL_ConvertSurfaceFormat(src, SDL_PIXELFORMAT_RGBA32, 0);
  if (!rgba) return false;
  for (int y = 0; y < dst->h; ++y) {
    for (int x = 0; x < dst->w; ++x) {
      const double x0 = src_rect.x + static_cast<double>(x) * src_rect.w / dst->w;
      const double x1 = src_rect.x + static_cast<double>(x + 1) * src_rect.w / dst->w;
      const double y0 = src_rect.y + static_cast<double>(y) * src_rect.h / dst->h;
      const double y1 = src_rect.y + static_cast<double>(y + 1) * src_rect.h / dst->h;
      const int ix0 = std::max(src_rect.x, static_cast<int>(std::floor(x0)));
      const int ix1 = std::min(src_rect.x + src_rect.w, static_cast<int>(std::ceil(x1)));
      const int iy0 = std::max(src_rect.y, static_cast<int>(std::floor(y0)));
      const int iy1 = std::min(src_rect.y + src_rect.h, static_cast<int>(std::ceil(y1)));
      double sum[4] = {0.0, 0.0, 0.0, 0.0};
      double total = 0.0;
      for (int sy = iy0; sy < iy1; ++sy) {
        const double wy = std::max(0.0, std::min(y1, static_cast<double>(sy + 1)) -
                                            std::max(y0, static_cast<double>(sy)));
        if (wy <= 0.0) continue;
        const unsigned char *row = static_cast<const unsigned char *>(rgba->pixels) + sy * rgba->pitch;
        for (int sx = ix0; sx < ix1; ++sx) {
          const double wx = std::max(0.0, std::min(x1, static_cast<double>(sx + 1)) -
                                              std::max(x0, static_cast<double>(sx)));
          const double weight = wx * wy;
          if (weight <= 0.0) continue;
          const unsigned char *p = row + sx * 4;
          for (int c = 0; c < 4; ++c) sum[c] += static_cast<double>(p[c]) * weight;
          total += weight;
        }
      }
      if (total <= 0.0) total = 1.0;
      unsigned char *out = static_cast<unsigned char *>(dst->pixels) + y * dst->pitch + x * 4;
      for (int c = 0; c < 4; ++c) {
        out[c] = static_cast<unsigned char>(std::clamp(static_cast<int>(std::lround(sum[c] / total)), 0, 255));
      }
    }
  }
  SDL_FreeSurface(rgba);
  return true;
}
} // namespace

SDL_Texture *LoadTextureFromFile(SDL_Renderer *renderer, const std::string &path) {
#ifdef HAVE_SDL2_IMAGE
  SDL_Surface *surface = IMG_Load(path.c_str());
#else
  SDL_Surface *surface = SDL_LoadBMP(path.c_str());
#endif
  if (!surface) surface = LoadSurfaceFromFileBytes(path);
  if (!surface) return nullptr;
  SDL_Texture *tex = SDL_CreateTextureFromSurface(renderer, surface);
  SDL_FreeSurface(surface);
  return tex;
}

SDL_Surface *LoadSurfaceFromFile(const std::string &path) {
#ifdef HAVE_SDL2_IMAGE
  if (SDL_Surface *surface = IMG_Load(path.c_str())) return surface;
#else
  if (SDL_Surface *surface = SDL_LoadBMP(path.c_str())) return surface;
#endif
  return LoadSurfaceFromFileBytes(path);
}

SDL_Surface *LoadSurfaceFromMemory(const void *data, size_t size) {
  return DecodeSurfaceFromMemory(data, size);
}

SDL_Texture *CreateNormalizedCoverTexture(SDL_Renderer *renderer, SDL_Surface *src_surface, int cover_w,
                                          int cover_h, float cover_aspect) {
  if (!renderer || !src_surface || src_surface->w <= 0 || src_surface->h <= 0) return nullptr;
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

  if (!ResampleRgbaSurface(src_surface, src, dst_surface)) {
    SDL_FreeSurface(dst_surface);
    return nullptr;
  }
  SDL_Texture *tex = SDL_CreateTextureFromSurface(renderer, dst_surface);
  SDL_FreeSurface(dst_surface);
  ApplyImageTextureFiltering(tex);
  return tex;
}

SDL_Texture *CreateTextureFromSurface(SDL_Renderer *renderer, SDL_Surface *surface) {
  if (!renderer || !surface) return nullptr;
  return SDL_CreateTextureFromSurface(renderer, surface);
}

SDL_Texture *CreateScaledTextureCache(SDL_Renderer *renderer, SDL_Texture *source, int width, int height) {
  if (!renderer || !source || width <= 0 || height <= 0) return nullptr;

  SDL_Texture *target = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGBA8888, SDL_TEXTUREACCESS_TARGET, width, height);
  if (!target) return nullptr;

  SDL_SetTextureBlendMode(target, SDL_BLENDMODE_BLEND);
  SDL_Texture *previous_target = SDL_GetRenderTarget(renderer);
  if (SDL_SetRenderTarget(renderer, target) != 0) {
    SDL_DestroyTexture(target);
    return nullptr;
  }

  SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
  SDL_SetRenderDrawColor(renderer, 0, 0, 0, 0);
  SDL_RenderClear(renderer);

  SDL_Rect dst{0, 0, width, height};
  SDL_RenderCopy(renderer, source, nullptr, &dst);

  SDL_SetRenderTarget(renderer, previous_target);
  return target;
}

void DrawRect(SDL_Renderer *renderer, int x, int y, int w, int h, SDL_Color color, bool fill) {
  SDL_SetRenderDrawColor(renderer, color.r, color.g, color.b, color.a);
  SDL_Rect rc{x, y, w, h};
  if (fill) SDL_RenderFillRect(renderer, &rc);
  else SDL_RenderDrawRect(renderer, &rc);
}

int StretchHeightForWidth(int src_w, int src_h, int target_w) {
  if (src_w <= 0 || src_h <= 0 || target_w <= 0) return 0;
  return std::max(1, static_cast<int>(std::round(static_cast<float>(src_h) * static_cast<float>(target_w) /
                                                   static_cast<float>(src_w))));
}

void RenderTextureStretchToWidthKeepAspect(SDL_Renderer *renderer, SDL_Texture *tex, int src_w, int src_h, int x,
                                           int y, int target_w) {
  if (!renderer || !tex || src_w <= 0 || src_h <= 0 || target_w <= 0) return;
  const int target_h = StretchHeightForWidth(src_w, src_h, target_w);
  SDL_Rect dst{x, y, target_w, target_h};
  SDL_RenderCopy(renderer, tex, nullptr, &dst);
}
