#include "wm.h"

#include <cstdio>
#include <cstring>
#include <ctime>
#include <algorithm>

void WM::fill(Drawable d, int x, int y, int w, int h, unsigned long p)
{
    if (w <= 0 || h <= 0)
        return;
    XSetForeground(dpy, gc, p);
    XFillRectangle(dpy, d, gc, x, y, w, h);
}

void WM::bevel(Drawable d, int x, int y, int w, int h, bool raised)
{
    if (w <= 0 || h <= 0)
        return;
    unsigned long tl = raised ? hi.pix : lo.pix;
    unsigned long br = raised ? dk.pix : hi.pix;
    unsigned long tl2 = raised ? face.pix : dk.pix;
    unsigned long br2 = raised ? lo.pix : face.pix;
    XSetForeground(dpy, gc, tl);
    XDrawLine(dpy, d, gc, x, y, x + w - 1, y);
    XDrawLine(dpy, d, gc, x, y, x, y + h - 1);
    XSetForeground(dpy, gc, br);
    XDrawLine(dpy, d, gc, x, y + h - 1, x + w - 1, y + h - 1);
    XDrawLine(dpy, d, gc, x + w - 1, y, x + w - 1, y + h - 1);
    if (w < 4 || h < 4)
        return;
    XSetForeground(dpy, gc, tl2);
    XDrawLine(dpy, d, gc, x + 1, y + 1, x + w - 3, y + 1);
    XDrawLine(dpy, d, gc, x + 1, y + 1, x + 1, y + h - 3);
    XSetForeground(dpy, gc, br2);
    XDrawLine(dpy, d, gc, x + 1, y + h - 2, x + w - 2, y + h - 2);
    XDrawLine(dpy, d, gc, x + w - 2, y + 1, x + w - 2, y + h - 2);
}

void WM::sunken(Drawable d, int x, int y, int w, int h)
{
    bevel(d, x, y, w, h, false);
}

int WM::text_w(const char *s, bool bold)
{
    if (!s)
        return 0;
    XFontStruct *f = (bold && font_b) ? font_b : font;
    if (!f)
        return (int)strlen(s) * 8;
    return XTextWidth(f, s, (int)strlen(s));
}

void WM::draw_str(Drawable d, int x, int y, int box_h, const char *s, unsigned long pix, bool bold)
{
    if (!s)
        return;
    XFontStruct *f = (bold && font_b) ? font_b : font;
    if (!f)
        return;
    int th = f->ascent + f->descent;
    int ty = y + f->ascent + std::max(0, (box_h - th) / 2);
    XSetFont(dpy, gc, f->fid);
    XSetForeground(dpy, gc, pix);
    XDrawString(dpy, d, gc, x, ty, s, (int)strlen(s));
}

void WM::draw_str_clip(Drawable d, int x, int y, int w, int h, const char *s, unsigned long pix, bool bold)
{
    if (w <= 0 || h <= 0 || !s)
        return;
    XRectangle r;
    r.x = (short)x;
    r.y = (short)y;
    r.width = (unsigned short)w;
    r.height = (unsigned short)h;
    XSetClipRectangles(dpy, gc, 0, 0, &r, 1, Unsorted);
    draw_str(d, x, y, h, s, pix, bold);
    XSetClipMask(dpy, gc, None);
}

void WM::draw_flag(Drawable d, int x, int y)
{
    fill(d, x + 1, y + 1, 6, 5, red.pix);
    fill(d, x + 8, y + 2, 6, 5, green.pix);
    fill(d, x + 1, y + 7, 6, 5, blue.pix);
    fill(d, x + 8, y + 8, 6, 5, yellow.pix);
}

