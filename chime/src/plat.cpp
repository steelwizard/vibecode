#include "wm.h"
#ifdef CHIME_WAYLAND
#include "plat_wl.h"
#endif

#include <algorithm>
#include <cstdint>
#include <cstring>

// Platform wrappers: Xlib when running as an X11 WM, software buffers +
// wlroots when running as a Wayland compositor. Drawing helpers (stroke,
// plot, …) keep draw.cpp free of raw X calls so both backends share chrome.

#ifdef CHIME_WAYLAND
static uint32_t argb_of(unsigned long pix)
{
    return 0xFF000000u | (uint32_t)(pix & 0x00FFFFFFu);
}

// Public-domain 8x8 font (Daniel Hepper, font8x8) for ASCII 32..126.
// Rows are doubled to 8x16 so captions match the old core-X "fixed" metrics.
static const unsigned char kFont8x8[95][8] = {
    {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}, {0x18, 0x3C, 0x3C, 0x18, 0x18, 0x00, 0x18, 0x00},
    {0x36, 0x36, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}, {0x36, 0x36, 0x7F, 0x36, 0x7F, 0x36, 0x36, 0x00},
    {0x0C, 0x3E, 0x03, 0x1E, 0x30, 0x1F, 0x0C, 0x00}, {0x00, 0x63, 0x33, 0x18, 0x0C, 0x66, 0x63, 0x00},
    {0x1C, 0x36, 0x1C, 0x6E, 0x3B, 0x33, 0x6E, 0x00}, {0x06, 0x06, 0x03, 0x00, 0x00, 0x00, 0x00, 0x00},
    {0x18, 0x0C, 0x06, 0x06, 0x06, 0x0C, 0x18, 0x00}, {0x06, 0x0C, 0x18, 0x18, 0x18, 0x0C, 0x06, 0x00},
    {0x00, 0x66, 0x3C, 0xFF, 0x3C, 0x66, 0x00, 0x00}, {0x00, 0x0C, 0x0C, 0x3F, 0x0C, 0x0C, 0x00, 0x00},
    {0x00, 0x00, 0x00, 0x00, 0x00, 0x0C, 0x0C, 0x06}, {0x00, 0x00, 0x00, 0x3F, 0x00, 0x00, 0x00, 0x00},
    {0x00, 0x00, 0x00, 0x00, 0x00, 0x0C, 0x0C, 0x00}, {0x60, 0x30, 0x18, 0x0C, 0x06, 0x03, 0x01, 0x00},
    {0x3E, 0x63, 0x73, 0x7B, 0x6F, 0x67, 0x3E, 0x00}, {0x0C, 0x0E, 0x0C, 0x0C, 0x0C, 0x0C, 0x3F, 0x00},
    {0x1E, 0x33, 0x30, 0x1C, 0x06, 0x33, 0x3F, 0x00}, {0x1E, 0x33, 0x30, 0x1C, 0x30, 0x33, 0x1E, 0x00},
    {0x38, 0x3C, 0x36, 0x33, 0x7F, 0x30, 0x78, 0x00}, {0x3F, 0x03, 0x1F, 0x30, 0x30, 0x33, 0x1E, 0x00},
    {0x1C, 0x06, 0x03, 0x1F, 0x33, 0x33, 0x1E, 0x00}, {0x3F, 0x33, 0x30, 0x18, 0x0C, 0x0C, 0x0C, 0x00},
    {0x1E, 0x33, 0x33, 0x1E, 0x33, 0x33, 0x1E, 0x00}, {0x1E, 0x33, 0x33, 0x3E, 0x30, 0x18, 0x0E, 0x00},
    {0x00, 0x0C, 0x0C, 0x00, 0x00, 0x0C, 0x0C, 0x00}, {0x00, 0x0C, 0x0C, 0x00, 0x00, 0x0C, 0x0C, 0x06},
    {0x18, 0x0C, 0x06, 0x03, 0x06, 0x0C, 0x18, 0x00}, {0x00, 0x00, 0x3F, 0x00, 0x00, 0x3F, 0x00, 0x00},
    {0x06, 0x0C, 0x18, 0x30, 0x18, 0x0C, 0x06, 0x00}, {0x1E, 0x33, 0x30, 0x18, 0x0C, 0x00, 0x0C, 0x00},
    {0x3E, 0x63, 0x7B, 0x7B, 0x7B, 0x03, 0x1E, 0x00}, {0x0C, 0x1E, 0x33, 0x33, 0x3F, 0x33, 0x33, 0x00},
    {0x3F, 0x66, 0x66, 0x3E, 0x66, 0x66, 0x3F, 0x00}, {0x3C, 0x66, 0x03, 0x03, 0x03, 0x66, 0x3C, 0x00},
    {0x1F, 0x36, 0x66, 0x66, 0x66, 0x36, 0x1F, 0x00}, {0x7F, 0x46, 0x16, 0x1E, 0x16, 0x46, 0x7F, 0x00},
    {0x7F, 0x46, 0x16, 0x1E, 0x16, 0x06, 0x0F, 0x00}, {0x3C, 0x66, 0x03, 0x03, 0x73, 0x66, 0x7C, 0x00},
    {0x33, 0x33, 0x33, 0x3F, 0x33, 0x33, 0x33, 0x00}, {0x1E, 0x0C, 0x0C, 0x0C, 0x0C, 0x0C, 0x1E, 0x00},
    {0x78, 0x30, 0x30, 0x30, 0x33, 0x33, 0x1E, 0x00}, {0x67, 0x66, 0x36, 0x1E, 0x36, 0x66, 0x67, 0x00},
    {0x0F, 0x06, 0x06, 0x06, 0x46, 0x66, 0x7F, 0x00}, {0x63, 0x77, 0x7F, 0x7F, 0x6B, 0x63, 0x63, 0x00},
    {0x63, 0x67, 0x6F, 0x7B, 0x73, 0x63, 0x63, 0x00}, {0x1C, 0x36, 0x63, 0x63, 0x63, 0x36, 0x1C, 0x00},
    {0x3F, 0x66, 0x66, 0x3E, 0x06, 0x06, 0x0F, 0x00}, {0x1E, 0x33, 0x33, 0x33, 0x3B, 0x1E, 0x38, 0x00},
    {0x3F, 0x66, 0x66, 0x3E, 0x36, 0x66, 0x67, 0x00}, {0x1E, 0x33, 0x07, 0x0E, 0x38, 0x33, 0x1E, 0x00},
    {0x3F, 0x2D, 0x0C, 0x0C, 0x0C, 0x0C, 0x1E, 0x00}, {0x33, 0x33, 0x33, 0x33, 0x33, 0x33, 0x3F, 0x00},
    {0x33, 0x33, 0x33, 0x33, 0x33, 0x1E, 0x0C, 0x00}, {0x63, 0x63, 0x63, 0x6B, 0x7F, 0x77, 0x63, 0x00},
    {0x63, 0x63, 0x36, 0x1C, 0x1C, 0x36, 0x63, 0x00}, {0x33, 0x33, 0x33, 0x1E, 0x0C, 0x0C, 0x1E, 0x00},
    {0x7F, 0x63, 0x31, 0x18, 0x4C, 0x66, 0x7F, 0x00}, {0x1E, 0x06, 0x06, 0x06, 0x06, 0x06, 0x1E, 0x00},
    {0x03, 0x06, 0x0C, 0x18, 0x30, 0x60, 0x40, 0x00}, {0x1E, 0x18, 0x18, 0x18, 0x18, 0x18, 0x1E, 0x00},
    {0x08, 0x1C, 0x36, 0x63, 0x00, 0x00, 0x00, 0x00}, {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xFF},
    {0x0C, 0x0C, 0x18, 0x00, 0x00, 0x00, 0x00, 0x00}, {0x00, 0x00, 0x1E, 0x30, 0x3E, 0x33, 0x6E, 0x00},
    {0x07, 0x06, 0x06, 0x3E, 0x66, 0x66, 0x3B, 0x00}, {0x00, 0x00, 0x1E, 0x33, 0x03, 0x33, 0x1E, 0x00},
    {0x38, 0x30, 0x30, 0x3e, 0x33, 0x33, 0x6E, 0x00}, {0x00, 0x00, 0x1E, 0x33, 0x3f, 0x03, 0x1E, 0x00},
    {0x1C, 0x36, 0x06, 0x0f, 0x06, 0x06, 0x0F, 0x00}, {0x00, 0x00, 0x6E, 0x33, 0x33, 0x3E, 0x30, 0x1F},
    {0x07, 0x06, 0x36, 0x6E, 0x66, 0x66, 0x67, 0x00}, {0x0C, 0x00, 0x0E, 0x0C, 0x0C, 0x0C, 0x1E, 0x00},
    {0x30, 0x00, 0x30, 0x30, 0x30, 0x33, 0x33, 0x1E}, {0x07, 0x06, 0x66, 0x36, 0x1E, 0x36, 0x67, 0x00},
    {0x0E, 0x0C, 0x0C, 0x0C, 0x0C, 0x0C, 0x1E, 0x00}, {0x00, 0x00, 0x33, 0x7F, 0x7F, 0x6B, 0x63, 0x00},
    {0x00, 0x00, 0x1F, 0x33, 0x33, 0x33, 0x33, 0x00}, {0x00, 0x00, 0x1E, 0x33, 0x33, 0x33, 0x1E, 0x00},
    {0x00, 0x00, 0x3B, 0x66, 0x66, 0x3E, 0x06, 0x0F}, {0x00, 0x00, 0x6E, 0x33, 0x33, 0x3E, 0x30, 0x78},
    {0x00, 0x00, 0x3B, 0x6E, 0x66, 0x06, 0x0F, 0x00}, {0x00, 0x00, 0x3E, 0x03, 0x1E, 0x30, 0x1F, 0x00},
    {0x08, 0x0C, 0x3E, 0x0C, 0x0C, 0x2C, 0x18, 0x00}, {0x00, 0x00, 0x33, 0x33, 0x33, 0x33, 0x6E, 0x00},
    {0x00, 0x00, 0x33, 0x33, 0x33, 0x1E, 0x0C, 0x00}, {0x00, 0x00, 0x63, 0x6B, 0x7F, 0x7F, 0x36, 0x00},
    {0x00, 0x00, 0x63, 0x36, 0x1C, 0x36, 0x63, 0x00}, {0x00, 0x00, 0x33, 0x33, 0x33, 0x3E, 0x30, 0x1F},
    {0x00, 0x00, 0x3F, 0x19, 0x0C, 0x26, 0x3F, 0x00}, {0x38, 0x0C, 0x0C, 0x07, 0x0C, 0x0C, 0x38, 0x00},
    {0x18, 0x18, 0x18, 0x00, 0x18, 0x18, 0x18, 0x00}, {0x07, 0x0C, 0x0C, 0x38, 0x0C, 0x0C, 0x07, 0x00},
    {0x6E, 0x3B, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},
};

