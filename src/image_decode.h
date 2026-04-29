#pragma once

#include <SDL.h>

#include <cstddef>

SDL_Surface *DecodeSurfaceFromMemory(const void *data, size_t size);
SDL_Surface *DecodeSurfaceFromMemoryFit(const void *data, size_t size, int max_w, int max_h);
