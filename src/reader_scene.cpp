#include "reader_scene.h"

#include "app_layout.h"
#include "chapter_detection.h"
#include "chapter_sidebar.h"

#include <algorithm>
#include <utility>

namespace {
constexpr Uint32 kTxtChapterClosedTickIntervalMs = 200;
constexpr size_t kTxtChapterClosedLineBudget = 256;
constexpr size_t kTxtChapterOpenLineBudget = 1024;

int TxtLayoutProgressPercentLocal(const TxtReaderState &txt) {
  if (!txt.open) return 0;
  if (!txt.loading) return 100;
  const size_t total = txt.layout_total_bytes > 0 ? txt.layout_total_bytes : txt.pending_raw.size();
  if (total == 0) return 0;
  return std::clamp(static_cast<int>((static_cast<int64_t>(txt.parse_pos) * 100) / total), 0, 100);
}

bool SupportsPageChapterList(ReaderMode mode, const IReaderModule *module) {
  if (!module || !module->IsOpen()) return false;
  if (mode == ReaderMode::Pdf || mode == ReaderMode::ZipImage) return true;
  if (mode == ReaderMode::Epub) {
    return std::string(module->BackendName()) != "epub-flow" &&
           module->Capabilities().is_image_sequence;
  }
  return false;
}

std::string PageChapterTitle(int page_index) {
  return u8"\u7b2c" + std::to_string(page_index + 1) + u8"\u9875";
}
}  // namespace

ReaderScene::ReaderScene(std::function<void()> on_reader_closed)
    : on_reader_closed_(std::move(on_reader_closed)) {}

ReaderSceneProgressInputConfig MakeReaderSceneProgressInputConfig(int tap_step_px) {
  return ReaderSceneProgressInputConfig{
      tap_step_px,
      1,
      0.25f,
      6.0f,
      26.0f,
      36.0f,
  };
}

ReaderSceneProgressOverlayMetrics MakeReaderSceneProgressOverlayMetrics(const LayoutMetrics &layout) {
  return ReaderSceneProgressOverlayMetrics{
      layout.reader_progress_panel_margin_x,
      layout.reader_progress_panel_margin_bottom,
      layout.reader_progress_bar_margin_x,
      layout.reader_progress_percent_margin_x,
  };
}

ReaderSceneInputServices MakeReaderSceneInputServices(
    std::function<void()> close_text_reader,
    std::function<void(const std::string &, bool)> persist_current_txt_resume_snapshot,
    ReaderSceneInputActions actions) {
  return ReaderSceneInputServices{
      std::move(close_text_reader),
      std::move(persist_current_txt_resume_snapshot),
      std::move(actions),
  };
}

ReaderSceneRenderServices MakeReaderSceneRenderServices(
    SDL_Renderer *renderer,
    std::function<int(int)> scale_px,
    std::function<void(int, int, int, int, SDL_Color, bool)> draw_rect,
    std::function<void()> clamp_text_scroll,
    std::function<TextCacheEntry *(const std::string &, SDL_Color)> get_text_texture,
    std::function<TextCacheEntry *(const std::string &, SDL_Color)> get_reader_text_texture) {
  return ReaderSceneRenderServices{
      std::move(scale_px),
      std::move(draw_rect),
      std::move(clamp_text_scroll),
      [renderer](const SDL_Rect &clip) {
        if (renderer) SDL_RenderSetClipRect(renderer, &clip);
      },
      [renderer]() {
        if (renderer) SDL_RenderSetClipRect(renderer, nullptr);
      },
      std::move(get_text_texture),
      std::move(get_reader_text_texture),
  };
}

bool ReaderScene::IsRenderPending(const ReaderUiState &ui, const ReaderManager *reader_manager) const {
  if (ui.chapter_sidebar_visible || ui.chapter_loading) return true;
  if (!reader_manager) return false;
  const IReaderModule *module = reader_manager->Module(ui.mode);
  return module && (module->IsRenderPending() || module->ChaptersLoading());
}

