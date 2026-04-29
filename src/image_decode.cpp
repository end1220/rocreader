#include "image_decode.h"

#include "runtime_log.h"

#include <SDL.h>
#ifdef HAVE_SDL2_IMAGE
#include <SDL_image.h>
#endif
#ifdef HAVE_WEBP
#include <webp/decode.h>
#endif
#ifdef HAVE_JPEG
extern "C" {
#include <jpeglib.h>
}
#include <setjmp.h>
#endif

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace {

#ifdef HAVE_WEBP
SDL_Surface *DecodeWebpSurface(const uint8_t *data, size_t size) {
  int webp_w = 0;
  int webp_h = 0;
  if (!WebPGetInfo(data, size, &webp_w, &webp_h) || webp_w <= 0 || webp_h <= 0) return nullptr;
  SDL_Surface *surface = SDL_CreateRGBSurfaceWithFormat(0, webp_w, webp_h, 32, SDL_PIXELFORMAT_RGBA32);
  if (!surface) return nullptr;
  uint8_t *decoded = WebPDecodeRGBAInto(data, size, static_cast<uint8_t *>(surface->pixels),
                                        surface->pitch * surface->h, surface->pitch);
  if (!decoded) {
    SDL_FreeSurface(surface);
    return nullptr;
  }
  return surface;
}
#endif

#ifdef HAVE_JPEG
struct JpegErrorState {
  jpeg_error_mgr pub;
  jmp_buf jump;
  char message[JMSG_LENGTH_MAX];
};

void JpegErrorExit(j_common_ptr cinfo) {
  auto *err = reinterpret_cast<JpegErrorState *>(cinfo->err);
  err->message[0] = '\0';
  (*cinfo->err->format_message)(cinfo, err->message);
  longjmp(err->jump, 1);
}

SDL_Surface *DecodeJpegSurface(const uint8_t *data, size_t size, int max_w = 0, int max_h = 0) {
  jpeg_decompress_struct cinfo{};
  JpegErrorState jerr{};
  cinfo.err = jpeg_std_error(&jerr.pub);
  jerr.pub.error_exit = JpegErrorExit;
  if (setjmp(jerr.jump)) {
    runtime_log::Line(std::string("[image_decode] jpeg decode failed: ") +
                      (jerr.message[0] ? jerr.message : "unknown libjpeg error") +
                      " bytes=" + std::to_string(size));
    jpeg_destroy_decompress(&cinfo);
    return nullptr;
  }
  jpeg_create_decompress(&cinfo);
  jpeg_mem_src(&cinfo, const_cast<unsigned char *>(data), static_cast<unsigned long>(size));
  if (jpeg_read_header(&cinfo, TRUE) != JPEG_HEADER_OK) {
    runtime_log::Line("[image_decode] jpeg header failed bytes=" + std::to_string(size));
    jpeg_destroy_decompress(&cinfo);
    return nullptr;
  }
  if (max_w > 0 && max_h > 0 &&
      (static_cast<int>(cinfo.image_width) > max_w || static_cast<int>(cinfo.image_height) > max_h)) {
    int denom = 1;
    while (denom < 8 &&
           static_cast<int>(cinfo.image_width / (denom * 2)) >= max_w &&
           static_cast<int>(cinfo.image_height / (denom * 2)) >= max_h) {
      denom *= 2;
    }
    cinfo.scale_num = 1;
    cinfo.scale_denom = denom;
  }
  cinfo.out_color_space = JCS_RGB;
  if (!jpeg_start_decompress(&cinfo)) {
    runtime_log::Line("[image_decode] jpeg start failed bytes=" + std::to_string(size));
    jpeg_destroy_decompress(&cinfo);
    return nullptr;
  }

  const int w = static_cast<int>(cinfo.output_width);
  const int h = static_cast<int>(cinfo.output_height);
  const int components = static_cast<int>(cinfo.output_components);
  if (w <= 0 || h <= 0 || components <= 0) {
    runtime_log::Line("[image_decode] jpeg invalid size=" + std::to_string(w) + "x" +
                      std::to_string(h) + " components=" + std::to_string(components));
    jpeg_finish_decompress(&cinfo);
    jpeg_destroy_decompress(&cinfo);
    return nullptr;
  }
  SDL_Surface *surface = SDL_CreateRGBSurfaceWithFormat(0, w, h, 32, SDL_PIXELFORMAT_RGBA32);
  if (!surface) {
    runtime_log::Line("[image_decode] jpeg surface create failed size=" + std::to_string(w) + "x" +
                      std::to_string(h) + " err=" + SDL_GetError());
    jpeg_finish_decompress(&cinfo);
    jpeg_destroy_decompress(&cinfo);
    return nullptr;
  }

  std::vector<uint8_t> row(static_cast<size_t>(w) * static_cast<size_t>(components));
  while (cinfo.output_scanline < cinfo.output_height) {
    JSAMPROW row_ptr = row.data();
    const JDIMENSION y = cinfo.output_scanline;
    if (jpeg_read_scanlines(&cinfo, &row_ptr, 1) != 1) {
      runtime_log::Line("[image_decode] jpeg scanline read failed y=" + std::to_string(y) +
                        " size=" + std::to_string(w) + "x" + std::to_string(h));
      SDL_FreeSurface(surface);
      jpeg_finish_decompress(&cinfo);
      jpeg_destroy_decompress(&cinfo);
      return nullptr;
    }
    uint8_t *dst = static_cast<uint8_t *>(surface->pixels) + static_cast<size_t>(y) * surface->pitch;
    for (int x = 0; x < w; ++x) {
      const size_t si = static_cast<size_t>(x) * static_cast<size_t>(components);
      dst[x * 4 + 0] = row[si + 0];
      dst[x * 4 + 1] = components > 1 ? row[si + 1] : row[si + 0];
      dst[x * 4 + 2] = components > 2 ? row[si + 2] : row[si + 0];
      dst[x * 4 + 3] = 255;
    }
  }

  jpeg_finish_decompress(&cinfo);
  jpeg_destroy_decompress(&cinfo);
  return surface;
}
#endif

} // namespace

SDL_Surface *DecodeSurfaceFromMemory(const void *data, size_t size) {
  if (!data || size == 0) return nullptr;
  const auto *bytes = static_cast<const uint8_t *>(data);
  SDL_RWops *rw = SDL_RWFromConstMem(data, static_cast<int>(size));
  if (rw) {
#ifdef HAVE_SDL2_IMAGE
    if (SDL_Surface *surface = IMG_Load_RW(rw, 1)) return surface;
#else
    if (SDL_Surface *surface = SDL_LoadBMP_RW(rw, 1)) return surface;
#endif
  }
#ifdef HAVE_WEBP
  if (SDL_Surface *surface = DecodeWebpSurface(bytes, size)) return surface;
#endif
#ifdef HAVE_JPEG
  if (SDL_Surface *surface = DecodeJpegSurface(bytes, size)) return surface;
#endif
  runtime_log::Line("[image_decode] no decoder accepted bytes=" + std::to_string(size));
  return nullptr;
}

SDL_Surface *DecodeSurfaceFromMemoryFit(const void *data, size_t size, int max_w, int max_h) {
  if (!data || size == 0) return nullptr;
  const auto *bytes = static_cast<const uint8_t *>(data);
#ifdef HAVE_JPEG
  if (size >= 4 && bytes[0] == 0xFF && bytes[1] == 0xD8) {
    if (SDL_Surface *surface = DecodeJpegSurface(bytes, size, max_w, max_h)) return surface;
  }
#endif
  return DecodeSurfaceFromMemory(data, size);
}
