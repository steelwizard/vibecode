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
constexpr int kTrayIcon = 20;
constexpr int kTraySlot = 22;
constexpr int kBannerW = 22;
constexpr int kMenuItemH = 22;
constexpr int kMenuBodyW = 168;
constexpr int kDblClickMs = 400;
constexpr int kSnapEdge = 24;
constexpr int kRunW = 372;
constexpr int kRunH = 176;
constexpr int kDlgBtnW = 75;
constexpr int kDlgBtnH = 23;
constexpr int kSetW = 448;
constexpr int kSetH = 372;
constexpr int kListRow = 16;

struct Rgb {
    uint8_t r, g, b;
};

struct Scheme {
    const char *name;
    Rgb desktop, face, hi, lo, dk, title, title_in, text, field, banner;
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

constexpr Scheme kSchemes[] = {
    {"Chicago", {0, 128, 128}, {192, 192, 192}, {255, 255, 255}, {128, 128, 128}, {0, 0, 0}, {0, 0, 128}, {128, 128, 128}, {0, 0, 0}, {255, 255, 255}, {0, 0, 0}},
    {"High Contrast", {0, 0, 0}, {64, 64, 64}, {255, 255, 255}, {32, 32, 32}, {0, 0, 0}, {255, 255, 0}, {80, 80, 80}, {255, 255, 255}, {0, 0, 0}, {0, 0, 0}},
    {"Paper", {192, 192, 192}, {255, 255, 255}, {255, 255, 255}, {128, 128, 128}, {0, 0, 0}, {0, 0, 0}, {160, 160, 160}, {0, 0, 0}, {255, 255, 255}, {0, 0, 0}},
    {"Desert", {200, 168, 112}, {212, 200, 176}, {255, 248, 224}, {148, 120, 80}, {64, 40, 16}, {128, 64, 0}, {176, 144, 96}, {48, 24, 0}, {255, 252, 240}, {80, 40, 0}},
    {"Rose", {176, 112, 128}, {216, 192, 200}, {255, 240, 244}, {144, 96, 112}, {64, 16, 32}, {128, 0, 64}, {160, 96, 112}, {48, 0, 24}, {255, 248, 252}, {64, 0, 32}},
    {"Storm", {64, 80, 104}, {176, 184, 192}, {232, 240, 248}, {96, 104, 120}, {16, 16, 32}, {0, 32, 96}, {80, 88, 104}, {0, 0, 0}, {240, 248, 255}, {0, 0, 32}},
    {"Maple", {160, 88, 32}, {204, 176, 136}, {255, 232, 192}, {128, 80, 40}, {48, 16, 0}, {112, 32, 0}, {160, 96, 48}, {32, 8, 0}, {255, 248, 224}, {48, 16, 0}},
    {"Eggplant", {80, 48, 88}, {176, 160, 184}, {232, 216, 240}, {96, 72, 104}, {32, 0, 40}, {80, 0, 80}, {112, 80, 120}, {255, 240, 255}, {248, 236, 255}, {32, 0, 40}},
    {"Spruce", {0, 80, 64}, {176, 192, 176}, {224, 255, 224}, {64, 96, 72}, {0, 24, 16}, {0, 80, 48}, {64, 112, 80}, {0, 16, 8}, {240, 255, 240}, {0, 24, 16}},
    {"Midnight", {16, 20, 48}, {56, 60, 80}, {120, 128, 160}, {32, 36, 56}, {0, 0, 0}, {72, 80, 160}, {40, 44, 64}, {220, 224, 255}, {24, 28, 48}, {8, 8, 24}},
};

constexpr int kSchemeN = (int)(sizeof(kSchemes) / sizeof(kSchemes[0]));

} // namespace w95
