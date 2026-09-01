#pragma once

#include <cstdint>

namespace w95 {

constexpr int kTaskbarH = 28;
constexpr int kFrameB = 4;
constexpr int kTitleH = 18;
constexpr int kBtn = 16;
constexpr int kBtnH = 14;
constexpr int kIcon = 32;
constexpr int kCellW = 76;
constexpr int kCellH = 84;
constexpr int kStartW = 58;
constexpr int kClockW = 86;
constexpr int kBannerW = 22;
constexpr int kMenuItemH = 22;
constexpr int kMenuBodyW = 168;
constexpr int kDblClickMs = 400;

struct Rgb {
    uint8_t r, g, b;
};

constexpr Rgb rgb_desktop{0, 128, 128};
constexpr Rgb rgb_face{192, 192, 192};
constexpr Rgb rgb_hi{255, 255, 255};
constexpr Rgb rgb_lo{128, 128, 128};
constexpr Rgb rgb_dk{0, 0, 0};
constexpr Rgb rgb_title{0, 0, 128};
constexpr Rgb rgb_title_in{128, 128, 128};
constexpr Rgb rgb_text{0, 0, 0};
constexpr Rgb rgb_white{255, 255, 255};
constexpr Rgb rgb_red{255, 0, 0};
constexpr Rgb rgb_green{0, 140, 0};
constexpr Rgb rgb_blue{0, 0, 255};
constexpr Rgb rgb_yellow{255, 216, 0};
constexpr Rgb rgb_field{255, 255, 255};
constexpr Rgb rgb_banner{0, 0, 0};

} // namespace w95
