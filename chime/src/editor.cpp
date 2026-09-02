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
#include <cstdlib>
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
constexpr int kItemH = 18;
constexpr long kBlinkMs = 530;

enum {
    ID_NONE = 0,
    ID_NEW,
    ID_OPEN,
    ID_SAVE,
    ID_SAVEAS,
    ID_EXIT,
    ID_CUT,
    ID_COPY,
    ID_PASTE,
    ID_DELETE,
    ID_SELALL,
    ID_ABOUT,
    ID_ABOUT_OK
};

struct Col {
    unsigned long pix;
};

struct DropItem {
    const char *lab;   // nullptr = separator
    const char *accel;
    int id;
};

struct ToolBtn {
    int x, w;
    const char *lab;
    int id;
};

struct TopMenu {
    const char *lab;
    const DropItem *items;
    int n;
    int x = 0, w = 0;
};

Display *dpy;
int screen;
Window win;
GC gc;
XFontStruct *font;
Col face, hi, lo, dk, title, fg, white, field;
int ww = kWinW, wh = kWinH;
Atom wm_delete, wm_protocols, atom_clip, atom_utf8, atom_targets;
bool running = true;

std::vector<std::string> lines;
int row = 0, col = 0;
int scroll = 0, scroll_x = 0;
int mark_r = -1, mark_c = 0;
bool dirty = false;
bool caret_on = true;
long last_blink = 0;
std::string path;
std::string status = "Ready";
std::string clip_local;

enum class Prompt { Idle, Open, SaveAs };
Prompt prompt = Prompt::Idle;
std::string pbuf;

int tool_press = -1;
bool tool_in = false;
int menu_open = -1;
int menu_hover = -1;
int pop_x = 0, pop_y = 0, pop_w = 0, pop_h = 0;
bool about = false;
int about_press = 0;
bool dragging_sel = false;

constexpr ToolBtn kTools[] = {
    {6, 40, "New", ID_NEW},
    {50, 48, "Open", ID_OPEN},
    {102, 48, "Save", ID_SAVE},
    {154, 64, "Save As", ID_SAVEAS},
};
constexpr int kToolN = (int)(sizeof(kTools) / sizeof(kTools[0]));

constexpr DropItem kFileMenu[] = {
    {"New", "Ctrl+N", ID_NEW},
    {"Open...", "Ctrl+O", ID_OPEN},
    {"Save", "Ctrl+S", ID_SAVE},
    {"Save As...", nullptr, ID_SAVEAS},
    {nullptr, nullptr, 0},
    {"Exit", nullptr, ID_EXIT},
};
constexpr DropItem kEditMenu[] = {
    {"Cut", "Ctrl+X", ID_CUT},
    {"Copy", "Ctrl+C", ID_COPY},
    {"Paste", "Ctrl+V", ID_PASTE},
    {nullptr, nullptr, 0},
    {"Delete", "Del", ID_DELETE},
    {"Select All", "Ctrl+A", ID_SELALL},
};
constexpr DropItem kHelpMenu[] = {
    {"About Editor...", nullptr, ID_ABOUT},
};

TopMenu tops[] = {
    {"File", kFileMenu, (int)(sizeof(kFileMenu) / sizeof(kFileMenu[0]))},
    {"Edit", kEditMenu, (int)(sizeof(kEditMenu) / sizeof(kEditMenu[0]))},
    {"Help", kHelpMenu, (int)(sizeof(kHelpMenu) / sizeof(kHelpMenu[0]))},
};
constexpr int kTopN = (int)(sizeof(tops) / sizeof(tops[0]));

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
    if (w <= 0 || h <= 0)
        return;
    unsigned long tl = raised ? hi.pix : lo.pix;
    unsigned long br = raised ? dk.pix : hi.pix;
    unsigned long tl2 = raised ? face.pix : dk.pix;
    unsigned long br2 = raised ? lo.pix : face.pix;
    XSetForeground(dpy, gc, tl);
    XDrawLine(dpy, win, gc, x, y, x + w - 1, y);
    XDrawLine(dpy, win, gc, x, y, x, y + h - 1);
    XSetForeground(dpy, gc, br);
    XDrawLine(dpy, win, gc, x, y + h - 1, x + w - 1, y + h - 1);
    XDrawLine(dpy, win, gc, x + w - 1, y, x + w - 1, y + h - 1);
    if (w < 4 || h < 4)
        return;
    XSetForeground(dpy, gc, tl2);
    XDrawLine(dpy, win, gc, x + 1, y + 1, x + w - 3, y + 1);
    XDrawLine(dpy, win, gc, x + 1, y + 1, x + 1, y + h - 3);
    XSetForeground(dpy, gc, br2);
    XDrawLine(dpy, win, gc, x + 1, y + h - 2, x + w - 2, y + h - 2);
    XDrawLine(dpy, win, gc, x + w - 2, y + 1, x + w - 2, y + h - 2);
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