static void sw_put(WM *wm, uint32_t *px, int dw, int dh, int x, int y, uint32_t c)
{
    if (x < 0 || y < 0 || x >= dw || y >= dh)
        return;
    if (wm->clip_on) {
        if (x < wm->clip_x || y < wm->clip_y || x >= wm->clip_x + wm->clip_w || y >= wm->clip_y + wm->clip_h)
            return;
    }
    px[y * dw + x] = c;
}

static void sw_fill(WM *wm, uint32_t *px, int dw, int dh, int x, int y, int w, int h, uint32_t c)
{
    int x0 = std::max(0, x), y0 = std::max(0, y);
    int x1 = std::min(dw, x + w), y1 = std::min(dh, y + h);
    if (wm->clip_on) {
        x0 = std::max(x0, wm->clip_x);
        y0 = std::max(y0, wm->clip_y);
        x1 = std::min(x1, wm->clip_x + wm->clip_w);
        y1 = std::min(y1, wm->clip_y + wm->clip_h);
    }
    for (int yy = y0; yy < y1; yy++)
        for (int xx = x0; xx < x1; xx++)
            px[yy * dw + xx] = c;
}

static uint32_t *lock_dst(WM *wm, Drawable d, int *w, int *h)
{
    return wl_pixels(wm, d, w, h);
}
static void unlock_dst(WM *wm, Drawable d)
{
    wl_damage(wm, d);
}
#endif