void WM::draw_caption_btn(Drawable d, int x, int y, int kind, bool maxed)
{
    fill(d, x, y, w95::kBtn, w95::kBtnH, face.pix);
    bevel(d, x, y, w95::kBtn, w95::kBtnH, true);
    XSetForeground(dpy, gc, fg.pix);
    if (kind == 0) {
        XDrawLine(dpy, d, gc, x + 3, y + w95::kBtnH - 5, x + w95::kBtn - 4, y + w95::kBtnH - 5);
        XDrawLine(dpy, d, gc, x + 3, y + w95::kBtnH - 4, x + w95::kBtn - 4, y + w95::kBtnH - 4);
    } else if (kind == 1) {
        if (maxed) {
            XDrawRectangle(dpy, d, gc, x + 5, y + 3, 6, 5);
            XDrawRectangle(dpy, d, gc, x + 3, y + 5, 6, 5);
        } else {
            XDrawRectangle(dpy, d, gc, x + 3, y + 3, w95::kBtn - 7, w95::kBtnH - 7);
            XDrawLine(dpy, d, gc, x + 3, y + 4, x + w95::kBtn - 5, y + 4);
        }
    } else {
        XDrawLine(dpy, d, gc, x + 4, y + 3, x + w95::kBtn - 5, y + w95::kBtnH - 4);
        XDrawLine(dpy, d, gc, x + 5, y + 3, x + w95::kBtn - 4, y + w95::kBtnH - 4);
        XDrawLine(dpy, d, gc, x + w95::kBtn - 5, y + 3, x + 4, y + w95::kBtnH - 4);
        XDrawLine(dpy, d, gc, x + w95::kBtn - 4, y + 3, x + 5, y + w95::kBtnH - 4);
    }
}

void WM::draw_icon(Drawable d, int x, int y, int kind, bool selected)
{
    if (selected)
        fill(d, x - 4, y - 2, w95::kIcon + 8, w95::kIcon + 6, title.pix);

    if (kind == 0) {
        fill(d, x + 4, y + 2, 24, 18, lo.pix);
        fill(d, x + 6, y + 4, 20, 13, title.pix);
        fill(d, x + 8, y + 6, 6, 4, white.pix);
        fill(d, x + 10, y + 20, 12, 3, face.pix);
        fill(d, x + 6, y + 23, 20, 4, lo.pix);
        fill(d, x + 22, y + 14, 6, 10, yellow.pix);
    } else if (kind == 1) {
        fill(d, x + 4, y + 8, 24, 18, yellow.pix);
        fill(d, x + 6, y + 4, 12, 6, yellow.pix);
        XSetForeground(dpy, gc, lo.pix);
        XDrawRectangle(dpy, d, gc, x + 4, y + 8, 24, 18);
    } else if (kind == 2) {
        fill(d, x + 2, y + 4, 28, 22, lo.pix);
        fill(d, x + 4, y + 6, 24, 16, dk.pix);
        XSetForeground(dpy, gc, green.pix);
        XDrawLine(dpy, d, gc, x + 7, y + 10, x + 18, y + 10);
        XDrawLine(dpy, d, gc, x + 7, y + 13, x + 22, y + 13);
        XDrawLine(dpy, d, gc, x + 7, y + 16, x + 14, y + 16);
        fill(d, x + 12, y + 22, 8, 4, face.pix);
        fill(d, x + 6, y + 26, 20, 3, lo.pix);
    } else {
        fill(d, x + 6, y + 2, 20, 26, white.pix);
        XSetForeground(dpy, gc, lo.pix);
        XDrawRectangle(dpy, d, gc, x + 6, y + 2, 20, 26);
        fill(d, x + 8, y + 4, 16, 4, title.pix);
        XSetForeground(dpy, gc, dk.pix);
        XDrawLine(dpy, d, gc, x + 9, y + 12, x + 23, y + 12);
        XDrawLine(dpy, d, gc, x + 9, y + 16, x + 21, y + 16);
        XDrawLine(dpy, d, gc, x + 9, y + 20, x + 23, y + 20);
        XDrawLine(dpy, d, gc, x + 9, y + 24, x + 18, y + 24);
    }
}

void WM::draw_frame(Client *c)
{
    const int B = w95::kFrameB;
    const int T = w95::kTitleH;
    fill(c->frame, 0, 0, c->w, c->h, face.pix);
    bevel(c->frame, 0, 0, c->w, c->h, true);
    bool act = (focused == c && !c->iconic);
    fill(c->frame, B, B, c->w - 2 * B, T, act ? title.pix : title_in.pix);
    fill(c->frame, B + 2, B + 2, 14, T - 4, face.pix);
    bevel(c->frame, B + 2, B + 2, 14, T - 4, true);
    fill(c->frame, B + 5, B + 5, 8, 5, act ? title.pix : title_in.pix);
    int bx = c->w - B - 2 - w95::kBtn;
    int by = B + (T - w95::kBtnH) / 2;
    draw_caption_btn(c->frame, bx, by, 2, c->maxed);
    bx -= w95::kBtn + 2;
    draw_caption_btn(c->frame, bx, by, 1, c->maxed);
    bx -= w95::kBtn + 2;
    draw_caption_btn(c->frame, bx, by, 0, c->maxed);
    int tx = B + 20;
    int tw = bx - tx - 4;
    draw_str_clip(c->frame, tx, B, tw, T, c->name.c_str(), white.pix, true);
    int cx = B;
    int cy = B + T;
    int cw = c->w - 2 * B;
    int ch = c->h - 2 * B - T;
    sunken(c->frame, cx - 1, cy - 1, cw + 2, ch + 2);
}

