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
        std::fprintf(stderr, "chime: not built with Wayland; using X11\n");
        wm.wayland = false;
    }
#endif
    if (!wm.init())
        return 1;
    wm.run();
    wm.finish();
    return 0;
}
