#pragma once

#include <cstdint>

// Win95-style metrics and palettes shared by the window manager and Cabinet.
// Sizes are in pixels and were chosen to look right at 640x480–1080p with the
// bitmap Helvetica/fixed fonts Chime loads, not to match Microsoft's original
// dialog units exactly.

namespace w95 {

// --- Chrome metrics ----------------------------------------------------------

constexpr int kTaskbarH = 28;   // Height of the bar along the bottom of each monitor
constexpr int kFrameB = 4;      // Outer bevel thickness around a managed window
constexpr int kTitleH = 18;     // Caption bar inside the frame (above the client)
constexpr int kBtn = 16;        // Caption button width (min / max / close)
constexpr int kBtnH = 14;       // Caption button height
constexpr int kIcon = 32;       // Desktop shortcut glyph size
constexpr int kCellW = 76;      // Hit box around a desktop icon (width)
constexpr int kCellH = 84;      // Hit box around a desktop icon (height, includes label)
constexpr int kStartW = 54;     // Start button width on the taskbar (label only)
constexpr int kClockW = 86;     // Clock well width (also used when no tray icons)
constexpr int kTrayIcon = 20;   // Size we force on each embedded tray client
constexpr int kTraySlot = 22;   // Horizontal stride between tray icons (includes pad)
constexpr int kBannerW = 22;    // Dark vertical strip on the left of the Start menu
constexpr int kMenuItemH = 22;  // One Start-menu / submenu row
constexpr int kMenuBodyW = 168; // Start menu body to the right of the banner
constexpr int kDblClickMs = 400; // Title / desktop-icon double-click window
constexpr int kSnapEdge = 24;   // Pointer proximity (px) that triggers Aero-style snap
constexpr int kRunW = 372;      // Run dialog width
constexpr int kRunH = 176;      // Run dialog height
constexpr int kDlgBtnW = 75;    // Standard OK / Cancel / Apply button
constexpr int kDlgBtnH = 23;
constexpr int kSetW = 460;      // Display Properties dialog
constexpr int kSetH = 408;
constexpr int kListRow = 16;
constexpr int kColorW = 420;    // Color scheme editor
constexpr int kColorH = 356;
constexpr int kFileW = 408;     // Wallpaper file picker
constexpr int kFileH = 320;
constexpr int kColorRoleN = 10;

// Display Properties hit boxes — keep in sync with draw_setdlg / on_button_press.
constexpr int kWallListX = 232, kWallListY = 42, kWallListW = 212, kWallListH = 88;
constexpr int kBrowseX = 232, kBrowseY = 134, kBrowseW = 88;
constexpr int kSchemeListX = 16, kSchemeListY = 176, kSchemeListW = 318, kSchemeListH = 88;
constexpr int kSchemeBtnX = 348, kSchemeBtnY = 176;
constexpr int kResListX = 16, kResListY = 282, kResListW = 220, kResListH = 64;

// 8-bit RGB triple. Converted to an X pixel with XAllocColor (8-bit -> 16-bit
// by multiplying by 257 so 0x80 becomes 0x8080, not 0x8000).
struct Rgb {
    uint8_t r, g, b;
};

// Named color scheme. `text` is window/menu foreground; `field` is the white
// of text boxes and listboxes; `banner` is the Start-menu spine.
struct Scheme {
    const char *name;
    Rgb desktop, face, hi, lo, dk, title, title_in, text, field, banner;
};

// Default Chicago (Windows 95) palette. `hi`/`lo`/`dk` are the three bevel
// stops: white highlight, gray shadow, black outer shadow.
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

// Built-in schemes shown in Display Properties. Index 0 is the factory default
// and the name written to ~/.chime/display when nothing has been saved yet.
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
