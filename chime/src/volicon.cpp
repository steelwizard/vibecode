// volicon — 20x20 speaker glyph that docks in Chime's system tray.
// Left-click mutes; wheel steps volume 5%; right-click opens alsamixer.
// Volume is applied with amixer (Master, then PCM). We wait up to 8s for
// the WM to own _NET_SYSTEM_TRAY_S<screen> before docking.

#include <X11/Xlib.h>
#include <X11/Xatom.h>
#include <X11/Xutil.h>

#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unistd.h>

namespace {

constexpr int kSz = 20;
Display *dpy;
Window icon;
GC gc;
unsigned long bg, fg, mute_col, hi;
int vol = 80;
bool muted = false;

unsigned long rgb(int r, int g, int b)
{
    XColor xc{};
    xc.red = (unsigned short)(r * 257);
    xc.green = (unsigned short)(g * 257);
    xc.blue = (unsigned short)(b * 257);
    xc.flags = DoRed | DoGreen | DoBlue;
    XAllocColor(dpy, DefaultColormap(dpy, DefaultScreen(dpy)), &xc);
    return xc.pixel;
}

void set_vol(int v, bool mute)
{
    if (v < 0)
        v = 0;
    if (v > 100)
        v = 100;
    vol = v;
    muted = mute;
    char cmd[192];
    std::snprintf(cmd, sizeof(cmd),
                  "amixer -q sset Master %d%% %s 2>/dev/null || "
                  "amixer -q sset PCM %d%% %s 2>/dev/null || true",
                  vol, muted ? "mute" : "unmute", vol, muted ? "mute" : "unmute");
    if (fork() == 0) {
        setsid();
        execl("/bin/sh", "sh", "-c", cmd, (char *)nullptr);
        _exit(127);
    }
}

void draw()
{
    XSetForeground(dpy, gc, bg);
    XFillRectangle(dpy, icon, gc, 0, 0, kSz, kSz);
    int x = 2, y = 6;
    XSetForeground(dpy, gc, muted ? mute_col : fg);
    XFillRectangle(dpy, icon, gc, x, y + 3, 4, 6);
    XPoint sp[3] = {{(short)(x + 4), (short)(y + 3)}, {(short)(x + 9), (short)y}, {(short)(x + 9), (short)(y + 12)}};
    XFillPolygon(dpy, icon, gc, sp, 3, Convex, CoordModeOrigin);
    if (!muted) {
        // 0–2 sound-wave arcs depending on volume band.
        XSetForeground(dpy, gc, hi);
        int n = vol < 35 ? 0 : (vol < 70 ? 1 : 2);
        for (int i = 0; i < n; i++) {
            int cx = x + 12 + i * 3;
            XDrawArc(dpy, icon, gc, cx - 6, y - 1, 10 + i * 4, 14, 320 * 64, 80 * 64);
        }
    } else {
        XSetForeground(dpy, gc, mute_col);
        XDrawLine(dpy, icon, gc, 3, 3, kSz - 4, kSz - 4);
        XDrawLine(dpy, icon, gc, kSz - 4, 3, 3, kSz - 4);
    }
}

Window tray_owner()
{
    char name[32];
    std::snprintf(name, sizeof(name), "_NET_SYSTEM_TRAY_S%d", DefaultScreen(dpy));
    Atom sel = XInternAtom(dpy, name, False);
    return XGetSelectionOwner(dpy, sel);
}

// SYSTEM_TRAY_REQUEST_DOCK (opcode 0): ask the tray to reparent `icon`.
void dock(Window owner)
{
    Atom opcode = XInternAtom(dpy, "_NET_SYSTEM_TRAY_OPCODE", False);
    XEvent ev{};
    ev.xclient.type = ClientMessage;
    ev.xclient.window = owner;
    ev.xclient.message_type = opcode;
    ev.xclient.format = 32;
    ev.xclient.data.l[0] = CurrentTime;
    ev.xclient.data.l[1] = 0;
    ev.xclient.data.l[2] = (long)icon;
    XSendEvent(dpy, owner, False, NoEventMask, &ev);
    XSync(dpy, False);
}

} // namespace

int main()
{
    dpy = XOpenDisplay(nullptr);
    if (!dpy)
        return 1;
    signal(SIGCHLD, SIG_IGN);
    int scr = DefaultScreen(dpy);
    bg = rgb(192, 192, 192);
    fg = rgb(0, 0, 0);
    hi = rgb(0, 128, 0);
    mute_col = rgb(160, 0, 0);
    XSetWindowAttributes swa{};
    swa.background_pixel = bg;
    swa.event_mask = ExposureMask | ButtonPressMask | StructureNotifyMask;
    icon = XCreateWindow(dpy, RootWindow(dpy, scr), 0, 0, kSz, kSz, 0, CopyFromParent, InputOutput, CopyFromParent,
                         CWBackPixel | CWEventMask, &swa);
    gc = XCreateGC(dpy, icon, 0, nullptr);
    XSizeHints sh{};
    sh.flags = PSize | PMinSize | PMaxSize;
    sh.width = sh.height = sh.min_width = sh.min_height = sh.max_width = sh.max_height = kSz;
    XSetWMNormalHints(dpy, icon, &sh);
    XStoreName(dpy, icon, "Volume");

    Window owner = 0;
    for (int i = 0; i < 80 && !owner; i++) {
        owner = tray_owner();
        if (!owner)
            usleep(100000);
    }
    if (!owner)
        return 1;
    dock(owner);
    XMapWindow(dpy, icon);
    set_vol(vol, false);

    Atom xembed = XInternAtom(dpy, "_XEMBED", False);
    for (;;) {
        XEvent e;
        XNextEvent(dpy, &e);
        if (e.type == Expose && e.xexpose.count == 0)
            draw();
        else if (e.type == ClientMessage && e.xclient.message_type == xembed)
            draw();
        else if (e.type == ButtonPress) {
            if (e.xbutton.button == 1) {
                set_vol(vol, !muted);
                draw();
            } else if (e.xbutton.button == 3) {
                if (fork() == 0) {
                    setsid();
                    execl("/bin/sh", "sh", "-c",
                          "alsamixergui >/dev/null 2>&1 || aterm -e alsamixer || xterm -e alsamixer || true",
                          (char *)nullptr);
                    _exit(127);
                }
            } else if (e.xbutton.button == 4) {
                set_vol(vol + 5, false);
                draw();
            } else if (e.xbutton.button == 5) {
                set_vol(vol - 5, false);
                draw();
            }
        }
    }
}
