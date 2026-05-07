#include "app_layout.h"

#include <algorithm>
#include <cmath>

namespace {

constexpr LayoutMetrics layout_720x480{
    720, 480, 20,
    0, 30,
    30, 50,
    80, 350,
    430, 50,
    140, 210,
    180, 250,
    33, 38,
    33, 100,
    36, 2, 4, 24,
    240, 0, 35,
    32, 20,
    21, 46,
    667, 46,
    90, 135, 42,
    18, 12, 34, 34,
    4, 2,
    1.0f,
};

constexpr LayoutMetrics layout_640x480{
    640, 480, 16,
    0, 30,
    30, 50,
    80, 350,
    430, 50,
    130, 195,
    167, 232,
    25, 30,
    23, 100,
    36, 2, 4, 24,
    160, 0, 35,
    28, 20,
    14, 46,
    587, 46,
    72, 124, 42,
    18, 12, 34, 34,
    4, 2,
    1.0f,
};

constexpr LayoutMetrics layout_960x720{
    960, 720, 27,
    0, 36,
    36, 58,
    104, 556,
    660, 60,
    160, 210,
    204, 250,
    42, 38,
    47, 100,
    36, 2, 4, 24,
    320, 0, 42,
    43, 26,
    28, 54,
    889, 54,
    120, 180, 50,
    24, 16, 45, 45,
    4, 3,
    1.0f,
};

constexpr LayoutMetrics layout_720x720{
    720, 720, 20,
    0, 36,
    36, 58,
    104, 556,
    660, 60,
    140, 210,
    180, 250,
    33, 38,
    33, 100,
    36, 2, 4, 24,
    240, 0, 42,
    32, 26,
    21, 54,
    667, 54,
    90, 135, 50,
    18, 16, 34, 34,
    4, 3,
    1.0f,
};

constexpr LayoutMetrics layout_1024x768{
    1024, 768, 26,
    0, 48,
    48, 80,
    128, 560,
    688, 80,
    208, 312,
    267, 371,
    40, 48,
    37, 160,
    58, 3, 6, 38,
    256, 0, 56,
    45, 32,
    22, 74,
    939, 74,
    115, 198, 67,
    29, 19, 54, 54,
    4, 2,
    1.6f,
};

constexpr float kFocusScaleCurrent = 1.045f;
constexpr float kFocusScale = kFocusScaleCurrent;

const LayoutMetrics *g_layout = &layout_720x480;

}  // namespace

const LayoutMetrics &Layout() { return *g_layout; }

const LayoutMetrics &SelectLayoutProfile(int screen_w, int screen_h) {
  if (screen_w == layout_1024x768.screen_w && screen_h == layout_1024x768.screen_h) return layout_1024x768;
  if (screen_w == layout_960x720.screen_w && screen_h == layout_960x720.screen_h) return layout_960x720;
  if (screen_w == layout_720x720.screen_w && screen_h == layout_720x720.screen_h) return layout_720x720;
  if (screen_w == layout_640x480.screen_w && screen_h == layout_640x480.screen_h) return layout_640x480;
  return layout_720x480;
}

void SetLayoutProfile(const LayoutMetrics &layout) {
  g_layout = &layout;
}

int FocusedCoverW() { return static_cast<int>(Layout().cover_w * kFocusScale + 0.5f); }
int FocusedCoverH() { return static_cast<int>(Layout().cover_h * kFocusScale + 0.5f); }

int ScalePx(int value) {
  return std::max(1, static_cast<int>(std::round(static_cast<float>(value) * Layout().ui_scale)));
}

float ScaleFloat(float value) { return value * Layout().ui_scale; }
int ShelfGridCols() { return std::max(1, Layout().grid_cols); }
int ShelfVisibleRows() { return std::max(1, Layout().visible_rows); }
int ShelfItemsPerPage() { return ShelfGridCols() * ShelfVisibleRows(); }