void btn(int x, int y, int w, int h, const char *lab, bool down)
{
    fill(x, y, w, h, face.pix);
    bevel(x, y, w, h, !down);
    int ox = down ? 1 : 0;
    int tw = text_w(lab);
    draw_str(x + (w - tw) / 2 + ox, y + ox, h, lab, fg.pix);
}

bool has_sel()
{
    return mark_r >= 0 && (mark_r != row || mark_c != col);
}

void clear_sel() { mark_r = -1; }

void order_sel(int &r0, int &c0, int &r1, int &c1)
{
    r0 = mark_r;
    c0 = mark_c;
    r1 = row;
    c1 = col;
    if (r0 > r1 || (r0 == r1 && c0 > c1)) {
        std::swap(r0, r1);
        std::swap(c0, c1);
    }
}

bool cmd_on(int id)
{
    if (id == ID_CUT || id == ID_COPY || id == ID_DELETE)
        return has_sel();
    return true;
}

int tool_y() { return kMenuH + 3; }
int tool_bh() { return 20; }

int hit_tool(int x, int y)
{
    if (y < tool_y() || y >= tool_y() + tool_bh())
        return -1;
    for (int i = 0; i < kToolN; i++)
        if (x >= kTools[i].x && x < kTools[i].x + kTools[i].w)
            return i;
    return -1;
}

void layout_menus()
{
    int x = 2;
    for (int i = 0; i < kTopN; i++) {
        tops[i].x = x;
        tops[i].w = text_w(tops[i].lab) + 16;
        x += tops[i].w;
    }
}

int hit_top(int x, int y)
{
    if (y < 0 || y >= kMenuH)
        return -1;
    for (int i = 0; i < kTopN; i++)
        if (x >= tops[i].x && x < tops[i].x + tops[i].w)
            return i;
    return -1;
}

void popup_metrics(const DropItem *it, int n, int &w, int &h)
{
    w = 140;
    h = 6;
    for (int i = 0; i < n; i++) {
        if (!it[i].lab) {
            h += 8;
            continue;
        }
        int tw = text_w(it[i].lab) + 28;
        if (it[i].accel)
            tw += text_w(it[i].accel) + 18;
        w = std::max(w, tw);
        h += kItemH;
    }
}

int popup_index(int x, int y)
{
    if (menu_open < 0)
        return -1;
    if (x < pop_x || y < pop_y || x >= pop_x + pop_w || y >= pop_y + pop_h)
        return -1;
    const DropItem *it = tops[menu_open].items;
    int n = tops[menu_open].n;
    int iy = pop_y + 3;
    for (int i = 0; i < n; i++) {
        if (!it[i].lab) {
            iy += 8;
            continue;
        }
        if (y >= iy && y < iy + kItemH)
            return i;
        iy += kItemH;
    }
    return -1;
}

void close_menu()
{
    menu_open = -1;
    menu_hover = -1;
}

void open_menu(int i)
{
    if (i < 0 || i >= kTopN)
        return;
    layout_menus();
    menu_open = i;
    popup_metrics(tops[i].items, tops[i].n, pop_w, pop_h);
    pop_x = tops[i].x;
    pop_y = kMenuH;
    if (pop_x + pop_w > ww - 2)
        pop_x = std::max(2, ww - 2 - pop_w);
    menu_hover = -1;
    for (int k = 0; k < tops[i].n; k++) {
        if (tops[i].items[k].lab && cmd_on(tops[i].items[k].id)) {
            menu_hover = k;
            break;
        }
    }
}

