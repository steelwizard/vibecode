#include "wm.h"

// Chime session entry: claim the X11 display as window manager, sit in the
// event loop, then tear down. init() fails if DISPLAY is unset (typical when
// you run ./chime on the host instead of inside the TinyCore image) or if
// another WM already selected SubstructureRedirect on the root window.
int main()
{
    WM wm;
    if (!wm.init())
        return 1;
    wm.run();
    wm.finish();
    return 0;
}