unsigned long WM::rgb_pack(std::uint8_t r, std::uint8_t g, std::uint8_t b)
{
    return ((unsigned long)r << 16) | ((unsigned long)g << 8) | (unsigned long)b;
}

void WM::win_map(Window w)
{
#ifdef CHIME_WAYLAND
    if (wayland) {
        wl_map(this, w);
        return;
    }
#endif
    if (dpy && w)
        XMapWindow(dpy, w);
}

void WM::win_unmap(Window w)
{
#ifdef CHIME_WAYLAND
    if (wayland) {
        wl_unmap(this, w);
        return;
    }
#endif
    if (dpy && w)
        XUnmapWindow(dpy, w);
}

void WM::win_destroy(Window w)
{
#ifdef CHIME_WAYLAND
    if (wayland) {
        wl_destroy(this, w);
        return;
    }
#endif
    if (dpy && w)
        XDestroyWindow(dpy, w);
}

void WM::win_raise(Window w)
{
#ifdef CHIME_WAYLAND
    if (wayland) {
        wl_raise(this, w);
        return;
    }
#endif
    if (dpy && w)
        XRaiseWindow(dpy, w);
}

void WM::win_lower(Window w)
{
#ifdef CHIME_WAYLAND
    if (wayland) {
        wl_lower(this, w);
        return;
    }
#endif
    if (dpy && w)
        XLowerWindow(dpy, w);
}

