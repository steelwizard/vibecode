#include "wm.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <algorithm>

// Drawing for Chime chrome: bevels, captions, desktop, taskbar, menus, dialogs.
// All painting goes through the WM's single GC. Callers that clip (draw_str_clip)
// must restore the clip mask so later fills are not truncated.

// Solid rectangle. Skips empty dests because XFillRectangle with w/h 0 is
// an X error on some servers.
void WM::fill(Drawable d, int x, int y, int w, int h, unsigned long p)
{
    if (w <= 0 || h <= 0)
        return;
    XSetForeground(dpy, gc, p);
    XFillRectangle(dpy, d, gc, x, y, w, h);
}

// Classic 2px Win95 bevel. Raised = light on top/left (button at rest);
// sunken = light on bottom/right (well, pressed Start button).
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

// Vertically center a string in a box of height box_h. XDrawString's y is the
// baseline, so we add ascent after the extra top padding.
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

// Caption chrome: kind 0 = minimize, 1 = maximize/restore, 2 = close (X).
void WM::draw_caption_btn(Drawable d, int x, int y, int kind, bool maxed)
{
    fill(d, x, y, w95::kBtn, w95::kBtnH, face.pix);
    bevel(d, x, y, w95::kBtn, w95::kBtnH, true);
    XSetForeground(dpy, gc, fg.pix);
    if (kind == 0) {
        // Underscore at the bottom of the button.
        XDrawLine(dpy, d, gc, x + 3, y + w95::kBtnH - 5, x + w95::kBtn - 4, y + w95::kBtnH - 5);
        XDrawLine(dpy, d, gc, x + 3, y + w95::kBtnH - 4, x + w95::kBtn - 4, y + w95::kBtnH - 4);
    } else if (kind == 1) {
        if (maxed) {
            // Two overlapping rects = "restore".
            XDrawRectangle(dpy, d, gc, x + 5, y + 3, 6, 5);
            XDrawRectangle(dpy, d, gc, x + 3, y + 5, 6, 5);
        } else {
            XDrawRectangle(dpy, d, gc, x + 3, y + 3, w95::kBtn - 7, w95::kBtnH - 7);
            XDrawLine(dpy, d, gc, x + 3, y + 4, x + w95::kBtn - 5, y + 4);
        }
    } else {
        // Close: two-pixel-thick X.
        XDrawLine(dpy, d, gc, x + 4, y + 3, x + w95::kBtn - 5, y + w95::kBtnH - 4);
        XDrawLine(dpy, d, gc, x + 5, y + 3, x + w95::kBtn - 4, y + w95::kBtnH - 4);
        XDrawLine(dpy, d, gc, x + w95::kBtn - 5, y + 3, x + 4, y + w95::kBtnH - 4);
        XDrawLine(dpy, d, gc, x + w95::kBtn - 4, y + 3, x + 5, y + w95::kBtnH - 4);
    }
}