void ReaderScene::HandleInput(const ReaderSceneInputDeps &deps) const {
  auto active_module = [&]() -> IReaderModule * {
    return deps.reader_manager ? deps.reader_manager->Module(deps.ui.mode) : nullptr;
  };
  auto flow_epub_open = [&]() {
    IReaderModule *module = active_module();
    return deps.ui.mode == ReaderMode::Epub && module && module->IsOpen() &&
           std::string(module->BackendName()) == "epub-flow";
  };
  auto page_chapters_open = [&]() {
    return SupportsPageChapterList(deps.ui.mode, active_module());
  };
  auto refresh_txt_chapters = [&](bool reset_window) {
    const TxtReaderState &txt = deps.ui.Txt();
    if (deps.ui.txt_chapter_scan.cache_key != txt.cache_key) ResetTxtChapterScan(deps.ui, txt);
    const int loading_pct = TxtLayoutProgressPercentLocal(txt);
    const Uint32 now = SDL_GetTicks();
    const bool sidebar_active = deps.ui.chapter_sidebar_visible || reset_window;
    const bool tick_due = sidebar_active ||
                          deps.ui.txt_chapter_scan.last_tick_ms == 0 ||
                          now - deps.ui.txt_chapter_scan.last_tick_ms >= kTxtChapterClosedTickIntervalMs;
    bool changed = false;
    if (tick_due) {
      deps.ui.txt_chapter_scan.last_tick_ms = now;
      changed = TickTxtChapterScan(deps.ui, txt,
                                   sidebar_active ? kTxtChapterOpenLineBudget
                                                  : kTxtChapterClosedLineBudget);
    }
    const bool loading = txt.loading || !deps.ui.txt_chapter_scan.done;
    const std::string scan_key = txt.cache_key + "|" +
                                 std::to_string(deps.ui.txt_chapter_scan.scan_pos) + "|" +
                                 std::to_string(deps.ui.txt_chapter_scan.anchors.size()) + "|" +
                                 (loading ? "loading" : "done") + "|" +
                                 std::to_string(loading_pct);
    if (!changed && deps.ui.chapter_cache_key == scan_key && !deps.ui.chapter_anchors.empty()) {
      deps.ui.chapter_loading = loading;
      deps.ui.chapter_loading_pct = loading_pct;
      return;
    }
    if (!deps.ui.txt_chapter_scan.anchors.empty()) {
      deps.ui.chapter_anchors = deps.ui.txt_chapter_scan.anchors;
    } else {
      deps.ui.chapter_anchors.clear();
      deps.ui.chapter_anchors.push_back(MakeBodyChapterAnchor());
    }
    deps.ui.chapter_loading = loading;
    deps.ui.chapter_loading_pct = loading_pct;
    deps.ui.chapter_cache_key = scan_key;
    deps.ui.chapter_sidebar_selected =
        std::clamp(deps.ui.chapter_sidebar_selected, 0,
                   std::max(0, static_cast<int>(deps.ui.chapter_anchors.size()) - 1));
    if (reset_window) deps.ui.chapter_sidebar_first_visible = 0;
  };
  auto select_current_txt_chapter = [&]() {
    const TxtReaderState &txt = deps.ui.Txt();
    size_t current_offset = 0;
    if (!txt.line_source_offsets.empty()) {
      const size_t top_line = std::min(txt.line_source_offsets.size() - 1,
                                       static_cast<size_t>(std::max(0, txt.scroll_px / std::max(1, txt.line_h))));
      current_offset = txt.line_source_offsets[top_line];
    }
    int selected = 0;
    for (size_t i = 0; i < deps.ui.chapter_anchors.size(); ++i) {
      if (deps.ui.chapter_anchors[i].source_offset > current_offset) break;
      selected = static_cast<int>(i);
    }
    deps.ui.chapter_sidebar_selected = selected;
  };
  auto select_current_epub_chapter = [&]() {
    IReaderModule *module = active_module();
    const int current_scroll_y = module ? module->Progress().scroll_y : 0;
    int selected = 0;
    for (size_t i = 0; i < deps.ui.chapter_anchors.size(); ++i) {
      if (deps.ui.chapter_anchors[i].scroll_y > current_scroll_y) break;
      selected = static_cast<int>(i);
    }
    deps.ui.chapter_sidebar_selected = selected;
  };
  auto refresh_epub_chapters = [&](bool reset_window) {
    IReaderModule *module = active_module();
    const bool loading = module && module->ChaptersLoading();
    const int loading_pct = module ? module->ChaptersLoadingPercent() : 100;
    const std::string cache_key = deps.ui.current_book + "|epub-flow|" +
                                  (loading ? "loading" : "done") + "|" +
                                  std::to_string(module ? module->PageCount() : 0) + "|" +
                                  std::to_string(module ? module->Chapters().size() : 0) + "|" +
                                  std::to_string(loading_pct);
    if (deps.ui.chapter_cache_key == cache_key && !deps.ui.chapter_anchors.empty()) return;
    deps.ui.chapter_anchors = module ? module->Chapters() : std::vector<ReaderChapterAnchor>{};
    if (deps.ui.chapter_anchors.empty()) deps.ui.chapter_anchors.push_back(MakeBodyChapterAnchor());
    deps.ui.chapter_loading = loading;
    deps.ui.chapter_loading_pct = loading ? loading_pct : 100;
    deps.ui.chapter_cache_key = cache_key;
    deps.ui.chapter_sidebar_selected =
        std::clamp(deps.ui.chapter_sidebar_selected, 0,
                   std::max(0, static_cast<int>(deps.ui.chapter_anchors.size()) - 1));
    if (reset_window) deps.ui.chapter_sidebar_first_visible = 0;
  };
  auto refresh_page_chapters = [&](bool reset_window) {
    IReaderModule *module = active_module();
    const int page_count = module ? std::max(1, module->PageCount()) : 1;
    const std::string cache_key = deps.ui.current_book + "|page-list|" +
                                  std::to_string(static_cast<int>(deps.ui.mode)) + "|" +
                                  std::to_string(page_count);
    if (deps.ui.chapter_cache_key != cache_key || deps.ui.chapter_anchors.size() != static_cast<size_t>(page_count)) {
      deps.ui.chapter_anchors.clear();
      deps.ui.chapter_anchors.reserve(static_cast<size_t>(page_count));
      for (int i = 0; i < page_count; ++i) {
        ReaderChapterAnchor anchor;
        anchor.title = PageChapterTitle(i);
        anchor.page = i;
        deps.ui.chapter_anchors.push_back(std::move(anchor));
      }
      deps.ui.chapter_cache_key = cache_key;
    }
    deps.ui.chapter_loading = false;
    deps.ui.chapter_loading_pct = 100;
    deps.ui.chapter_sidebar_selected =
        std::clamp(deps.ui.chapter_sidebar_selected, 0,
                   std::max(0, static_cast<int>(deps.ui.chapter_anchors.size()) - 1));
    if (reset_window) deps.ui.chapter_sidebar_first_visible = 0;
  };
  auto jump_to_chapter = [&](const ReaderChapterAnchor &chapter) {
    if (deps.ui.mode == ReaderMode::Txt && deps.ui.Txt().open) {
      if (chapter.source_offset > 0 && deps.services.actions.jump_to_txt_source_offset) {
        deps.services.actions.jump_to_txt_source_offset(chapter.source_offset);
      } else {
        TxtReaderState &txt = deps.ui.Txt();
        const int max_scroll = std::max(0, txt.content_h - txt.viewport_h);
        txt.target_scroll_px = std::clamp(chapter.scroll_y, 0, max_scroll);
        txt.scroll_px = txt.target_scroll_px;
        txt.restore_source_offset = chapter.source_offset;
        txt.resume_cache_dirty = true;
        if (deps.services.persist_current_txt_resume_snapshot) {
          deps.services.persist_current_txt_resume_snapshot(deps.ui.current_book, true);
        }
      }
      return;
    }
    IReaderModule *module = active_module();
    if (flow_epub_open() && module) {
      module->JumpToChapter(chapter);
    } else if (page_chapters_open() && module) {
      module->SetPage(chapter.page);
    }
  };

  if (deps.ui.mode == ReaderMode::Txt && deps.ui.Txt().open) {
    refresh_txt_chapters(false);
  } else if (flow_epub_open()) {
    refresh_epub_chapters(false);
  }

  if (deps.ui.chapter_sidebar_visible) {
    if (deps.ui.mode == ReaderMode::Txt && deps.ui.Txt().open) {
      refresh_txt_chapters(false);
    } else if (flow_epub_open()) {
      refresh_epub_chapters(false);
    } else if (page_chapters_open()) {
      refresh_page_chapters(false);
    }
    ChapterSidebarInputDeps sidebar_deps{deps.input, deps.ui, jump_to_chapter};
    if (HandleChapterSidebarInput(sidebar_deps)) return;
  }

  if (deps.input.IsJustPressed(Button::Y) && deps.ui.mode == ReaderMode::Txt && deps.ui.Txt().open) {
    refresh_txt_chapters(true);
    select_current_txt_chapter();
    deps.ui.chapter_sidebar_first_visible = deps.ui.chapter_sidebar_selected;
    deps.ui.chapter_marquee_selected = -1;
    deps.ui.chapter_marquee_start_ticks = 0;
    deps.ui.chapter_sidebar_visible = true;
    deps.ui.progress_overlay_visible = false;
    return;
  }
  if (deps.input.IsJustPressed(Button::Y) && flow_epub_open()) {
    refresh_epub_chapters(true);
    select_current_epub_chapter();
    deps.ui.chapter_sidebar_first_visible = deps.ui.chapter_sidebar_selected;
    deps.ui.chapter_marquee_selected = -1;
    deps.ui.chapter_marquee_start_ticks = 0;
    deps.ui.chapter_sidebar_visible = true;
    deps.ui.progress_overlay_visible = false;
    return;
  }
  if (deps.input.IsJustPressed(Button::Y) && page_chapters_open()) {
    IReaderModule *module = active_module();
    refresh_page_chapters(true);
    deps.ui.chapter_sidebar_selected =
        module ? std::clamp(module->CurrentPage(), 0,
                            std::max(0, static_cast<int>(deps.ui.chapter_anchors.size()) - 1))
               : 0;
    deps.ui.chapter_sidebar_first_visible = deps.ui.chapter_sidebar_selected;
    deps.ui.chapter_marquee_selected = -1;
    deps.ui.chapter_marquee_start_ticks = 0;
    deps.ui.chapter_sidebar_visible = true;
    deps.ui.progress_overlay_visible = false;
    return;
  }

  if (deps.input.IsJustPressed(Button::B)) {
    ReaderCloseDeps close_deps{
        deps.ui,
        deps.progress,
        deps.reader_manager,
        &deps.pdf_runtime,
        &deps.epub_runtime,
        &deps.zip_image_runtime,
        deps.services.close_text_reader,
        deps.services.persist_current_txt_resume_snapshot,
    };
    CloseReaderSession(close_deps);
    deps.ui.chapter_sidebar_visible = false;
    deps.ui.chapter_loading = false;
    deps.ui.chapter_loading_pct = 100;
    OnReaderClosed();
    return;
  }

  ReaderInputRouterDeps reader_input_deps{
      deps.input,
      deps.ui,
      deps.pdf_runtime,
      deps.epub_runtime,
      deps.zip_image_runtime,
      deps.reader_manager,
      deps.dt,
      deps.progress_input.tap_step_px,
      deps.progress_input.overlay_tap_step_pct,
      deps.progress_input.hold_delay_sec,
      deps.progress_input.hold_speed_min_pct,
      deps.progress_input.hold_speed_max_pct,
      deps.progress_input.hold_accel_pct,
      deps.transient_message_dismissed_this_frame,
      deps.services.actions.current_progress_pct,
      deps.services.actions.jump_to_percent,
      deps.services.actions.text_scroll_by,
      deps.services.actions.text_page_by,
      deps.services.actions.show_transient_message,
  };
  HandleReaderInput(reader_input_deps);
}