void WM::win_mapraised(Window w)
{
#ifdef CHIME_WAYLAND
    if (wayland) {
        wl_map(this, w);
        wl_raise(this, w);
        return;
    }
#endif
    if (dpy && w)
        XMapRaised(dpy, w);
}

void WM::win_move_resize(Window w, int x, int y, int wd, int ht)
{
#ifdef CHIME_WAYLAND
    if (wayland) {
        wl_move_resize(this, w, x, y, wd, ht);
        return;
    }
#endif
    if (dpy && w)
        XMoveResizeWindow(dpy, w, x, y, (unsigned)std::max(1, wd), (unsigned)std::max(1, ht));
}

void WM::win_resize(Window w, int wd, int ht)
{
#ifdef CHIME_WAYLAND
    if (wayland) {
        int dummy;
        wl_ptr_xy(this, &dummy, &dummy);
        wl_ensure_size(this, w, wd, ht);
        return;
    }
#endif
    if (dpy && w)
        XResizeWindow(dpy, w, (unsigned)std::max(1, wd), (unsigned)std::max(1, ht));
}

void WM::win_reparent(Window child, Window parent, int x, int y)
{
#ifdef CHIME_WAYLAND
    if (wayland) {
        wl_reparent(this, child, parent, x, y);
        return;
    }
#endif
    if (dpy && child)
        XReparentWindow(dpy, child, parent, x, y);
}

void WM::win_set_bg(Window w, unsigned long pix)
{
#ifdef CHIME_WAYLAND
    if (wayland) {
        wl_set_bg(this, w, pix);
        return;
    }
#endif
    if (dpy && w)
        XSetWindowBackground(dpy, w, pix);
}

void WM::win_cursor(Window w, Cursor c)
{
#ifdef CHIME_WAYLAND
    if (wayland) {
        wl_set_cursor(this, c);
        return;
    }
#endif
    if (dpy && w && c)
        XDefineCursor(dpy, w, c);
}