static const char *kDeskNames[] = {"My Computer", "My Documents", "Terminal", "Editor"};

void WM::draw_desktop(Monitor &m)
{
    tile_wall(m.desktop, 0, 0, m.w, m.h - w95::kTaskbarH);
    if (!m.primary)
        return;
    int pad = 12;
    for (int i = 0; i < 4; i++) {
        int ix = pad;
        int iy = pad + i * w95::kCellH;
        bool sel = (selected_icon == i);
        draw_icon(m.desktop, ix + 12, iy, i, sel);
        const char *lab = kDeskNames[i];
        int lw = text_w(lab);
        int lx = ix + (w95::kCellW - lw) / 2;
        int ly = iy + w95::kIcon + 6;
        if (sel)
            fill(m.desktop, lx - 2, ly, lw + 4, 16, title.pix);
        draw_str(m.desktop, lx, ly, 16, lab, sel ? white.pix : white.pix, false);
        if (!sel) {
            XSetForeground(dpy, gc, dk.pix);
            XFontStruct *f = font;
            if (f)
                XDrawString(dpy, m.desktop, gc, lx + 1, ly + f->ascent + 1, lab, (int)strlen(lab));
            draw_str(m.desktop, lx, ly, 16, lab, white.pix, false);
        }
    }
}

void WM::draw_clock(Drawable d, int x, int y, int w, int h)
{
    time_t now = time(nullptr);
    struct tm tm{};
    localtime_r(&now, &tm);
    int h12 = tm.tm_hour % 12;
    if (h12 == 0)
        h12 = 12;
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%d:%02d %s", h12, tm.tm_min, tm.tm_hour >= 12 ? "PM" : "AM");
    int cw = text_w(buf);
    draw_str(d, x + (w - cw) / 2, y, h, buf, fg.pix, false);
}

void WM::tile_wall(Drawable d, int x, int y, int w, int h)
{
    fill(d, x, y, w, h, desktop.pix);
    if (!wall_tile || wall_tw < 1 || wall_th < 1)
        return;
    for (int py = y; py < y + h; py += wall_th) {
        for (int px = x; px < x + w; px += wall_tw) {
            int cw = std::min(wall_tw, x + w - px);
            int ch = std::min(wall_th, y + h - py);
            if (cw > 0 && ch > 0)
                XCopyArea(dpy, wall_tile, d, gc, 0, 0, cw, ch, px, py);
        }
    }
}

void WM::draw_dlg_btn(Drawable d, int x, int y, int w, int h, const char *lab)
{
    fill(d, x, y, w, h, face.pix);
    bevel(d, x, y, w, h, true);
    int lw = text_w(lab);
    draw_str(d, x + (w - lw) / 2, y, h, lab, fg.pix, false);
}

void WM::draw_listbox(Drawable d, int x, int y, int w, int h, int sel, int n, const char *(*label)(int))
{
    fill(d, x, y, w, h, field.pix);
    sunken(d, x, y, w, h);
    int vis = (h - 4) / w95::kListRow;
    for (int i = 0; i < n && i < vis; i++) {
        int iy = y + 2 + i * w95::kListRow;
        const char *lab = label(i);
        if (i == sel) {
            fill(d, x + 2, iy, w - 4, w95::kListRow, title.pix);
            draw_str_clip(d, x + 6, iy, w - 12, w95::kListRow, lab, white.pix, false);
        } else
            draw_str_clip(d, x + 6, iy, w - 12, w95::kListRow, lab, fg.pix, false);
    }
}