void ReaderScene::OnReaderClosed() const {
  if (on_reader_closed_) on_reader_closed_();
}

void ReaderScene::Draw(const ReaderSceneRenderDeps &deps) const {
  if (!deps.renderer) return;

  const SDL_Color reader_bg =
      (deps.ui.mode == ReaderMode::Txt && deps.ui.Txt().open)
          ? deps.txt_background_color
          : SDL_Color{12, 12, 12, 255};
  if (deps.services.draw_rect) deps.services.draw_rect(0, 0, deps.screen_w, deps.screen_h, reader_bg, true);

  if (deps.ui.mode == ReaderMode::Txt && deps.ui.Txt().open) {
    TxtReaderRenderDeps txt_render_deps{
        deps.renderer,
        deps.ui,
        deps.services.clamp_text_scroll,
        deps.services.set_clip_rect,
        deps.services.clear_clip_rect,
        [&](const std::string &text, int x, int y) {
#ifdef HAVE_SDL2_TTF
          if (!deps.services.get_reader_text_texture) return;
          TextCacheEntry *te = deps.services.get_reader_text_texture(text, deps.txt_font_color);
          if (te && te->texture) {
            SDL_Rect td{x, y, te->w, te->h};
            SDL_RenderCopy(deps.renderer, te->texture, nullptr, &td);
          }
#else
          (void)text;
          (void)x;
          (void)y;
#endif
        },
    };
    DrawTxtReaderRuntime(txt_render_deps);
  } else if (deps.reader_manager) {
    IReaderModule *module = deps.reader_manager->Module(deps.ui.mode);
    if (module && module->IsOpen()) {
      module->UpdateViewport(deps.screen_w, deps.screen_h);
      module->Tick(deps.dt);
      module->Draw(deps.renderer);
    }
  }

  ReaderProgressOverlayRenderDeps progress_overlay_deps{
      deps.renderer,
      deps.progress,
      ReaderProgressOverlayLayout{
          deps.screen_w,
          deps.screen_h,
          deps.progress_overlay_metrics.panel_margin_x,
          deps.progress_overlay_metrics.panel_margin_bottom,
          deps.progress_overlay_metrics.bar_margin_x,
          deps.progress_overlay_metrics.percent_margin_x,
      },
      deps.services.scale_px,
      deps.services.draw_rect,
      deps.services.get_text_texture,
  };
  DrawReaderProgressOverlay(progress_overlay_deps);

  ChapterSidebarRenderDeps chapter_sidebar_deps{
      deps.renderer,
      deps.ui,
      deps.screen_w,
      deps.screen_h,
      deps.chapter_sidebar_w,
      deps.services.scale_px,
      deps.services.draw_rect,
      deps.services.get_text_texture,
  };
  DrawChapterSidebar(chapter_sidebar_deps);
}