bool WM::ptr_xy(int *x, int *y)
{
#ifdef CHIME_WAYLAND
    if (wayland)
        return wl_ptr_xy(this, x, y);
#endif
    if (!dpy)
        return false;
    Window rr, cr;
    int wx, wy;
    unsigned mask;
    if (!XQueryPointer(dpy, root, &rr, &cr, x, y, &wx, &wy, &mask))
        return false;
    return true;
}

void WM::ptr_grab(Window w, unsigned mask, Cursor c)
{
#ifdef CHIME_WAYLAND
    if (wayland) {
        wl_ptr_grab(this, w);
        if (c)
            wl_set_cursor(this, c);
        return;
    }
#endif
    if (dpy && w)
        XGrabPointer(dpy, w, False, mask, GrabModeAsync, GrabModeAsync, None, c, CurrentTime);
}

void WM::ptr_ungrab()
{
#ifdef CHIME_WAYLAND
    if (wayland) {
        wl_ptr_ungrab(this);
        return;
    }
#endif
    if (dpy)
        XUngrabPointer(dpy, CurrentTime);
}

void WM::ptr_grab_change(unsigned mask, Cursor c)
{
#ifdef CHIME_WAYLAND
    if (wayland) {
        if (c)
            wl_set_cursor(this, c);
        return;
    }
#endif
    if (dpy)
        XChangeActivePointerGrab(dpy, mask, c, CurrentTime);
}

void WM::kbd_focus(Window w)
{
#ifdef CHIME_WAYLAND
    if (wayland) {
        kbd_grab_w = w;
        wl_kbd_grab(this, w);
        return;
    }
#endif
    if (dpy && w) {
        XSetInputFocus(dpy, w, RevertToPointerRoot, CurrentTime);
        XGrabKeyboard(dpy, w, True, GrabModeAsync, GrabModeAsync, CurrentTime);
    }
}

void WM::kbd_ungrab()
{
#ifdef CHIME_WAYLAND
    if (wayland) {
        kbd_grab_w = 0;
        wl_kbd_grab(this, 0);
        return;
    }
#endif
    if (dpy)
        XUngrabKeyboard(dpy, CurrentTime);
}

void WM::copy_area(Drawable dst, Drawable src, int sx, int sy, int w, int h, int dx, int dy)
{
#ifdef CHIME_WAYLAND
    if (wayland) {
        int sw = 0, sh = 0, dw = 0, dh = 0;
        uint32_t *sp = wl_pixels(this, src, &sw, &sh);
        uint32_t *dp = wl_pixels(this, dst, &dw, &dh);
        if (!sp || !dp || w < 1 || h < 1)
            return;
        for (int y = 0; y < h; y++) {
            int syy = sy + y, dyy = dy + y;
            if (syy < 0 || syy >= sh || dyy < 0 || dyy >= dh)
                continue;
            for (int x = 0; x < w; x++) {
                int sxx = sx + x, dxx = dx + x;
                if (sxx < 0 || sxx >= sw || dxx < 0 || dxx >= dw)
                    continue;
                sw_put(this, dp, dw, dh, dxx, dyy, sp[syy * sw + sxx]);
            }
        }
        wl_damage(this, dst);
        return;
    }
#endif
    if (dpy && dst && src && w > 0 && h > 0)
        XCopyArea(dpy, src, dst, gc, sx, sy, (unsigned)w, (unsigned)h, dx, dy);
}

Pixmap WM::pix_create(int w, int h)
{
#ifdef CHIME_WAYLAND
    if (wayland)
        return wl_pix_create(this, std::max(1, w), std::max(1, h));
#endif
    if (!dpy)
        return 0;
    return XCreatePixmap(dpy, root, (unsigned)std::max(1, w), (unsigned)std::max(1, h), (unsigned)depth);
}