void WM::draw_setdlg()
{
    const int w = w95::kSetW, h = w95::kSetH;
    fill(setdlg, 0, 0, w, h, face.pix);
    bevel(setdlg, 0, 0, w, h, true);
    fill(setdlg, 3, 3, w - 6, w95::kTitleH, title.pix);
    draw_str(setdlg, 8, 3, w95::kTitleH, "Display Properties", white.pix, true);
    draw_caption_btn(setdlg, w - 6 - w95::kBtn, 5, 2, false);

    draw_str(setdlg, 16, 26, 16, "Preview", fg.pix, false);
    sunken(setdlg, 16, 42, 204, 106);
    fill(setdlg, 20, 46, 196, 78, dk.pix);
    tile_wall(setdlg, 24, 50, 188, 70);
    fill(setdlg, 96, 120, 44, 8, lo.pix);
    fill(setdlg, 80, 128, 76, 12, face.pix);
    bevel(setdlg, 80, 128, 76, 12, true);

    draw_str(setdlg, 232, 26, 16, "Wallpaper", fg.pix, false);
    fill(setdlg, 232, 44, 200, 104, field.pix);
    sunken(setdlg, 232, 44, 200, 104);
    {
        int vis = (104 - 4) / w95::kListRow;
        for (int i = 0; i < (int)walls.size() && i < vis; i++) {
            int iy = 46 + i * w95::kListRow;
            if (i == wall_i) {
                fill(setdlg, 234, iy, 196, w95::kListRow, title.pix);
                draw_str_clip(setdlg, 238, iy, 188, w95::kListRow, walls[i].name.c_str(), white.pix, false);
            } else
                draw_str_clip(setdlg, 238, iy, 188, w95::kListRow, walls[i].name.c_str(), fg.pix, false);
        }
    }

    draw_str(setdlg, 16, 152, 16, "Color scheme", fg.pix, false);
    fill(setdlg, 16, 168, 416, 132, field.pix);
    sunken(setdlg, 16, 168, 416, 132);
    {
        int vis = (132 - 4) / w95::kListRow;
        for (int i = 0; i < w95::kSchemeN && i < vis; i++) {
            int iy = 170 + i * w95::kListRow;
            if (i == scheme_i) {
                fill(setdlg, 18, iy, 412, w95::kListRow, title.pix);
                draw_str_clip(setdlg, 22, iy, 404, w95::kListRow, w95::kSchemes[i].name, white.pix, false);
            } else
                draw_str_clip(setdlg, 22, iy, 404, w95::kListRow, w95::kSchemes[i].name, fg.pix, false);
        }
    }

    XSetForeground(dpy, gc, lo.pix);
    XDrawLine(dpy, setdlg, gc, 8, h - 44, w - 9, h - 44);
    XSetForeground(dpy, gc, hi.pix);
    XDrawLine(dpy, setdlg, gc, 8, h - 43, w - 9, h - 43);

    const int bw = w95::kDlgBtnW, bh = w95::kDlgBtnH;
    int by = h - 12 - bh;
    int apply_x = w - 12 - bw;
    int cancel_x = apply_x - 8 - bw;
    int ok_x = cancel_x - 8 - bw;
    draw_dlg_btn(setdlg, ok_x, by, bw, bh, "OK");
    draw_dlg_btn(setdlg, cancel_x, by, bw, bh, "Cancel");
    draw_dlg_btn(setdlg, apply_x, by, bw, bh, "Apply");
}

void WM::draw_tray()
{
    if (!traywin)
        return;
    XWindowAttributes wa{};
    if (!XGetWindowAttributes(dpy, traywin, &wa) || wa.map_state == IsUnmapped)
        return;
    fill(traywin, 0, 0, wa.width, wa.height, face.pix);
    sunken(traywin, 0, 0, wa.width, wa.height);
    draw_clock(traywin, wa.width - w95::kClockW, 0, w95::kClockW, wa.height);
}

