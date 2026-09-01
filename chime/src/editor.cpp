// Editor — Win95-style Notepad for Chime.
// Arrow keys move a blinking caret; typing inserts at the caret. Open a path
// as argv[1] (Cabinet does this). Files are plain UTF-8 treated as bytes.

#include "theme.h"

#include <X11/Xlib.h>
#include <X11/Xatom.h>
#include <X11/Xutil.h>
#include <X11/keysym.h>

#include <algorithm>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <string>
#include <sys/select.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>
#include <vector>

namespace {

constexpr int kWinW = 560;
constexpr int kWinH = 400;
constexpr int kMenuH = 20;
constexpr int kToolH = 26;
constexpr int kStatH = 22;
constexpr int kPad = 6;
constexpr long kBlinkMs = 530;

struct Col {
    unsigned long pix;
};

Display *dpy;
int screen;
Window win;
GC gc;
XFontStruct *font;
Col face, hi, lo, dk, title, fg, white, field;
int ww = kWinW, wh = kWinH;
Atom wm_delete, wm_protocols;

std::vector<std::string> lines;
int row = 0, col = 0;
int scroll = 0, scroll_x = 0;
bool dirty = false;
bool caret_on = true;
long last_blink = 0;
std::string path;
std::string status = "Ready";

enum class Prompt { Idle, Open, SaveAs };
Prompt prompt = Prompt::Idle;
std::string pbuf;

long now_ms()
{
    timeval tv{};
    gettimeofday(&tv, nullptr);
    return tv.tv_sec * 1000L + tv.tv_usec / 1000;
}

unsigned long alloc_rgb(w95::Rgb c)
{
    XColor xc{};
    xc.red = (unsigned short)(c.r * 257);
    xc.green = (unsigned short)(c.g * 257);
    xc.blue = (unsigned short)(c.b * 257);
    xc.flags = DoRed | DoGreen | DoBlue;
    XAllocColor(dpy, DefaultColormap(dpy, screen), &xc);
    return xc.pixel;
}

void fill(int x, int y, int w, int h, unsigned long p)
{
    if (w <= 0 || h <= 0)
        return;
    XSetForeground(dpy, gc, p);
    XFillRectangle(dpy, win, gc, x, y, w, h);
}

void bevel(int x, int y, int w, int h, bool raised)
{
    unsigned long tl = raised ? hi.pix : lo.pix;
    unsigned long br = raised ? dk.pix : hi.pix;
    XSetForeground(dpy, gc, tl);
    XDrawLine(dpy, win, gc, x, y, x + w - 1, y);
    XDrawLine(dpy, win, gc, x, y, x, y + h - 1);
    XSetForeground(dpy, gc, br);
    XDrawLine(dpy, win, gc, x, y + h - 1, x + w - 1, y + h - 1);
    XDrawLine(dpy, win, gc, x + w - 1, y, x + w - 1, y + h - 1);
}

int text_w(const char *s, int n = -1)
{
    if (!s)
        return 0;
    if (n < 0)
        n = (int)strlen(s);
    return font ? XTextWidth(font, s, n) : n * 8;
}

void draw_str(int x, int y, int box_h, const char *s, unsigned long pix, int n = -1)
{
    if (!font || !s)
        return;
    if (n < 0)
        n = (int)strlen(s);
    int th = font->ascent + font->descent;
    int ty = y + font->ascent + std::max(0, (box_h - th) / 2);
    XSetFont(dpy, gc, font->fid);
    XSetForeground(dpy, gc, pix);
    XDrawString(dpy, win, gc, x, ty, s, n);
}

void clip_str(int x, int y, int w, int h, const char *s, unsigned long pix, int n = -1)
{
    XRectangle r{(short)x, (short)y, (unsigned short)std::max(0, w), (unsigned short)std::max(0, h)};
    XSetClipRectangles(dpy, gc, 0, 0, &r, 1, Unsorted);
    draw_str(x, y, h, s, pix, n);
    XSetClipMask(dpy, gc, None);
}

void btn(int x, int y, int w, int h, const char *lab)
{
    fill(x, y, w, h, face.pix);
    bevel(x, y, w, h, true);
    int tw = text_w(lab);
    draw_str(x + (w - tw) / 2, y, h, lab, fg.pix);
}

void set_title()
{
    std::string t = "Editor";
    if (!path.empty())
        t += " - " + path;
    if (dirty)
        t += " *";
    XStoreName(dpy, win, t.c_str());
}

void reset_doc()
{
    lines.clear();
    lines.push_back("");
    row = col = scroll = scroll_x = 0;
    dirty = false;
}

bool load_file(const std::string &p)
{
    FILE *f = fopen(p.c_str(), "rb");
    if (!f) {
        status = "Cannot open " + p;
        return false;
    }
    lines.clear();
    std::string cur;
    int c;
    while ((c = fgetc(f)) != EOF) {
        if (c == '\r')
            continue;
        if (c == '\n') {
            lines.push_back(cur);
            cur.clear();
        } else
            cur.push_back((char)c);
    }
    lines.push_back(cur);
    if (lines.empty())
        lines.push_back("");
    fclose(f);
    path = p;
    row = col = scroll = scroll_x = 0;
    dirty = false;
    status = "Opened " + p;
    set_title();
    return true;
}

bool save_file(const std::string &p)
{
    FILE *f = fopen(p.c_str(), "wb");
    if (!f) {
        status = "Cannot save " + p;
        return false;
    }
    for (size_t i = 0; i < lines.size(); i++) {
        fwrite(lines[i].data(), 1, lines[i].size(), f);
        if (i + 1 < lines.size())
            fputc('\n', f);
    }
    fclose(f);
    path = p;
    dirty = false;
    status = "Saved " + p;
    set_title();
    return true;
}

int row_h() { return font ? font->ascent + font->descent + 2 : 16; }
int text_top() { return kMenuH + kToolH + kPad; }
int text_h() { return std::max(row_h(), wh - text_top() - kStatH - kPad); }
int rows_vis() { return std::max(1, text_h() / row_h()); }
int text_left() { return kPad + 4; }
int text_width() { return std::max(8, ww - 2 * kPad - 4); }

void show_caret()
{
    caret_on = true;
    last_blink = now_ms();
}

void clamp_caret()
{
    if (lines.empty())
        reset_doc();
    row = std::clamp(row, 0, (int)lines.size() - 1);
    col = std::clamp(col, 0, (int)lines[row].size());
    if (row < scroll)
        scroll = row;
    int vis = rows_vis();
    if (row >= scroll + vis)
        scroll = row - vis + 1;

    int prefix = text_w(lines[row].c_str(), col);
    int tw = text_width();
    if (prefix - scroll_x > tw - 8)
        scroll_x = prefix - tw + 8;
    if (prefix < scroll_x)
        scroll_x = std::max(0, prefix - 8);
}

void redraw()
{
    fill(0, 0, ww, wh, face.pix);
    draw_str(8, 0, kMenuH, "File   Edit   Help", fg.pix);

    int ty = kMenuH + 3;
    btn(6, ty, 40, 20, "New");
    btn(50, ty, 48, 20, "Open");
    btn(102, ty, 48, 20, "Save");
    btn(154, ty, 64, 20, "Save As");

    int lx = kPad, ly = text_top(), lw = ww - 2 * kPad, lh = text_h();
    fill(lx, ly, lw, lh, field.pix);
    bevel(lx, ly, lw, lh, false);

    clamp_caret();
    XRectangle clip{(short)(lx + 2), (short)(ly + 2), (unsigned short)std::max(0, lw - 4),
                    (unsigned short)std::max(0, lh - 4)};
    XSetClipRectangles(dpy, gc, 0, 0, &clip, 1, Unsorted);

    int rh = row_h();
    int vis = rows_vis();
    int tx = text_left() - scroll_x;
    for (int i = 0; i < vis; i++) {
        int idx = scroll + i;
        if (idx >= (int)lines.size())
            break;
        int y = ly + 2 + i * rh;
        const std::string &s = lines[idx];
        draw_str(tx, y, rh, s.c_str(), fg.pix);
        if (idx == row && caret_on && prompt == Prompt::Idle) {
            int cx = tx + text_w(s.c_str(), col);
            int ch = font ? font->ascent + font->descent : rh - 4;
            int cy = y + std::max(0, (rh - ch) / 2);
            // 2px Win95 caret; inverted so it reads on white and on black text.
            fill(cx, cy, 2, ch, title.pix);
        }
    }
    XSetClipMask(dpy, gc, None);

    int sy = wh - kStatH;
    fill(0, sy, ww, kStatH, face.pix);
    bevel(2, sy + 2, ww - 4, kStatH - 4, false);
    std::string st = status;
    if (prompt == Prompt::Open)
        st = "Open: " + pbuf + "_";
    else if (prompt == Prompt::SaveAs)
        st = "Save as: " + pbuf + "_";
    else {
        char buf[64];
        std::snprintf(buf, sizeof buf, "  Ln %d, Col %d", row + 1, col + 1);
        st += buf;
    }
    clip_str(8, sy + 2, ww - 16, kStatH - 4, st.c_str(), fg.pix);
}

void mark_dirty()
{
    dirty = true;
    set_title();
    show_caret();
}

void insert_char(char ch)
{
    clamp_caret();
    lines[row].insert((size_t)col, 1, ch);
    col++;
    mark_dirty();
}

void newline()
{
    clamp_caret();
    std::string rest = lines[row].substr((size_t)col);
    lines[row].resize((size_t)col);
    lines.insert(lines.begin() + row + 1, rest);
    row++;
    col = 0;
    mark_dirty();
}

void backspace()
{
    clamp_caret();
    if (col > 0) {
        lines[row].erase((size_t)col - 1, 1);
        col--;
        mark_dirty();
    } else if (row > 0) {
        col = (int)lines[row - 1].size();
        lines[row - 1] += lines[row];
        lines.erase(lines.begin() + row);
        row--;
        mark_dirty();
    }
}

void delete_fwd()
{
    clamp_caret();
    if (col < (int)lines[row].size()) {
        lines[row].erase((size_t)col, 1);
        mark_dirty();
    } else if (row + 1 < (int)lines.size()) {
        lines[row] += lines[row + 1];
        lines.erase(lines.begin() + row + 1);
        mark_dirty();
    }
}

void move_left()
{
    clamp_caret();
    if (col > 0)
        col--;
    else if (row > 0) {
        row--;
        col = (int)lines[row].size();
    }
    show_caret();
}

void move_right()
{
    clamp_caret();
    if (col < (int)lines[row].size())
        col++;
    else if (row + 1 < (int)lines.size()) {
        row++;
        col = 0;
    }
    show_caret();
}

void move_up()
{
    clamp_caret();
    if (row > 0) {
        row--;
        col = std::min(col, (int)lines[row].size());
    }
    show_caret();
}

void move_down()
{
    clamp_caret();
    if (row + 1 < (int)lines.size()) {
        row++;
        col = std::min(col, (int)lines[row].size());
    }
    show_caret();
}

void do_new()
{
    reset_doc();
    path.clear();
    status = "New document";
    set_title();
    show_caret();
}

void start_open()
{
    prompt = Prompt::Open;
    pbuf = path.empty() ? std::string(getenv("HOME") ? getenv("HOME") : "/") : path;
}

void start_save_as()
{
    prompt = Prompt::SaveAs;
    pbuf = path.empty() ? "untitled.txt" : path;
}

void do_save()
{
    if (path.empty())
        start_save_as();
    else
        save_file(path);
}

void on_tool(int x, int y)
{
    if (y < kMenuH || y >= kMenuH + kToolH)
        return;
    auto hit = [&](int bx, int bw) { return x >= bx && x < bx + bw; };
    if (hit(6, 40))
        do_new();
    else if (hit(50, 48))
        start_open();
    else if (hit(102, 48))
        do_save();
    else if (hit(154, 64))
        start_save_as();
}

void click_text(int x, int y)
{
    int ly = text_top();
    int rh = row_h();
    int r = scroll + (y - ly - 2) / rh;
    r = std::clamp(r, 0, (int)lines.size() - 1);
    row = r;
    int px = x - text_left() + scroll_x;
    const std::string &s = lines[row];
    int best = (int)s.size();
    for (int i = 0; i <= (int)s.size(); i++) {
        if (text_w(s.c_str(), i) >= px) {
            best = i;
            break;
        }
    }
    col = best;
    show_caret();
}

void on_key(XKeyEvent *e)
{
    KeySym ks;
    char buf[16];
    int n = XLookupString(e, buf, sizeof buf, &ks, nullptr);

    if (prompt != Prompt::Idle) {
        if (ks == XK_Escape) {
            prompt = Prompt::Idle;
            status = "Cancelled";
            return;
        }
        if (ks == XK_Return) {
            if (prompt == Prompt::Open)
                load_file(pbuf);
            else
                save_file(pbuf);
            prompt = Prompt::Idle;
            return;
        }
        if (ks == XK_BackSpace) {
            if (!pbuf.empty())
                pbuf.pop_back();
            return;
        }
        if (n == 1 && buf[0] >= 32 && buf[0] < 127)
            pbuf.push_back(buf[0]);
        return;
    }

    if ((e->state & ControlMask) && (ks == XK_s || ks == XK_S)) {
        do_save();
        return;
    }
    if ((e->state & ControlMask) && (ks == XK_o || ks == XK_O)) {
        start_open();
        return;
    }
    if ((e->state & ControlMask) && (ks == XK_n || ks == XK_N)) {
        do_new();
        return;
    }

    if (ks == XK_Left)
        move_left();
    else if (ks == XK_Right)
        move_right();
    else if (ks == XK_Up)
        move_up();
    else if (ks == XK_Down)
        move_down();
    else if (ks == XK_Home) {
        if (e->state & ControlMask)
            row = 0;
        col = 0;
        show_caret();
    }
    else if (ks == XK_End) {
        if (e->state & ControlMask) {
            row = (int)lines.size() - 1;
            col = (int)lines[row].size();
        } else
            col = (int)lines[row].size();
        show_caret();
    } else if (ks == XK_Page_Up) {
        row = std::max(0, row - rows_vis());
        show_caret();
    } else if (ks == XK_Page_Down) {
        row = std::min((int)lines.size() - 1, row + rows_vis());
        show_caret();
    } else if (ks == XK_Return)
        newline();
    else if (ks == XK_BackSpace)
        backspace();
    else if (ks == XK_Delete)
        delete_fwd();
    else if (ks == XK_Tab) {
        insert_char(' ');
        insert_char(' ');
        insert_char(' ');
        insert_char(' ');
    } else if (n == 1 && buf[0] >= 32 && buf[0] < 127)
        insert_char(buf[0]);
}

} // namespace