void WM::pix_free(Pixmap p)
{
#ifdef CHIME_WAYLAND
    if (wayland) {
        wl_pix_free(this, p);
        return;
    }
#endif
    if (dpy && p)
        XFreePixmap(dpy, p);
}

void WM::stroke(Drawable d, int x0, int y0, int x1, int y1, unsigned long p)
{
#ifdef CHIME_WAYLAND
    if (wayland) {
        int dw = 0, dh = 0;
        uint32_t *px = lock_dst(this, d, &dw, &dh);
        if (!px)
            return;
        uint32_t c = argb_of(p);
        int dx = abs(x1 - x0), sx = x0 < x1 ? 1 : -1;
        int dy = -abs(y1 - y0), sy = y0 < y1 ? 1 : -1;
        int err = dx + dy;
        for (;;) {
            sw_put(this, px, dw, dh, x0, y0, c);
            if (x0 == x1 && y0 == y1)
                break;
            int e2 = 2 * err;
            if (e2 >= dy) {
                err += dy;
                x0 += sx;
            }
            if (e2 <= dx) {
                err += dx;
                y0 += sy;
            }
        }
        unlock_dst(this, d);
        return;
    }
#endif
    if (dpy && d) {
        XSetForeground(dpy, gc, p);
        XDrawLine(dpy, d, gc, x0, y0, x1, y1);
    }
}

void WM::stroke_rect(Drawable d, int x, int y, int w, int h, unsigned long p)
{
    if (w < 1 || h < 1)
        return;
    stroke(d, x, y, x + w - 1, y, p);
    stroke(d, x, y, x, y + h - 1, p);
    stroke(d, x, y + h - 1, x + w - 1, y + h - 1, p);
    stroke(d, x + w - 1, y, x + w - 1, y + h - 1, p);
}

void WM::plot(Drawable d, int x, int y, unsigned long p)
{
#ifdef CHIME_WAYLAND
    if (wayland) {
        int dw = 0, dh = 0;
        uint32_t *px = lock_dst(this, d, &dw, &dh);
        if (!px)
            return;
        sw_put(this, px, dw, dh, x, y, argb_of(p));
        unlock_dst(this, d);
        return;
    }
#endif
    if (dpy && d) {
        XSetForeground(dpy, gc, p);
        XDrawPoint(dpy, d, gc, x, y);
    }
}

void WM::fill_tri(Drawable d, int x0, int y0, int x1, int y1, int x2, int y2, unsigned long p)
{
#ifdef CHIME_WAYLAND
    if (wayland) {
        int dw = 0, dh = 0;
        uint32_t *px = lock_dst(this, d, &dw, &dh);
        if (!px)
            return;
        uint32_t c = argb_of(p);
        int miny = std::max(0, std::min(y0, std::min(y1, y2)));
        int maxy = std::min(dh - 1, std::max(y0, std::max(y1, y2)));
        auto edge = [](int ax, int ay, int bx, int by, int y, int &x) -> bool {
            if (ay == by)
                return false;
            if ((y < ay && y < by) || (y > ay && y > by))
                return false;
            x = ax + (bx - ax) * (y - ay) / (by - ay);
            return true;
        };
        for (int y = miny; y <= maxy; y++) {
            int xs[3], n = 0, t;
            if (edge(x0, y0, x1, y1, y, t))
                xs[n++] = t;
            if (edge(x1, y1, x2, y2, y, t))
                xs[n++] = t;
            if (edge(x2, y2, x0, y0, y, t) && n < 3)
                xs[n++] = t;
            if (n < 2)
                continue;
            int a = xs[0], b = xs[1];
            if (a > b)
                std::swap(a, b);
            for (int x = a; x <= b; x++)
                sw_put(this, px, dw, dh, x, y, c);
        }
        unlock_dst(this, d);
        return;
    }
#endif
    if (dpy && d) {
        XPoint pts[3] = {{(short)x0, (short)y0}, {(short)x1, (short)y1}, {(short)x2, (short)y2}};
        XSetForeground(dpy, gc, p);
        XFillPolygon(dpy, d, gc, pts, 3, Convex, CoordModeOrigin);
    }
}

