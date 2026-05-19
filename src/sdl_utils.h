#pragma once

#include <SDL.h>

#include <cstddef>
#include <string>

SDL_Texture *LoadTextureFromFile(SDL_Renderer *renderer, const std::string &path);
SDL_Surface *LoadSurfaceFromFile(const std::string &path);
SDL_Surface *LoadSurfaceFromMemory(const void *data, size_t size);
SDL_Texture *CreateNormalizedCoverTexture(SDL_Renderer *renderer, SDL_Surface *src_surface, int cover_w,
                                          int cover_h, float cover_aspect);
SDL_Texture *CreateTextureFromSurface(SDL_Renderer *renderer, SDL_Surface *surface);
SDL_Texture *CreateScaledTextureCache(SDL_Renderer *renderer, SDL_Texture *source, int width, int height);
void DrawRect(SDL_Renderer *renderer, int x, int y, int w, int h, SDL_Color color, bool fill = true);
int StretchHeightForWidth(int src_w, int src_h, int target_w);
void RenderTextureStretchToWidthKeepAspect(SDL_Renderer *renderer, SDL_Texture *tex, int src_w, int src_h,
                                           int x, int y, int target_w);