int main(int argc, char **argv)
{
    signal(SIGCHLD, SIG_IGN);
    dpy = XOpenDisplay(nullptr);
    if (!dpy) {
        std::fprintf(stderr, "editor: no display\n");
        return 1;
    }
    screen = DefaultScreen(dpy);
    face.pix = alloc_rgb(w95::rgb_face);
    hi.pix = alloc_rgb(w95::rgb_hi);
    lo.pix = alloc_rgb(w95::rgb_lo);
    dk.pix = alloc_rgb(w95::rgb_dk);
    title.pix = alloc_rgb(w95::rgb_title);
    fg.pix = alloc_rgb(w95::rgb_text);
    white.pix = alloc_rgb(w95::rgb_white);
    field.pix = alloc_rgb(w95::rgb_field);

    XSetWindowAttributes swa{};
    swa.background_pixel = face.pix;
    swa.event_mask = ExposureMask | KeyPressMask | ButtonPressMask | StructureNotifyMask;
    win = XCreateWindow(dpy, RootWindow(dpy, screen), 100, 80, kWinW, kWinH, 0, CopyFromParent, InputOutput,
                        CopyFromParent, CWBackPixel | CWEventMask, &swa);
    gc = XCreateGC(dpy, win, 0, nullptr);
    const char *fn[] = {"-*-helvetica-medium-r-*-*-12-*-*-*-*-*-*-*", "fixed", nullptr};
    for (int i = 0; fn[i] && !font; i++)
        font = XLoadQueryFont(dpy, fn[i]);
    if (font)
        XSetFont(dpy, gc, font->fid);

    wm_protocols = XInternAtom(dpy, "WM_PROTOCOLS", False);
    wm_delete = XInternAtom(dpy, "WM_DELETE_WINDOW", False);
    XSetWMProtocols(dpy, win, &wm_delete, 1);
    XClassHint ch{};
    ch.res_name = (char *)"editor";
    ch.res_class = (char *)"Editor";
    XSetClassHint(dpy, win, &ch);

    reset_doc();
    if (argc >= 2)
        load_file(argv[1]);
    set_title();

    XMapWindow(dpy, win);
    last_blink = now_ms();
    int fd = ConnectionNumber(dpy);
    for (;;) {
        while (XPending(dpy)) {
            XEvent e;
            XNextEvent(dpy, &e);
            if (e.type == Expose && e.xexpose.count == 0)
                redraw();
            else if (e.type == ConfigureNotify) {
                ww = e.xconfigure.width;
                wh = e.xconfigure.height;
                redraw();
            } else if (e.type == ButtonPress) {
                if (e.xbutton.button == 4) {
                    scroll = std::max(0, scroll - 3);
                    redraw();
                } else if (e.xbutton.button == 5) {
                    int maxs = std::max(0, (int)lines.size() - rows_vis());
                    scroll = std::min(maxs, scroll + 3);
                    redraw();
                } else if (e.xbutton.button == 1) {
                    int y = e.xbutton.y;
                    if (y < kMenuH + kToolH)
                        on_tool(e.xbutton.x, y);
                    else if (y >= text_top() && y < text_top() + text_h())
                        click_text(e.xbutton.x, y);
                    redraw();
                }
            } else if (e.type == KeyPress) {
                on_key(&e.xkey);
                redraw();
            } else if (e.type == ClientMessage && (Atom)e.xclient.data.l[0] == wm_delete)
                return 0;
        }
        long t = now_ms();
        if (t - last_blink >= kBlinkMs) {
            last_blink = t;
            caret_on = !caret_on;
            redraw();
        }
        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(fd, &fds);
        struct timeval tv;
        tv.tv_sec = 0;
        tv.tv_usec = 50000;
        select(fd + 1, &fds, nullptr, nullptr, &tv);
    }
}