// Desktop / Run-dialog glyphs. kind: 0 computer, 1 folder, 2 terminal, 3 document.
void WM::draw_icon(Drawable d, int x, int y, int kind, bool selected)
{
    if (selected)
        fill(d, x - 4, y - 2, w95::kIcon + 8, w95::kIcon + 6, title.pix);

    if (kind == 0) {
        // CRT on a stand, yellow "screen glow" at the corner.
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

// Paint the frame around a client: outer bevel, caption, sys-menu box,
// min/max/close, title text, sunken client well. The application window is
// a child; we only draw the chrome here.
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

// Wallpaper plus, on the primary monitor only, the four desktop shortcuts.
// Non-primary screens stay empty so we don't duplicate icons across heads.
void WM::draw_desktop(Monitor &m)
{
    tile_wall(m.desktop, 0, 0, m.w, m.h - w95::kTaskbarH);
    if (!m.primary)
        return;
    int pad = 12;
    for (int i = 0; i < 4; i++) {
        int ix = pad;
        int iy = pad + i * w95::kCellH;
        bool sel = (desk_mask & (1u << i)) != 0;
        draw_icon(m.desktop, ix + 12, iy, i, sel);
        const char *lab = kDeskNames[i];
        int lw = text_w(lab);
        int lx = ix + (w95::kCellW - lw) / 2;
        int ly = iy + w95::kIcon + 6;
        if (sel)
            fill(m.desktop, lx - 2, ly, lw + 4, 16, title.pix);
        draw_str(m.desktop, lx, ly, 16, lab, sel ? white.pix : white.pix, false);
        if (!sel) {
            // 1px black offset under white text so labels read on teal/patterns.
            XSetForeground(dpy, gc, dk.pix);
            XFontStruct *f = font;
            if (f)
                XDrawString(dpy, m.desktop, gc, lx + 1, ly + f->ascent + 1, lab, (int)strlen(lab));
            draw_str(m.desktop, lx, ly, 16, lab, white.pix, false);
        }
    }
    if (drag == DragMode::Select && sel_mon == mon_index(&m))
        draw_dotted_rect(m.desktop, sel_x0, sel_y0, sel_x1, sel_y1);
}

void WM::draw_dotted_rect(Drawable d, int x0, int y0, int x1, int y1)
{
    int x = std::min(x0, x1), y = std::min(y0, y1);
    int w = std::abs(x1 - x0), h = std::abs(y1 - y0);
    if (w < 1 || h < 1)
        return;
    char dash[] = {1, 1};
    XSetDashes(dpy, gc, 0, dash, 2);
    XSetLineAttributes(dpy, gc, 0, LineOnOffDash, CapButt, JoinMiter);
    XSetForeground(dpy, gc, dk.pix);
    XDrawRectangle(dpy, d, gc, x, y, (unsigned)w, (unsigned)h);
    XSetForeground(dpy, gc, white.pix);
    XSetDashes(dpy, gc, 1, dash, 2);
    XDrawRectangle(dpy, d, gc, x, y, (unsigned)w, (unsigned)h);
    XSetLineAttributes(dpy, gc, 0, LineSolid, CapButt, JoinMiter);
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

static const char *kRoleName[] = {
    "Desktop",       "Window",         "3D Highlight",  "3D Shadow", "3D Dark",
    "Active Title",  "Inactive Title", "Text",          "Text box",  "Start banner",
};

// Display Properties: mini desktop preview, wallpaper list + Browse, scheme
// list + New/Edit/Delete, OK / Cancel / Apply. Hit testing uses kWallList* /
// kSchemeList* / kBrowse* / kSchemeBtn* in theme.h.
void WM::draw_setdlg()
{
    const int w = w95::kSetW, h = w95::kSetH;
    fill(setdlg, 0, 0, w, h, face.pix);
    bevel(setdlg, 0, 0, w, h, true);
    fill(setdlg, 3, 3, w - 6, w95::kTitleH, title.pix);
    draw_str(setdlg, 8, 3, w95::kTitleH, "Display Properties", white.pix, true);
    draw_caption_btn(setdlg, w - 6 - w95::kBtn, 5, 2, false);

    draw_str(setdlg, 16, 26, 16, "Preview", fg.pix, false);
    sunken(setdlg, 16, 42, 204, 88);
    fill(setdlg, 20, 46, 196, 62, dk.pix);
    tile_wall(setdlg, 24, 50, 188, 54);
    fill(setdlg, 96, 108, 44, 6, lo.pix);
    fill(setdlg, 80, 114, 76, 10, face.pix);
    bevel(setdlg, 80, 114, 76, 10, true);

    draw_str(setdlg, w95::kWallListX, 26, 16, "Wallpaper", fg.pix, false);
    fill(setdlg, w95::kWallListX, w95::kWallListY, w95::kWallListW, w95::kWallListH, field.pix);
    sunken(setdlg, w95::kWallListX, w95::kWallListY, w95::kWallListW, w95::kWallListH);
    {
        int vis = (w95::kWallListH - 4) / w95::kListRow;
        ensure_list_scroll(wall_scroll, wall_i, vis, (int)walls.size());
        for (int i = 0; i < vis; i++) {
            int idx = wall_scroll + i;
            if (idx >= (int)walls.size())
                break;
            int iy = w95::kWallListY + 2 + i * w95::kListRow;
            if (idx == wall_i) {
                fill(setdlg, w95::kWallListX + 2, iy, w95::kWallListW - 4, w95::kListRow, title.pix);
                draw_str_clip(setdlg, w95::kWallListX + 6, iy, w95::kWallListW - 12, w95::kListRow,
                              walls[idx].name.c_str(), white.pix, false);
            } else
                draw_str_clip(setdlg, w95::kWallListX + 6, iy, w95::kWallListW - 12, w95::kListRow,
                              walls[idx].name.c_str(), fg.pix, false);
        }
    }
    draw_dlg_btn(setdlg, w95::kBrowseX, w95::kBrowseY, w95::kBrowseW, w95::kDlgBtnH, "Browse...");

    draw_str(setdlg, 16, 160, 16, "Color scheme", fg.pix, false);
    fill(setdlg, w95::kSchemeListX, w95::kSchemeListY, w95::kSchemeListW, w95::kSchemeListH, field.pix);
    sunken(setdlg, w95::kSchemeListX, w95::kSchemeListY, w95::kSchemeListW, w95::kSchemeListH);
    {
        int vis = (w95::kSchemeListH - 4) / w95::kListRow;
        ensure_list_scroll(scheme_scroll, scheme_i, vis, (int)schemes.size());
        for (int i = 0; i < vis; i++) {
            int idx = scheme_scroll + i;
            if (idx >= (int)schemes.size())
                break;
            int iy = w95::kSchemeListY + 2 + i * w95::kListRow;
            std::string lab = schemes[idx].name;
            if (!schemes[idx].builtin)
                lab += " *";
            if (idx == scheme_i) {
                fill(setdlg, w95::kSchemeListX + 2, iy, w95::kSchemeListW - 4, w95::kListRow, title.pix);
                draw_str_clip(setdlg, w95::kSchemeListX + 6, iy, w95::kSchemeListW - 12, w95::kListRow, lab.c_str(),
                              white.pix, false);
            } else
                draw_str_clip(setdlg, w95::kSchemeListX + 6, iy, w95::kSchemeListW - 12, w95::kListRow, lab.c_str(),
                              fg.pix, false);
        }
    }
    int byb = w95::kSchemeBtnY;
    draw_dlg_btn(setdlg, w95::kSchemeBtnX, byb, 96, w95::kDlgBtnH, "New");
    draw_dlg_btn(setdlg, w95::kSchemeBtnX, byb + 28, 96, w95::kDlgBtnH, "Edit...");
    draw_dlg_btn(setdlg, w95::kSchemeBtnX, byb + 56, 96, w95::kDlgBtnH, "Delete");

    draw_str(setdlg, w95::kResListX, w95::kResListY - 16, 16, "Screen resolution", fg.pix, false);
    fill(setdlg, w95::kResListX, w95::kResListY, w95::kResListW, w95::kResListH, field.pix);
    sunken(setdlg, w95::kResListX, w95::kResListY, w95::kResListW, w95::kResListH);
    {
        int vis = (w95::kResListH - 4) / w95::kListRow;
        ensure_list_scroll(mode_scroll, mode_i, vis, (int)modes.size());
        for (int i = 0; i < vis; i++) {
            int idx = mode_scroll + i;
            if (idx >= (int)modes.size())
                break;
            int iy = w95::kResListY + 2 + i * w95::kListRow;
            char lab[32];
            std::snprintf(lab, sizeof lab, "%d x %d", modes[idx].w, modes[idx].h);
            if (idx == mode_i) {
                fill(setdlg, w95::kResListX + 2, iy, w95::kResListW - 4, w95::kListRow, title.pix);
                draw_str_clip(setdlg, w95::kResListX + 6, iy, w95::kResListW - 12, w95::kListRow, lab, white.pix, false);
            } else
                draw_str_clip(setdlg, w95::kResListX + 6, iy, w95::kResListW - 12, w95::kListRow, lab, fg.pix, false);
        }
    }
    if (mode_i >= 0 && mode_i < (int)modes.size()) {
        char cur[64];
        std::snprintf(cur, sizeof cur, "Desktop area: %d by %d pixels", modes[mode_i].w, modes[mode_i].h);
        draw_str(setdlg, w95::kResListX + w95::kResListW + 12, w95::kResListY + 8, 16, cur, fg.pix, false);
    }
    if (!mode_note.empty())
        draw_str_clip(setdlg, w95::kResListX + w95::kResListW + 12, w95::kResListY + 28, 200, 32, mode_note.c_str(),
                      fg.pix, false);

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

void WM::draw_colordlg()
{
    const int w = w95::kColorW, h = w95::kColorH;
    fill(colordlg, 0, 0, w, h, face.pix);
    bevel(colordlg, 0, 0, w, h, true);
    fill(colordlg, 3, 3, w - 6, w95::kTitleH, title.pix);
    draw_str(colordlg, 8, 3, w95::kTitleH, "Edit Color Scheme", white.pix, true);
    draw_caption_btn(colordlg, w - 6 - w95::kBtn, 5, 2, false);

    draw_str(colordlg, 16, 26, 18, "Name", fg.pix, false);
    fill(colordlg, 60, 26, w - 76, 20, field.pix);
    sunken(colordlg, 60, 26, w - 76, 20);
    draw_str_clip(colordlg, 66, 26, w - 88, 20, color_name_buf.c_str(), fg.pix, false);
    if (color_name_edit && caret_on) {
        int cx = 66 + text_w(color_name_buf.c_str());
        fill(colordlg, cx, 30, 2, 12, fg.pix);
    }

    fill(colordlg, 16, 54, 180, 180, field.pix);
    sunken(colordlg, 16, 54, 180, 180);
    {
        int vis = (180 - 4) / w95::kListRow;
        for (int i = 0; i < w95::kColorRoleN && i < vis; i++) {
            int iy = 56 + i * w95::kListRow;
            if (i == color_role) {
                fill(colordlg, 18, iy, 176, w95::kListRow, title.pix);
                draw_str_clip(colordlg, 24, iy, 164, w95::kListRow, kRoleName[i], white.pix, false);
            } else
                draw_str_clip(colordlg, 24, iy, 164, w95::kListRow, kRoleName[i], fg.pix, false);
        }
    }

    ColorScheme dummy;
    ColorScheme &s = (scheme_i >= 0 && scheme_i < (int)schemes.size()) ? schemes[scheme_i] : dummy;
    w95::Rgb rgb = scheme_role(s, color_role);
    unsigned long sw = pixel_from_rgb(rgb.r, rgb.g, rgb.b);
    fill(colordlg, 212, 54, 188, 70, sw);
    sunken(colordlg, 212, 54, 188, 70);

    auto bar = [&](int y, const char *lab, int val, int chan) {
        draw_str(colordlg, 212, y, 16, lab, fg.pix, false);
        fill(colordlg, 232, y + 2, 160, 12, field.pix);
        sunken(colordlg, 232, y + 2, 160, 12);
        int fw = std::max(2, val * 156 / 255);
        fill(colordlg, 234, y + 4, fw, 8, chan == color_chan ? title.pix : lo.pix);
        char buf[8];
        std::snprintf(buf, sizeof buf, "%d", val);
        draw_str(colordlg, 396, y, 16, buf, fg.pix, false);
    };
    bar(136, "R", rgb.r, 0);
    bar(160, "G", rgb.g, 1);
    bar(184, "B", rgb.b, 2);
    draw_str(colordlg, 212, 214, 32, "Click a bar or use arrows.", fg.pix, false);

    const int bw = w95::kDlgBtnW, bh = w95::kDlgBtnH;
    int by = h - 12 - bh;
    int cancel_x = w - 12 - bw;
    int ok_x = cancel_x - 8 - bw;
    draw_dlg_btn(colordlg, ok_x, by, bw, bh, "OK");
    draw_dlg_btn(colordlg, cancel_x, by, bw, bh, "Cancel");
}

void WM::draw_filedlg()
{
    const int w = w95::kFileW, h = w95::kFileH;
    fill(filedlg, 0, 0, w, h, face.pix);
    bevel(filedlg, 0, 0, w, h, true);
    fill(filedlg, 3, 3, w - 6, w95::kTitleH, title.pix);
    draw_str(filedlg, 8, 3, w95::kTitleH, "Browse for Wallpaper", white.pix, true);
    draw_caption_btn(filedlg, w - 6 - w95::kBtn, 5, 2, false);

    draw_str(filedlg, 12, 26, 18, "Look in", fg.pix, false);
    fill(filedlg, 68, 26, w - 160, 20, field.pix);
    sunken(filedlg, 68, 26, w - 160, 20);
    draw_str_clip(filedlg, 74, 26, w - 172, 20, file_dir.c_str(), fg.pix, false);
    draw_dlg_btn(filedlg, w - 84, 24, 32, 22, "Up");
    draw_dlg_btn(filedlg, w - 48, 24, 36, 22, "Home");

    int lx = 12, ly = 54, lw = w - 24, lh = 180;
    fill(filedlg, lx, ly, lw, lh, field.pix);
    sunken(filedlg, lx, ly, lw, lh);
    int vis = (lh - 4) / w95::kListRow;
    ensure_list_scroll(file_scroll, file_sel, vis, (int)file_ents.size());
    for (int i = 0; i < vis; i++) {
        int idx = file_scroll + i;
        if (idx >= (int)file_ents.size())
            break;
        int iy = ly + 2 + i * w95::kListRow;
        std::string lab = file_ents[idx].name;
        if (file_ents[idx].dir)
            lab += " /";
        if (idx == file_sel) {
            fill(filedlg, lx + 2, iy, lw - 4, w95::kListRow, title.pix);
            draw_str_clip(filedlg, lx + 8, iy, lw - 16, w95::kListRow, lab.c_str(), white.pix, false);
        } else
            draw_str_clip(filedlg, lx + 8, iy, lw - 16, w95::kListRow, lab.c_str(), fg.pix, false);
    }

    draw_str(filedlg, 12, 242, 20, "File", fg.pix, false);
    fill(filedlg, 48, 242, w - 60, 20, field.pix);
    sunken(filedlg, 48, 242, w - 60, 20);
    draw_str_clip(filedlg, 54, 242, w - 72, 20, file_typed.c_str(), fg.pix, false);
    if (caret_on) {
        int cx = 54 + text_w(file_typed.c_str());
        fill(filedlg, cx, 246, 2, 12, fg.pix);
    }

    const int bw = w95::kDlgBtnW, bh = w95::kDlgBtnH;
    int by = h - 12 - bh;
    int cancel_x = w - 12 - bw;
    int ok_x = cancel_x - 8 - bw;
    draw_dlg_btn(filedlg, ok_x, by, bw, bh, "OK");
    draw_dlg_btn(filedlg, cancel_x, by, bw, bh, "Cancel");
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

// Taskbar: highlight along the top, Start, task buttons for windows on this
// monitor, then tray/clock. The primary bar hosts traywin as a child; other
// bars draw a clock well in place.
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
    const char *slabel = "Start";
    int slw = text_w(slabel, true);
    draw_str(m.taskbar, sbx + (sbw - slw) / 2, sby, sbh, slabel, fg.pix, true);

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
    // Vertical "Chime" along the banner, drawn bottom-up like "Windows 95".
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
    int caret = tx + text_w(run_text.c_str(), false);
    // Prefer the insertion index if the Run dialog is using a moving caret.
    if (run_cursor >= 0 && run_cursor <= (int)run_text.size()) {
        std::string pre = run_text.substr(0, (size_t)run_cursor);
        caret = tx + text_w(pre.c_str(), false);
    }
    if (caret > tx + tw - 4)
        caret = tx + tw - 4;
    if (caret_on)
        fill(rundlg, caret, fy + 4, 2, fh - 8, fg.pix);

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