void WM::draw_taskbar(Monitor &m)
{
    fill(m.taskbar, 0, 0, m.w, w95::kTaskbarH, face.pix);
    XSetForeground(dpy, gc, hi.pix);
    XDrawLine(dpy, m.taskbar, gc, 0, 0, m.w, 0);
    XSetForeground(dpy, gc, face.pix);
    XDrawLine(dpy, m.taskbar, gc, 0, 1, m.w, 1);

    int sbx = 2, sby = 3, sbw = w95::kStartW, sbh = w95::kTaskbarH - 6;
    fill(m.taskbar, sbx, sby, sbw, sbh, face.pix);
    bevel(m.taskbar, sbx, sby, sbw, sbh, !m.start_open);
    draw_flag(m.taskbar, sbx + 4, sby + 4);
    draw_str(m.taskbar, sbx + 22, sby, sbh, "Start", fg.pix, true);

    int trayw = tray_width(m);
    int trayx = m.w - trayw - 4;
    if (!(m.primary && traywin)) {
        sunken(m.taskbar, trayx, 4, trayw, w95::kTaskbarH - 8);
        draw_clock(m.taskbar, trayx, 4, trayw, w95::kTaskbarH - 8);
    } else
        draw_tray();

    auto list = clients_on(mon_index(&m));
    int x0 = sbx + sbw + 6;
    int x1 = trayx - 6;
    int avail = x1 - x0;
    if (avail < 16 || list.empty())
        return;
    int n = (int)list.size();
    int bw = std::min(160, std::max(70, avail / n - 2));
    int x = x0;
    for (int i = 0; i < n && x + 20 < x1; i++) {
        Client *c = list[i];
        bool down = (focused == c && !c->iconic);
        int bw2 = std::min(bw, x1 - x);
        fill(m.taskbar, x, 4, bw2, w95::kTaskbarH - 8, face.pix);
        bevel(m.taskbar, x, 4, bw2, w95::kTaskbarH - 8, !down);
        draw_str_clip(m.taskbar, x + 6, 4, bw2 - 10, w95::kTaskbarH - 8, c->name.c_str(), fg.pix, false);
        x += bw2 + 2;
    }
}

MenuItem kMenu[] = {
    {"Programs", false, true, true},
    {"Documents", false, false, true},
    {"Settings", false, false, true},
    {"Find", false, true, false},
    {"Help", false, false, true},
    {"Run...", false, false, true},
    {nullptr, true, false, false},
    {"Shut Down...", false, false, true},
};

const int kMenuN = 8;

int menu_height()
{
    int h = 8;
    for (int i = 0; i < kMenuN; i++)
        h += kMenu[i].sep ? 8 : w95::kMenuItemH;
    return h;
}

void WM::draw_startmenu(Monitor &m)
{
    int mw = w95::kBannerW + w95::kMenuBodyW;
    int mh = menu_height();
    fill(m.startmenu, 0, 0, mw, mh, face.pix);
    bevel(m.startmenu, 0, 0, mw, mh, true);
    fill(m.startmenu, 3, 3, w95::kBannerW - 2, mh - 6, banner.pix);
    const char *letters = "Chime";
    int ly = mh - 18;
    for (int i = (int)strlen(letters) - 1; i >= 0; i--) {
        char ch[2] = {letters[i], 0};
        if (letters[i] == ' ') {
            ly -= 8;
            continue;
        }
        bool num = (letters[i] >= '0' && letters[i] <= '9');
        draw_str(m.startmenu, 7, ly - 12, 14, ch, num ? white.pix : lo.pix, true);
        ly -= 13;
    }
    int y = 4;
    int bx = w95::kBannerW;
    for (int i = 0; i < kMenuN; i++) {
        if (kMenu[i].sep) {
            sunken(m.startmenu, bx + 4, y + 2, w95::kMenuBodyW - 10, 2);
            y += 8;
            continue;
        }
        int ih = w95::kMenuItemH;
        bool hot = (m.hover == i);
        if (hot && kMenu[i].enabled)
            fill(m.startmenu, bx + 2, y, w95::kMenuBodyW - 6, ih, title.pix);
        unsigned long col = !kMenu[i].enabled ? lo.pix : (hot ? white.pix : fg.pix);
        draw_str(m.startmenu, bx + 10, y, ih, kMenu[i].label, col, false);
        if (kMenu[i].sub) {
            XPoint p[3];
            int ax = bx + w95::kMenuBodyW - 16;
            int ay = y + ih / 2;
            p[0] = {(short)ax, (short)(ay - 4)};
            p[1] = {(short)ax, (short)(ay + 4)};
            p[2] = {(short)(ax + 5), (short)ay};
            XSetForeground(dpy, gc, col);
            XFillPolygon(dpy, m.startmenu, gc, p, 3, Convex, CoordModeOrigin);
        }
        y += ih;
    }
}

