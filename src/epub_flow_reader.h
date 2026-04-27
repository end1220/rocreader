#pragma once

#include <SDL.h>

#include <string>

struct EpubRuntimeProgress;

class EpubFlowReader {
public:
  EpubFlowReader();
  ~EpubFlowReader();

  bool Open(const std::string &path, SDL_Renderer *renderer, int screen_w, int screen_h,
            const EpubRuntimeProgress &initial_progress, int base_font_pt = 18,
            SDL_Color background_color = SDL_Color{250, 249, 244, 255},
            SDL_Color font_color = SDL_Color{28, 28, 28, 255});
  void Close();

  bool IsOpen() const;
  bool HasRealRenderer() const;
  const char *BackendName() const;
  bool IsRenderPending() const;

  void UpdateViewport(int screen_w, int screen_h);
  void Tick();
  void Draw(SDL_Renderer *renderer) const;

  void RotateLeft();
  void RotateRight();
  void ZoomOut();
  void ZoomIn();
  void ResetView();
  void SetBaseFontPointSize(int base_font_pt);
  void SetColors(SDL_Color background_color, SDL_Color font_color);
  void ScrollByPixels(int delta_px);
  void JumpByScreen(int direction);
  void SetPage(int page_index);

  int PageCount() const;
  bool PageSize(int page_index, int &w, int &h) const;
  int CurrentPage() const;
  EpubRuntimeProgress Progress() const;

  static bool LooksLikeMixedLayout(const std::string &path);
  static bool ExtractFirstDocumentImage(const std::string &path, std::string &bytes, std::string &error);

private:
  struct Impl;
  Impl *impl_ = nullptr;
};