void WM::fill_oval(Drawable d, int x, int y, int s, unsigned long p)
{
#ifdef CHIME_WAYLAND
    if (wayland) {
        int dw = 0, dh = 0;
        uint32_t *px = lock_dst(this, d, &dw, &dh);
        if (!px || s < 1)
            return;
        uint32_t c = argb_of(p);
        int cx = x + s / 2, cy = y + s / 2, r = s / 2;
        int r2 = r * r;
        for (int yy = y; yy < y + s; yy++)
            for (int xx = x; xx < x + s; xx++) {
                int dx = xx - cx, dy = yy - cy;
                if (dx * dx + dy * dy <= r2)
                    sw_put(this, px, dw, dh, xx, yy, c);
            }
        unlock_dst(this, d);
        return;
    }
#endif
    if (dpy && d) {
        XSetForeground(dpy, gc, p);
        XFillArc(dpy, d, gc, x, y, s, s, 0, 360 * 64);
    }
}

void WM::stroke_oval(Drawable d, int x, int y, int s, unsigned long p)
{
#ifdef CHIME_WAYLAND
    if (wayland) {
        int dw = 0, dh = 0;
        uint32_t *px = lock_dst(this, d, &dw, &dh);
        if (!px || s < 2)
            return;
        uint32_t c = argb_of(p);
        int cx = x + s / 2, cy = y + s / 2, r = s / 2;
        int x0 = r, y0 = 0, err = 0;
        while (x0 >= y0) {
            sw_put(this, px, dw, dh, cx + x0, cy + y0, c);
            sw_put(this, px, dw, dh, cx + y0, cy + x0, c);
            sw_put(this, px, dw, dh, cx - y0, cy + x0, c);
            sw_put(this, px, dw, dh, cx - x0, cy + y0, c);
            sw_put(this, px, dw, dh, cx - x0, cy - y0, c);
            sw_put(this, px, dw, dh, cx - y0, cy - x0, c);
            sw_put(this, px, dw, dh, cx + y0, cy - x0, c);
            sw_put(this, px, dw, dh, cx + x0, cy - y0, c);
            if (err <= 0) {
                y0++;
                err += 2 * y0 + 1;
            }
            if (err > 0) {
                x0--;
                err -= 2 * x0 + 1;
            }
        }
        unlock_dst(this, d);
        return;
    }
#endif
    if (dpy && d) {
        XSetForeground(dpy, gc, p);
        XDrawArc(dpy, d, gc, x, y, s, s, 0, 360 * 64);
    }
}

void WM::clip_set(int x, int y, int w, int h)
{
    clip_on = true;
    clip_x = x;
    clip_y = y;
    clip_w = w;
    clip_h = h;
#ifndef CHIME_WAYLAND
    (void)0;
#endif
    if (!wayland && dpy && gc) {
        XRectangle r;
        r.x = (short)x;
        r.y = (short)y;
        r.width = (unsigned short)std::max(0, w);
        r.height = (unsigned short)std::max(0, h);
        XSetClipRectangles(dpy, gc, 0, 0, &r, 1, Unsorted);
    }
}

void WM::clip_clear()
{
    clip_on = false;
    if (!wayland && dpy && gc)
        XSetClipMask(dpy, gc, None);
}

bool WM::win_geom(Window w, int *wd, int *ht, bool *mapped)
{
#ifdef CHIME_WAYLAND
    if (wayland) {
        int ww = wl_win_w(this, w);
        int hh = wl_win_h(this, w);
        if (wd)
            *wd = ww;
        if (ht)
            *ht = hh;
        if (mapped)
            *mapped = wl_win_mapped(this, w);
        return ww > 0 && hh > 0;
    }
#endif
    if (!dpy || !w)
        return false;
    XWindowAttributes wa{};
    if (!XGetWindowAttributes(dpy, w, &wa))
        return false;
    if (wd)
        *wd = wa.width;
    if (ht)
        *ht = wa.height;
    if (mapped)
        *mapped = wa.map_state != IsUnmapped;
    return true;
}

