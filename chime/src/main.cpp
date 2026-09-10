#include "wm.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

// Pick X11 WM vs Wayland compositor:
//   CHIME_BACKEND=x11|wayland overrides everything
//   else WAYLAND_DISPLAY → nest as a compositor
//   else DISPLAY → classic X11 window manager
//   else DRM/TTY compositor
static bool want_wayland()
{
    const char *be = getenv("CHIME_BACKEND");
    if (be && !std::strcmp(be, "wayland"))
        return true;
    if (be && !std::strcmp(be, "x11"))
        return false;
    if (getenv("WAYLAND_DISPLAY"))
        return true;
    if (getenv("DISPLAY"))
        return false;
    return true;
}

int main()
{
    WM wm;
    wm.wayland = want_wayland();
#ifndef CHIME_WAYLAND
    if (wm.wayland) {
        // Login sessions unset DISPLAY. Falling back to X11 here exits
        // immediately and the display manager drops to a TTY.
        std::fprintf(stderr, "chime: not built with Wayland support (rebuild with make CHIME_WL=1)\n");
        return 1;
    }
#endif
    if (!wm.init())
        return 1;
    wm.run();
    wm.finish();
    return 0;
}