void WM::draw_submenu(Monitor &m)
{
    const char *items[] = {"Terminal", "Editor", "Cabinet"};
    int n = 3;
    int mw = 150, mh = 8 + n * w95::kMenuItemH;
    fill(m.submenu, 0, 0, mw, mh, face.pix);
    bevel(m.submenu, 0, 0, mw, mh, true);
    for (int i = 0; i < n; i++) {
        int y = 4 + i * w95::kMenuItemH;
        bool hot = (m.subhover == i);
        if (hot)
            fill(m.submenu, 3, y, mw - 6, w95::kMenuItemH, title.pix);
        draw_str(m.submenu, 12, y, w95::kMenuItemH, items[i], hot ? white.pix : fg.pix, false);
    }
}

void WM::draw_rundlg()
{
    const int w = w95::kRunW, h = w95::kRunH;
    fill(rundlg, 0, 0, w, h, face.pix);
    bevel(rundlg, 0, 0, w, h, true);
    fill(rundlg, 3, 3, w - 6, w95::kTitleH, title.pix);
    draw_str(rundlg, 8, 3, w95::kTitleH, "Run", white.pix, true);
    draw_caption_btn(rundlg, w - 6 - w95::kBtn, 5, 2, false);

    draw_icon(rundlg, 12, 30, 0, false);
    draw_str(rundlg, 52, 28, 16, "Type the name of a program, folder,", fg.pix, false);
    draw_str(rundlg, 52, 44, 16, "or document, and Chime will open it.", fg.pix, false);

    const char *openlab = "Open:";
    int labw = text_w(openlab);
    int fy = 88, fh = 22;
    draw_str(rundlg, 12, fy, fh, openlab, fg.pix, false);
    int fx = 12 + labw + 10;
    int fw = w - fx - 14;
    sunken(rundlg, fx, fy, fw, fh);
    fill(rundlg, fx + 2, fy + 2, fw - 4, fh - 4, field.pix);
    int tx = fx + 6;
    int tw = fw - 12;
    draw_str_clip(rundlg, tx, fy + 2, tw - 6, fh - 4, run_text.c_str(), fg.pix, false);
    int caret = tx + text_w(run_text.c_str());
    if (caret > tx + tw - 4)
        caret = tx + tw - 4;
    fill(rundlg, caret, fy + 4, 1, fh - 8, fg.pix);

    XSetForeground(dpy, gc, lo.pix);
    XDrawLine(dpy, rundlg, gc, 8, h - 44, w - 9, h - 44);
    XSetForeground(dpy, gc, hi.pix);
    XDrawLine(dpy, rundlg, gc, 8, h - 43, w - 9, h - 43);

    const int bw = w95::kDlgBtnW, bh = w95::kDlgBtnH;
    int by = h - 12 - bh;
    int cancel_x = w - 12 - bw;
    int ok_x = cancel_x - 8 - bw;
    draw_dlg_btn(rundlg, ok_x, by, bw, bh, "OK");
    draw_dlg_btn(rundlg, cancel_x, by, bw, bh, "Cancel");
}

void WM::draw_shutdlg()
{
    int w = 320, h = 120;
    fill(shutdlg, 0, 0, w, h, face.pix);
    bevel(shutdlg, 0, 0, w, h, true);
    fill(shutdlg, 4, 4, w - 8, w95::kTitleH, title.pix);
    draw_str(shutdlg, 8, 4, w95::kTitleH, "Shut Down", white.pix, true);
    draw_str(shutdlg, 16, 32, 24, "Are you sure you want to shut down?", fg.pix, false);
    int bw = 74, bh = 24;
    fill(shutdlg, w / 2 - bw - 8, h - 36, bw, bh, face.pix);
    bevel(shutdlg, w / 2 - bw - 8, h - 36, bw, bh, true);
    draw_str(shutdlg, w / 2 - bw - 8 + 24, h - 36, bh, "Yes", fg.pix, false);
    fill(shutdlg, w / 2 + 8, h - 36, bw, bh, face.pix);
    bevel(shutdlg, w / 2 + 8, h - 36, bw, bh, true);
    draw_str(shutdlg, w / 2 + 8 + 28, h - 36, bh, "No", fg.pix, false);
}