bool WM::win_root_xy(Window w, int *x, int *y)
{
#ifdef CHIME_WAYLAND
    if (wayland)
        return wl_win_xy(this, w, x, y);
#endif
    if (!dpy || !w)
        return false;
    Window child = 0;
    int rx = 0, ry = 0;
    if (!XTranslateCoordinates(dpy, w, root, 0, 0, &rx, &ry, &child))
        return false;
    if (x)
        *x = rx;
    if (y)
        *y = ry;
    return true;
}

void WM::allow_replay()
{
#ifdef CHIME_WAYLAND
    if (wayland)
        return;
#endif
    if (dpy)
        XAllowEvents(dpy, ReplayPointer, CurrentTime);
}

void WM::backend_flush()
{
#ifdef CHIME_WAYLAND
    if (wayland) {
        wl_backend_flush(this);
        return;
    }
#endif
    if (dpy)
        XFlush(dpy);
}

int WM::backend_fd()
{
#ifdef CHIME_WAYLAND
    if (wayland)
        return wl_backend_fd(this);
#endif
    return dpy ? ConnectionNumber(dpy) : -1;
}

void WM::backend_drain()
{
#ifdef CHIME_WAYLAND
    if (wayland) {
        wl_backend_drain(this);
        return;
    }
#endif
    if (!dpy)
        return;
    while (XPending(dpy)) {
        XEvent e;
        XNextEvent(dpy, &e);
        handle_event(&e);
    }
}

void WM::soft_fill(Drawable d, int x, int y, int w, int h, unsigned long p)
{
#ifdef CHIME_WAYLAND
    if (!wayland)
        return;
    int dw = 0, dh = 0;
    uint32_t *px = lock_dst(this, d, &dw, &dh);
    if (!px || w < 1 || h < 1)
        return;
    sw_fill(this, px, dw, dh, x, y, w, h, argb_of(p));
    unlock_dst(this, d);
#else
    (void)d;
    (void)x;
    (void)y;
    (void)w;
    (void)h;
    (void)p;
#endif
}

int WM::soft_text_w(const char *s, bool bold)
{
    if (!s)
        return 0;
    int n = (int)strlen(s);
    return n * (bold ? 9 : 8);
}

void WM::soft_text(Drawable d, int x, int y, int box_h, const char *s, unsigned long p, bool bold)
{
#ifdef CHIME_WAYLAND
    if (!wayland || !s)
        return;
    int dw = 0, dh = 0;
    uint32_t *px = lock_dst(this, d, &dw, &dh);
    if (!px)
        return;
    uint32_t c = argb_of(p);
    int th = 16;
    int ty = y + std::max(0, (box_h - th) / 2);
    int cx = x;
    for (const unsigned char *q = (const unsigned char *)s; *q; q++) {
        unsigned char ch = *q;
        if (ch < 32 || ch > 126)
            ch = '?';
        const unsigned char *g = kFont8x8[ch - 32];
        for (int row = 0; row < 8; row++) {
            unsigned char bits = g[row];
            for (int col = 0; col < 8; col++) {
                if (bits & (1u << col)) {
                    sw_put(this, px, dw, dh, cx + col, ty + row * 2, c);
                    sw_put(this, px, dw, dh, cx + col, ty + row * 2 + 1, c);
                    if (bold) {
                        sw_put(this, px, dw, dh, cx + col + 1, ty + row * 2, c);
                        sw_put(this, px, dw, dh, cx + col + 1, ty + row * 2 + 1, c);
                    }
                }
            }
        }
        cx += bold ? 9 : 8;
    }
    unlock_dst(this, d);
#else
    (void)d;
    (void)x;
    (void)y;
    (void)box_h;
    (void)s;
    (void)p;
    (void)bold;
#endif
}