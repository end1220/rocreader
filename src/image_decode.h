#pragma once

#include <SDL.h>

#include <cstddef>

SDL_Surface *DecodeSurfaceFromMemory(const void *data, size_t size);