void about_box(int &x, int &y, int &w, int &h)
{
    w = 300;
    h = 132;
    x = std::max(8, (ww - w) / 2);
    y = std::max(8, (wh - h) / 2);
}

void about_ok(int &x, int &y, int &w, int &h)
{
    int bx, by, bw, bh;
    about_box(bx, by, bw, bh);
    w = w95::kDlgBtnW;
    h = w95::kDlgBtnH;
    x = bx + bw - 12 - w;
    y = by + bh - 12 - h;
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
    clear_sel();
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
    clear_sel();
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

void draw_popup()
{
    if (menu_open < 0)
        return;
    const DropItem *it = tops[menu_open].items;
    int n = tops[menu_open].n;
    fill(pop_x, pop_y, pop_w, pop_h, face.pix);
    bevel(pop_x, pop_y, pop_w, pop_h, true);
    int y = pop_y + 3;
    for (int i = 0; i < n; i++) {
        if (!it[i].lab) {
            int gy = y + 3;
            fill(pop_x + 4, gy, pop_w - 8, 1, lo.pix);
            fill(pop_x + 4, gy + 1, pop_w - 8, 1, hi.pix);
            y += 8;
            continue;
        }
        bool hot = (menu_hover == i);
        bool on = cmd_on(it[i].id);
        if (hot && on)
            fill(pop_x + 2, y, pop_w - 4, kItemH, title.pix);
        unsigned long col = !on ? lo.pix : (hot ? white.pix : fg.pix);
        draw_str(pop_x + 12, y, kItemH, it[i].lab, col);
        if (it[i].accel) {
            int aw = text_w(it[i].accel);
            draw_str(pop_x + pop_w - 10 - aw, y, kItemH, it[i].accel, col);
        }
        y += kItemH;
    }
}

void draw_about()
{
    int x, y, w, h;
    about_box(x, y, w, h);
    fill(x + 3, y + 3, w, h, lo.pix);
    fill(x, y, w, h, face.pix);
    bevel(x, y, w, h, true);
    fill(x + 3, y + 3, w - 6, w95::kTitleH, title.pix);
    draw_str(x + 8, y + 3, w95::kTitleH, "About Editor", white.pix);
    int cx = x + w - 6 - w95::kBtn;
    fill(cx, y + 5, w95::kBtn, w95::kBtnH, face.pix);
    bevel(cx, y + 5, w95::kBtn, w95::kBtnH, true);
    XSetForeground(dpy, gc, fg.pix);
    XDrawLine(dpy, win, gc, cx + 4, y + 8, cx + w95::kBtn - 5, y + 5 + w95::kBtnH - 4);
    XDrawLine(dpy, win, gc, cx + w95::kBtn - 5, y + 8, cx + 4, y + 5 + w95::kBtnH - 4);
    draw_str(x + 16, y + 32, 18, "Chime Editor", fg.pix);
    draw_str(x + 16, y + 50, 16, "A Notepad for the Chime desktop.", fg.pix);
    int ox, oy, ow, oh;
    about_ok(ox, oy, ow, oh);
    btn(ox, oy, ow, oh, "OK", about_press == ID_ABOUT_OK);
}

void draw_line_text(int tx, int y, int rh, const std::string &s, int idx)
{
    if (!has_sel()) {
        draw_str(tx, y, rh, s.c_str(), fg.pix);
        return;
    }
    int r0, c0, r1, c1;
    order_sel(r0, c0, r1, c1);
    if (idx < r0 || idx > r1) {
        draw_str(tx, y, rh, s.c_str(), fg.pix);
        return;
    }
    int a = (idx == r0) ? c0 : 0;
    int b = (idx == r1) ? c1 : (int)s.size();
    a = std::clamp(a, 0, (int)s.size());
    b = std::clamp(b, 0, (int)s.size());
    if (a > b)
        std::swap(a, b);
    int x0 = tx + text_w(s.c_str(), a);
    int x1 = tx + text_w(s.c_str(), b);
    if (x1 > x0)
        fill(x0, y, x1 - x0, rh, title.pix);
    if (a > 0)
        draw_str(tx, y, rh, s.c_str(), fg.pix, a);
    if (b > a)
        draw_str(x0, y, rh, s.c_str() + a, white.pix, b - a);
    if (b < (int)s.size())
        draw_str(x1, y, rh, s.c_str() + b, fg.pix);
}

void redraw()
{
    fill(0, 0, ww, wh, face.pix);
    layout_menus();
    for (int i = 0; i < kTopN; i++) {
        bool on = (menu_open == i);
        if (on) {
            fill(tops[i].x, 1, tops[i].w, kMenuH - 2, face.pix);
            bevel(tops[i].x, 1, tops[i].w, kMenuH - 2, false);
        }
        int ox = on ? 1 : 0;
        draw_str(tops[i].x + 8 + ox, 1 + ox, kMenuH - 2, tops[i].lab, fg.pix);
    }

    int ty = tool_y();
    for (int i = 0; i < kToolN; i++)
        btn(kTools[i].x, ty, kTools[i].w, tool_bh(), kTools[i].lab, tool_press == i && tool_in);

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
        draw_line_text(tx, y, rh, s, idx);
        if (idx == row && caret_on && prompt == Prompt::Idle && !has_sel() && !about && menu_open < 0) {
            int cx = tx + text_w(s.c_str(), col);
            int ch = font ? font->ascent + font->descent : rh - 4;
            int cy = y + std::max(0, (rh - ch) / 2);
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

    draw_popup();
    if (about)
        draw_about();
}

void mark_dirty()
{
    dirty = true;
    set_title();
    show_caret();
}

std::string sel_text()
{
    if (!has_sel())
        return {};
    int r0, c0, r1, c1;
    order_sel(r0, c0, r1, c1);
    if (r0 == r1)
        return lines[r0].substr((size_t)c0, (size_t)(c1 - c0));
    std::string s = lines[r0].substr((size_t)c0) + "\n";
    for (int i = r0 + 1; i < r1; i++)
        s += lines[i] + "\n";
    s += lines[r1].substr(0, (size_t)c1);
    return s;
}

void del_sel()
{
    if (!has_sel())
        return;
    int r0, c0, r1, c1;
    order_sel(r0, c0, r1, c1);
    if (r0 == r1)
        lines[r0].erase((size_t)c0, (size_t)(c1 - c0));
    else {
        lines[r0] = lines[r0].substr(0, (size_t)c0) + lines[r1].substr((size_t)c1);
        lines.erase(lines.begin() + r0 + 1, lines.begin() + r1 + 1);
    }
    row = r0;
    col = c0;
    clear_sel();
    mark_dirty();
}

void insert_char(char ch)
{
    if (has_sel())
        del_sel();
    clamp_caret();
    lines[row].insert((size_t)col, 1, ch);
    col++;
    mark_dirty();
}

void newline()
{
    if (has_sel())
        del_sel();
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
    if (has_sel()) {
        del_sel();
        return;
    }
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
    if (has_sel()) {
        del_sel();
        return;
    }
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

void keep_or_clear_sel(unsigned state)
{
    if (state & ShiftMask) {
        if (mark_r < 0) {
            mark_r = row;
            mark_c = col;
        }
    } else
        clear_sel();
}

void move_left(unsigned state)
{
    keep_or_clear_sel(state);
    clamp_caret();
    if (col > 0)
        col--;
    else if (row > 0) {
        row--;
        col = (int)lines[row].size();
    }
    show_caret();
}

void move_right(unsigned state)
{
    keep_or_clear_sel(state);
    clamp_caret();
    if (col < (int)lines[row].size())
        col++;
    else if (row + 1 < (int)lines.size()) {
        row++;
        col = 0;
    }
    show_caret();
}

void move_up(unsigned state)
{
    keep_or_clear_sel(state);
    clamp_caret();
    if (row > 0) {
        row--;
        col = std::min(col, (int)lines[row].size());
    }
    show_caret();
}

void move_down(unsigned state)
{
    keep_or_clear_sel(state);
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
    prompt = Prompt::Idle;
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

void own_clip()
{
    XSetSelectionOwner(dpy, atom_clip, win, CurrentTime);
    XSetSelectionOwner(dpy, XA_PRIMARY, win, CurrentTime);
}

void do_copy()
{
    if (!has_sel()) {
        status = "Nothing to copy";
        return;
    }
    clip_local = sel_text();
    own_clip();
    status = "Copied";
}

void do_cut()
{
    if (!has_sel()) {
        status = "Nothing to cut";
        return;
    }
    clip_local = sel_text();
    own_clip();
    del_sel();
    status = "Cut";
}

void insert_text(const std::string &t)
{
    if (has_sel())
        del_sel();
    for (char ch : t) {
        if (ch == '\r')
            continue;
        if (ch == '\n')
            newline();
        else
            insert_char(ch);
    }
}

void do_paste()
{
    Window owner = XGetSelectionOwner(dpy, atom_clip);
    Atom sel = atom_clip;
    if (owner == None) {
        owner = XGetSelectionOwner(dpy, XA_PRIMARY);
        sel = XA_PRIMARY;
    }
    if (owner == None || owner == win) {
        if (!clip_local.empty())
            insert_text(clip_local);
        else
            status = "Clipboard empty";
        return;
    }
    XConvertSelection(dpy, sel, XA_STRING, XA_STRING, win, CurrentTime);
}

void do_selall()
{
    if (lines.empty())
        reset_doc();
    mark_r = 0;
    mark_c = 0;
    row = (int)lines.size() - 1;
    col = (int)lines[row].size();
    show_caret();
}

void do_cmd(int id)
{
    close_menu();
    switch (id) {
    case ID_NEW:
        do_new();
        break;
    case ID_OPEN:
        start_open();
        break;
    case ID_SAVE:
        do_save();
        break;
    case ID_SAVEAS:
        start_save_as();
        break;
    case ID_EXIT:
        running = false;
        break;
    case ID_CUT:
        do_cut();
        break;
    case ID_COPY:
        do_copy();
        break;
    case ID_PASTE:
        do_paste();
        break;
    case ID_DELETE:
        delete_fwd();
        break;
    case ID_SELALL:
        do_selall();
        break;
    case ID_ABOUT:
        about = true;
        about_press = 0;
        break;
    default:
        break;
    }
}

void click_text(int x, int y, unsigned state, bool press)
{
    int ly = text_top();
    int rh = row_h();
    int r = scroll + (y - ly - 2) / rh;
    r = std::clamp(r, 0, (int)lines.size() - 1);
    if (press) {
        if (state & ShiftMask) {
            if (mark_r < 0) {
                mark_r = row;
                mark_c = col;
            }
        } else {
            mark_r = r;
            mark_c = 0;
        }
        dragging_sel = true;
    }
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
    if (press && !(state & ShiftMask))
        mark_c = col;
    show_caret();
}

int first_enabled(int mi, int from, int dir)
{
    const DropItem *it = tops[mi].items;
    int n = tops[mi].n;
    int i = from;
    for (int k = 0; k < n + 1; k++) {
        i = (i + dir + n) % n;
        if (it[i].lab && cmd_on(it[i].id))
            return i;
    }
    return from;
}

void on_menu_key(KeySym ks)
{
    if (ks == XK_Escape) {
        close_menu();
        return;
    }
    if (ks == XK_Left)
        open_menu((menu_open + kTopN - 1) % kTopN);
    else if (ks == XK_Right)
        open_menu((menu_open + 1) % kTopN);
    else if (ks == XK_Down)
        menu_hover = first_enabled(menu_open, menu_hover, 1);
    else if (ks == XK_Up)
        menu_hover = first_enabled(menu_open, menu_hover, -1);
    else if (ks == XK_Return || ks == XK_space) {
        if (menu_hover >= 0 && menu_hover < tops[menu_open].n) {
            const DropItem &it = tops[menu_open].items[menu_hover];
            if (it.lab && cmd_on(it.id))
                do_cmd(it.id);
        }
    }
}

void on_key(XKeyEvent *e)
{
    KeySym ks;
    char buf[16];
    int n = XLookupString(e, buf, sizeof buf, &ks, nullptr);

    if (about) {
        if (ks == XK_Escape || ks == XK_Return || ks == XK_space)
            about = false;
        return;
    }
    if (menu_open >= 0) {
        on_menu_key(ks);
        return;
    }

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
    if ((e->state & ControlMask) && (ks == XK_a || ks == XK_A)) {
        do_selall();
        return;
    }
    if ((e->state & ControlMask) && (ks == XK_c || ks == XK_C)) {
        do_copy();
        return;
    }
    if ((e->state & ControlMask) && (ks == XK_x || ks == XK_X)) {
        do_cut();
        return;
    }
    if ((e->state & ControlMask) && (ks == XK_v || ks == XK_V)) {
        do_paste();
        return;
    }

    if (ks == XK_Left)
        move_left(e->state);
    else if (ks == XK_Right)
        move_right(e->state);
    else if (ks == XK_Up)
        move_up(e->state);
    else if (ks == XK_Down)
        move_down(e->state);
    else if (ks == XK_Home) {
        keep_or_clear_sel(e->state);
        if (e->state & ControlMask)
            row = 0;
        col = 0;
        show_caret();
    } else if (ks == XK_End) {
        keep_or_clear_sel(e->state);
        if (e->state & ControlMask) {
            row = (int)lines.size() - 1;
            col = (int)lines[row].size();
        } else
            col = (int)lines[row].size();
        show_caret();
    } else if (ks == XK_Page_Up) {
        keep_or_clear_sel(e->state);
        row = std::max(0, row - rows_vis());
        show_caret();
    } else if (ks == XK_Page_Down) {
        keep_or_clear_sel(e->state);
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

void on_sel_req(XSelectionRequestEvent *req)
{
    XSelectionEvent ev{};
    ev.type = SelectionNotify;
    ev.display = req->display;
    ev.requestor = req->requestor;
    ev.selection = req->selection;
    ev.target = req->target;
    ev.time = req->time;
    ev.property = req->property;
    if (req->property == None)
        ev.property = req->target;
    if (req->target == atom_targets) {
        Atom ts[] = {XA_STRING, atom_utf8};
        XChangeProperty(dpy, req->requestor, ev.property, XA_ATOM, 32, PropModeReplace, (unsigned char *)ts, 2);
    } else if (req->target == XA_STRING || req->target == atom_utf8) {
        XChangeProperty(dpy, req->requestor, ev.property, req->target, 8, PropModeReplace,
                        (unsigned char *)clip_local.data(), (int)clip_local.size());
    } else
        ev.property = None;
    XSendEvent(dpy, req->requestor, False, 0, (XEvent *)&ev);
}

void on_sel_notify(XSelectionEvent *e)
{
    if (e->property == None) {
        if (!clip_local.empty())
            insert_text(clip_local);
        else
            status = "Clipboard empty";
        return;
    }
    Atom type = None;
    int fmt = 0;
    unsigned long n = 0, extra = 0;
    unsigned char *data = nullptr;
    if (XGetWindowProperty(dpy, win, e->property, 0, 1024 * 1024, True, AnyPropertyType, &type, &fmt, &n, &extra,
                           &data) == Success &&
        data) {
        insert_text(std::string((char *)data, n));
        XFree(data);
    }
}

bool about_close_hit(int x, int y)
{
    int bx, by, bw, bh;
    about_box(bx, by, bw, bh);
    int cx = bx + bw - 6 - w95::kBtn, cy = by + 5;
    return x >= cx && y >= cy && x < cx + w95::kBtn && y < cy + w95::kBtnH;
}

void on_press(XButtonEvent *b)
{
    if (b->button == 4) {
        if (about || menu_open >= 0)
            return;
        scroll = std::max(0, scroll - 3);
        return;
    }
    if (b->button == 5) {
        if (about || menu_open >= 0)
            return;
        int maxs = std::max(0, (int)lines.size() - rows_vis());
        scroll = std::min(maxs, scroll + 3);
        return;
    }
    if (b->button != 1)
        return;

    if (about) {
        int ox, oy, ow, oh;
        about_ok(ox, oy, ow, oh);
        if (b->x >= ox && b->y >= oy && b->x < ox + ow && b->y < oy + oh)
            about_press = ID_ABOUT_OK;
        else if (about_close_hit(b->x, b->y))
            about = false;
        return;
    }

    if (menu_open >= 0) {
        int hi = popup_index(b->x, b->y);
        if (hi >= 0) {
            menu_hover = hi;
            return;
        }
        int t = hit_top(b->x, b->y);
        if (t >= 0) {
            if (t == menu_open)
                close_menu();
            else
                open_menu(t);
            return;
        }
        close_menu();
        return;
    }

    int t = hit_top(b->x, b->y);
    if (t >= 0) {
        open_menu(t);
        return;
    }

    int ti = hit_tool(b->x, b->y);
    if (ti >= 0) {
        tool_press = ti;
        tool_in = true;
        return;
    }

    if (b->y >= text_top() && b->y < text_top() + text_h())
        click_text(b->x, b->y, b->state, true);
}

void on_release(XButtonEvent *b)
{
    if (b->button != 1)
        return;
    if (about) {
        if (about_press == ID_ABOUT_OK) {
            int ox, oy, ow, oh;
            about_ok(ox, oy, ow, oh);
            if (b->x >= ox && b->y >= oy && b->x < ox + ow && b->y < oy + oh)
                about = false;
        }
        about_press = 0;
        return;
    }
    if (menu_open >= 0) {
        int hi = popup_index(b->x, b->y);
        if (hi >= 0) {
            const DropItem &it = tops[menu_open].items[hi];
            if (it.lab && cmd_on(it.id))
                do_cmd(it.id);
        }
        return;
    }
    if (tool_press >= 0) {
        int ti = hit_tool(b->x, b->y);
        int id = (ti == tool_press && tool_in) ? kTools[tool_press].id : 0;
        tool_press = -1;
        tool_in = false;
        if (id)
            do_cmd(id);
        return;
    }
    if (dragging_sel) {
        dragging_sel = false;
        if (mark_r == row && mark_c == col)
            clear_sel();
    }
}

void on_motion(XMotionEvent *m)
{
    if (about) {
        if (about_press == ID_ABOUT_OK) {
            int ox, oy, ow, oh;
            about_ok(ox, oy, ow, oh);
            bool in = m->x >= ox && m->y >= oy && m->x < ox + ow && m->y < oy + oh;
            about_press = in ? ID_ABOUT_OK : -ID_ABOUT_OK;
        }
        return;
    }
    if (menu_open >= 0) {
        int t = hit_top(m->x, m->y);
        if (t >= 0 && t != menu_open)
            open_menu(t);
        else
            menu_hover = popup_index(m->x, m->y);
        return;
    }
    if (tool_press >= 0) {
        tool_in = hit_tool(m->x, m->y) == tool_press;
        return;
    }
    if (dragging_sel && (m->state & Button1Mask))
        click_text(m->x, m->y, ShiftMask, false);
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
    swa.event_mask = ExposureMask | KeyPressMask | ButtonPressMask | ButtonReleaseMask | ButtonMotionMask |
                     PointerMotionMask | StructureNotifyMask | LeaveWindowMask;
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
    atom_clip = XInternAtom(dpy, "CLIPBOARD", False);
    atom_utf8 = XInternAtom(dpy, "UTF8_STRING", False);
    atom_targets = XInternAtom(dpy, "TARGETS", False);
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
    while (running) {
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
                on_press(&e.xbutton);
                redraw();
            } else if (e.type == ButtonRelease) {
                on_release(&e.xbutton);
                redraw();
            } else if (e.type == MotionNotify) {
                if (about || menu_open >= 0 || tool_press >= 0 || dragging_sel) {
                    on_motion(&e.xmotion);
                    redraw();
                }
            } else if (e.type == LeaveNotify) {
                if (tool_press >= 0 && tool_in) {
                    tool_in = false;
                    redraw();
                }
            } else if (e.type == KeyPress) {
                on_key(&e.xkey);
                redraw();
            } else if (e.type == SelectionRequest)
                on_sel_req(&e.xselectionrequest);
            else if (e.type == SelectionNotify) {
                on_sel_notify(&e.xselection);
                redraw();
            } else if (e.type == ClientMessage && (Atom)e.xclient.data.l[0] == wm_delete)
                running = false;
        }
        if (!running)
            break;
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
    XCloseDisplay(dpy);
    return 0;
}
