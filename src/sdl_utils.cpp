#include "sdl_utils.h"
#include "image_decode.h"

#include <SDL.h>
#ifdef HAVE_SDL2_IMAGE
#include <SDL_image.h>
#endif

#include <algorithm>
#include <cmath>

SDL_Texture *LoadTextureFromFile(SDL_Renderer *renderer, const std::string &path) {
#ifdef HAVE_SDL2_IMAGE
  SDL_Surface *surface = IMG_Load(path.c_str());
#else
  SDL_Surface *surface = SDL_LoadBMP(path.c_str());
#endif
  if (!surface) return nullptr;
  SDL_Texture *tex = SDL_CreateTextureFromSurface(renderer, surface);
  SDL_FreeSurface(surface);
  return tex;
}

SDL_Surface *LoadSurfaceFromFile(const std::string &path) {
#ifdef HAVE_SDL2_IMAGE
  return IMG_Load(path.c_str());
#else
  return SDL_LoadBMP(path.c_str());
#endif
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

  if (SDL_BlitScaled(src_surface, &src, dst_surface, nullptr) != 0) {
    SDL_FreeSurface(dst_surface);
    return nullptr;
  }
  SDL_Texture *tex = SDL_CreateTextureFromSurface(renderer, dst_surface);
  SDL_FreeSurface(dst_surface);
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
