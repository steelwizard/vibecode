// Cabinet — Chime's Explorer-style file manager.
// Menu bar, toolbar, address well, details list, and status line. Directory IO
// is POSIX; opening a file shells out to editor or vi. Toolbar hit boxes live
// in kTools[] and must match the painted buttons.

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
#include <ctime>
#include <cerrno>
#include <dirent.h>
#include <string>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

namespace {

constexpr int kWinW = 640;
constexpr int kWinH = 420;
constexpr int kMenuH = 20;
constexpr int kToolH = 26;
constexpr int kAddrH = 22;
constexpr int kHeadH = 18;  // Name / Size / Type column headers
constexpr int kStatH = 20;
constexpr int kRowH = 16;
constexpr int kItemH = 18;

enum {
    ID_NONE = 0,
    ID_UP,
    ID_COMPUTER,
    ID_HOME,
    ID_OPEN,
    ID_MKDIR,
    ID_DEL,
    ID_RENAME,
    ID_CLOSE,
    ID_SELALL,
    ID_REFRESH,
    ID_HIDDEN,
    ID_ABOUT,
    ID_ABOUT_OK
};

struct DropItem {
    const char *lab;
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

struct Col {
    unsigned long pix;
};

struct Entry {
    std::string name;
    std::string path;
    bool is_dir = false;
    bool is_parent = false; // The synthetic ".." row
    off_t size = 0;
    time_t mtime = 0;
};

// Status-line modal: Y/N confirm, or type a name and Return.
enum class Prompt { Idle, Del, Rename, Mkdir };

Display *dpy;
int screen;
Window win;
GC gc;
XFontStruct *font;
Col face, hi, lo, dk, title, fg, white, field, yellow;
int ww = kWinW, wh = kWinH;
std::vector<Entry> ents;
int sel = 0, scroll = 0;
int mark_anchor = 0;
std::vector<char> marked;
bool dragging = false;
int drag_x0 = 0, drag_y0 = 0, drag_x1 = 0, drag_y1 = 0;
std::vector<char> drag_saved;
std::string cwd;
Prompt prompt = Prompt::Idle;
std::string pbuf, status;
Time last_click = 0;
int last_row = -1;
Atom wm_delete, wm_protocols;
bool running = true;
bool show_hidden = false;
int tool_press = -1;
bool tool_in = false;
int menu_open = -1;
bool ctx_open = false;
int menu_hover = -1;
int pop_x = 0, pop_y = 0, pop_w = 0, pop_h = 0;
const DropItem *pop_items = nullptr;
int pop_n = 0;
bool about = false;
int about_press = 0;

constexpr ToolBtn kTools[] = {
    {6, 36, "Up", ID_UP},
    {46, 72, "Computer", ID_COMPUTER},
    {122, 48, "Home", ID_HOME},
    {178, 80, "New Folder", ID_MKDIR},
    {262, 52, "Delete", ID_DEL},
};
constexpr int kToolN = (int)(sizeof(kTools) / sizeof(kTools[0]));

constexpr DropItem kFileMenu[] = {
    {"Open", "Enter", ID_OPEN},
    {"New Folder", "F7", ID_MKDIR},
    {"Delete", "Del", ID_DEL},
    {"Rename", "F2", ID_RENAME},
    {nullptr, nullptr, 0},
    {"Close", nullptr, ID_CLOSE},
};
constexpr DropItem kEditMenu[] = {
    {"Select All", "Ctrl+A", ID_SELALL},
};
constexpr DropItem kViewMenu[] = {
    {"Refresh", "F5", ID_REFRESH},
    {nullptr, nullptr, 0},
    {"Hidden Files", nullptr, ID_HIDDEN},
};
constexpr DropItem kHelpMenu[] = {
    {"About Cabinet...", nullptr, ID_ABOUT},
};
constexpr DropItem kCtxMenu[] = {
    {"Open", nullptr, ID_OPEN},
    {nullptr, nullptr, 0},
    {"Rename", "F2", ID_RENAME},
    {"Delete", "Del", ID_DEL},
};

TopMenu tops[] = {
    {"File", kFileMenu, (int)(sizeof(kFileMenu) / sizeof(kFileMenu[0]))},
    {"Edit", kEditMenu, (int)(sizeof(kEditMenu) / sizeof(kEditMenu[0]))},
    {"View", kViewMenu, (int)(sizeof(kViewMenu) / sizeof(kViewMenu[0]))},
    {"Help", kHelpMenu, (int)(sizeof(kHelpMenu) / sizeof(kHelpMenu[0]))},
};
constexpr int kTopN = (int)(sizeof(tops) / sizeof(tops[0]));

unsigned long alloc_rgb(w95::Rgb c)
{
    XColor xc{};
    // XColor channels are 16-bit; 257 * 8-bit maps 0..255 onto 0..65535 evenly.
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

int text_w(const char *s)
{
    return font ? XTextWidth(font, s, (int)strlen(s)) : 8 * (int)strlen(s);
}

void draw_str(int x, int y, int box_h, const char *s, unsigned long pix)
{
    if (!font || !s)
        return;
    int th = font->ascent + font->descent;
    int ty = y + font->ascent + std::max(0, (box_h - th) / 2);
    XSetFont(dpy, gc, font->fid);
    XSetForeground(dpy, gc, pix);
    XDrawString(dpy, win, gc, x, ty, s, (int)strlen(s));
}

void clip_str(int x, int y, int w, int h, const char *s, unsigned long pix)
{
    XRectangle r{(short)x, (short)y, (unsigned short)std::max(0, w), (unsigned short)std::max(0, h)};
    XSetClipRectangles(dpy, gc, 0, 0, &r, 1, Unsorted);
    draw_str(x, y, h, s, pix);
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

bool cmd_on(int id)
{
    if (id == ID_OPEN)
        return !ents.empty();
    if (id == ID_RENAME)
        return !ents.empty() && sel >= 0 && sel < (int)ents.size() && !ents[sel].is_parent;
    if (id == ID_DEL) {
        if (ents.empty())
            return false;
        int n = 0;
        for (int i = 0; i < (int)marked.size(); i++)
            if (marked[i] && !ents[i].is_parent)
                n++;
        if (n == 0 && sel >= 0 && sel < (int)ents.size() && !ents[sel].is_parent)
            n = 1;
        return n > 0;
    }
    return true;
}

bool cmd_checked(int id) { return id == ID_HIDDEN && show_hidden; }

void popup_metrics(const DropItem *it, int n, int &w, int &h)
{
    bool checks = false;
    w = 140;
    h = 6;
    for (int i = 0; i < n; i++) {
        if (!it[i].lab) {
            h += 8;
            continue;
        }
        if (it[i].id == ID_HIDDEN)
            checks = true;
        int tw = text_w(it[i].lab) + 28;
        if (it[i].accel)
            tw += text_w(it[i].accel) + 18;
        w = std::max(w, tw);
        h += kItemH;
    }
    if (checks)
        w += 10;
}

int popup_index(int x, int y)
{
    if (!pop_items || (!ctx_open && menu_open < 0))
        return -1;
    if (x < pop_x || y < pop_y || x >= pop_x + pop_w || y >= pop_y + pop_h)
        return -1;
    int iy = pop_y + 3;
    for (int i = 0; i < pop_n; i++) {
        if (!pop_items[i].lab) {
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
    ctx_open = false;
    menu_hover = -1;
    pop_items = nullptr;
    pop_n = 0;
}

void place_popup(int x, int y)
{
    popup_metrics(pop_items, pop_n, pop_w, pop_h);
    pop_x = x;
    pop_y = y;
    if (pop_x + pop_w > ww - 2)
        pop_x = std::max(2, ww - 2 - pop_w);
    if (pop_y + pop_h > wh - kStatH)
        pop_y = std::max(kMenuH, wh - kStatH - pop_h);
    menu_hover = -1;
    for (int k = 0; k < pop_n; k++) {
        if (pop_items[k].lab && cmd_on(pop_items[k].id)) {
            menu_hover = k;
            break;
        }
    }
}

void open_menu(int i)
{
    if (i < 0 || i >= kTopN)
        return;
    layout_menus();
    ctx_open = false;
    menu_open = i;
    pop_items = tops[i].items;
    pop_n = tops[i].n;
    place_popup(tops[i].x, kMenuH);
}

void open_ctx(int x, int y)
{
    menu_open = -1;
    ctx_open = true;
    pop_items = kCtxMenu;
    pop_n = (int)(sizeof(kCtxMenu) / sizeof(kCtxMenu[0]));
    place_popup(x, y);
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

// Detach from Cabinet so a long-running editor doesn't zombie this process.
// SIGCHLD is SIG_IGN in main, so we don't wait.
void spawn(const char *cmd)
{
    if (fork() == 0) {
        setsid();
        execl("/bin/sh", "sh", "-c", cmd, (char *)nullptr);
        _exit(127);
    }
}

// Single-quote for /bin/sh so paths with spaces or $ don't expand.
std::string shell_quote(const std::string &s)
{
    std::string o = "'";
    for (char c : s) {
        if (c == '\'')
            o += "'\\''";
        else
            o += c;
    }
    o += "'";
    return o;
}

void set_title()
{
    std::string t = "Cabinet - " + cwd;
    XStoreName(dpy, win, t.c_str());
}

// Rebuild ents from path. Hidden names (leading '.') are skipped. Sort is
// parent first, then directories, then files, case-insensitive.
void load_dir(const std::string &path)
{
    DIR *d = opendir(path.c_str());
    if (!d) {
        status = "Cannot open " + path;
        return;
    }
    cwd = path;
    while (cwd.size() > 1 && cwd.back() == '/')
        cwd.pop_back();
    ents.clear();
    if (cwd != "/") {
        Entry p;
        p.name = "..";
        p.path = cwd;
        auto slash = p.path.rfind('/');
        p.path = (slash == 0 || slash == std::string::npos) ? "/" : p.path.substr(0, slash);
        p.is_dir = true;
        p.is_parent = true;
        ents.push_back(p);
    }
    while (dirent *de = readdir(d)) {
        if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, ".."))
            continue;
        if (de->d_name[0] == '.' && !show_hidden)
            continue;
        Entry e;
        e.name = de->d_name;
        e.path = cwd == "/" ? (std::string("/") + e.name) : (cwd + "/" + e.name);
        struct stat st{};
        if (lstat(e.path.c_str(), &st) != 0)
            continue;
        e.is_dir = S_ISDIR(st.st_mode);
        e.size = st.st_size;
        e.mtime = st.st_mtime;
        ents.push_back(std::move(e));
    }
    closedir(d);
    std::sort(ents.begin(), ents.end(), [](const Entry &a, const Entry &b) {
        if (a.is_parent != b.is_parent)
            return a.is_parent;
        if (a.is_dir != b.is_dir)
            return a.is_dir;
        return strcasecmp(a.name.c_str(), b.name.c_str()) < 0;
    });
    sel = 0;
    scroll = 0;
    marked.assign(ents.size(), 0);
    if (!ents.empty())
        marked[0] = 1;
    status = std::to_string(ents.size()) + " object(s)";
    set_title();
}

int list_top() { return kMenuH + kToolH + kAddrH + kHeadH; }
int list_h() { return std::max(kRowH, wh - list_top() - kStatH); }
int rows_vis() { return std::max(1, list_h() / kRowH); }

void ensure_sel()
{
    if (ents.empty()) {
        sel = 0;
        return;
    }
    sel = std::clamp(sel, 0, (int)ents.size() - 1);
    if (sel < scroll)
        scroll = sel;
    int vis = rows_vis();
    if (sel >= scroll + vis)
        scroll = sel - vis + 1;
}

int marked_count()
{
    int n = 0;
    for (char c : marked)
        n += c ? 1 : 0;
    return n;
}

void mark_only(int i)
{
    marked.assign(ents.size(), 0);
    if (i >= 0 && i < (int)marked.size())
        marked[i] = 1;
    sel = i < 0 ? 0 : i;
    mark_anchor = sel;
}

void mark_range(int a, int b)
{
    marked.assign(ents.size(), 0);
    if (ents.empty())
        return;
    if (a > b)
        std::swap(a, b);
    a = std::clamp(a, 0, (int)ents.size() - 1);
    b = std::clamp(b, 0, (int)ents.size() - 1);
    for (int i = a; i <= b; i++)
        marked[i] = 1;
    sel = b;
}

void apply_marquee()
{
    int ly = list_top();
    int vis = rows_vis();
    int x = std::min(drag_x0, drag_x1), y = std::min(drag_y0, drag_y1);
    int w = std::abs(drag_x1 - drag_x0), h = std::abs(drag_y1 - drag_y0);
    marked = drag_saved;
    marked.resize(ents.size(), 0);
    int last = sel;
    for (int i = 0; i < vis; i++) {
        int idx = scroll + i;
        if (idx < 0 || idx >= (int)ents.size())
            break;
        int ry = ly + 1 + i * kRowH;
        int rx = 4, rw = ww - 8, rh = kRowH;
        if (x < rx + rw && rx < x + w && y < ry + rh && ry < y + h) {
            marked[idx] = 1;
            last = idx;
        }
    }
    sel = last;
}

void draw_dotted_rect(int x0, int y0, int x1, int y1)
{
    int x = std::min(x0, x1), y = std::min(y0, y1);
    int w = std::abs(x1 - x0), h = std::abs(y1 - y0);
    if (w < 1 || h < 1)
        return;
    char dash[] = {1, 1};
    XSetDashes(dpy, gc, 0, dash, 2);
    XSetLineAttributes(dpy, gc, 0, LineOnOffDash, CapButt, JoinMiter);
    XSetForeground(dpy, gc, dk.pix);
    XDrawRectangle(dpy, win, gc, x, y, (unsigned)w, (unsigned)h);
    XSetForeground(dpy, gc, white.pix);
    XSetDashes(dpy, gc, 1, dash, 2);
    XDrawRectangle(dpy, win, gc, x, y, (unsigned)w, (unsigned)h);
    XSetLineAttributes(dpy, gc, 0, LineSolid, CapButt, JoinMiter);
}

std::string fmt_size(const Entry &e)
{
    if (e.is_dir)
        return "";
    char buf[32];
    if (e.size < 1024)
        snprintf(buf, sizeof buf, "%ld", (long)e.size);
    else if (e.size < 1024 * 1024)
        snprintf(buf, sizeof buf, "%ld KB", (long)((e.size + 1023) / 1024));
    else
        snprintf(buf, sizeof buf, "%ld MB", (long)(e.size / (1024 * 1024)));
    return buf;
}

const char *fmt_type(const Entry &e)
{
    if (e.is_parent)
        return "Parent Folder";
    if (e.is_dir)
        return "File Folder";
    auto dot = e.name.rfind('.');
    if (dot != std::string::npos && dot > 0)
        return e.name.c_str() + dot;
    return "File";
}

void draw_folder_icon(int x, int y)
{
    fill(x + 1, y + 5, 12, 8, yellow.pix);
    fill(x + 1, y + 3, 6, 4, yellow.pix);
    XSetForeground(dpy, gc, lo.pix);
    XDrawRectangle(dpy, win, gc, x + 1, y + 5, 12, 8);
}

void draw_file_icon(int x, int y)
{
    fill(x + 3, y + 2, 9, 12, white.pix);
    XSetForeground(dpy, gc, lo.pix);
    XDrawRectangle(dpy, win, gc, x + 3, y + 2, 9, 12);
}

void draw_popup()
{
    if (!pop_items || (menu_open < 0 && !ctx_open))
        return;
    fill(pop_x, pop_y, pop_w, pop_h, face.pix);
    bevel(pop_x, pop_y, pop_w, pop_h, true);
    int y = pop_y + 3;
    bool checks = false;
    for (int i = 0; i < pop_n; i++)
        if (pop_items[i].lab && pop_items[i].id == ID_HIDDEN)
            checks = true;
    int tx = pop_x + (checks ? 22 : 12);
    for (int i = 0; i < pop_n; i++) {
        if (!pop_items[i].lab) {
            int gy = y + 3;
            fill(pop_x + 4, gy, pop_w - 8, 1, lo.pix);
            fill(pop_x + 4, gy + 1, pop_w - 8, 1, hi.pix);
            y += 8;
            continue;
        }
        bool hot = (menu_hover == i);
        bool on = cmd_on(pop_items[i].id);
        if (hot && on)
            fill(pop_x + 2, y, pop_w - 4, kItemH, title.pix);
        unsigned long col = !on ? lo.pix : (hot ? white.pix : fg.pix);
        if (cmd_checked(pop_items[i].id)) {
            XSetForeground(dpy, gc, col);
            XDrawLine(dpy, win, gc, pop_x + 6, y + 9, pop_x + 9, y + 12);
            XDrawLine(dpy, win, gc, pop_x + 9, y + 12, pop_x + 14, y + 5);
        }
        draw_str(tx, y, kItemH, pop_items[i].lab, col);
        if (pop_items[i].accel) {
            int aw = text_w(pop_items[i].accel);
            draw_str(pop_x + pop_w - 10 - aw, y, kItemH, pop_items[i].accel, col);
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
    draw_str(x + 8, y + 3, w95::kTitleH, "About Cabinet", white.pix);
    int cx = x + w - 6 - w95::kBtn;
    fill(cx, y + 5, w95::kBtn, w95::kBtnH, face.pix);
    bevel(cx, y + 5, w95::kBtn, w95::kBtnH, true);
    XSetForeground(dpy, gc, fg.pix);
    XDrawLine(dpy, win, gc, cx + 4, y + 8, cx + w95::kBtn - 5, y + 5 + w95::kBtnH - 4);
    XDrawLine(dpy, win, gc, cx + w95::kBtn - 5, y + 8, cx + 4, y + 5 + w95::kBtnH - 4);
    draw_str(x + 16, y + 32, 18, "Chime Cabinet", fg.pix);
    draw_str(x + 16, y + 50, 16, "A file manager for the Chime desktop.", fg.pix);
    int ox, oy, ow, oh;
    about_ok(ox, oy, ow, oh);
    btn(ox, oy, ow, oh, "OK", about_press == ID_ABOUT_OK);
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

    int ay = kMenuH + kToolH + 2;
    draw_str(8, ay, kAddrH - 2, "Address", fg.pix);
    int ax = 64, aw = ww - 72, ah = 18;
    fill(ax, ay, aw, ah, field.pix);
    bevel(ax, ay, aw, ah, false);
    clip_str(ax + 6, ay, aw - 10, ah, cwd.c_str(), fg.pix);

    int hy = kMenuH + kToolH + kAddrH;
    fill(4, hy, ww - 8, kHeadH, face.pix);
    bevel(4, hy, ww - 8, kHeadH, true);
    draw_str(24, hy, kHeadH, "Name", fg.pix);
    draw_str(ww - 260, hy, kHeadH, "Size", fg.pix);
    draw_str(ww - 160, hy, kHeadH, "Type", fg.pix);

    int lx = 4, ly = list_top(), lw = ww - 8, lh = list_h();
    fill(lx, ly, lw, lh, white.pix);
    bevel(lx, ly, lw, lh, false);

    ensure_sel();
    int vis = rows_vis();
    for (int i = 0; i < vis; i++) {
        int idx = scroll + i;
        if (idx >= (int)ents.size())
            break;
        int y = ly + 1 + i * kRowH;
        const Entry &e = ents[idx];
        bool on = (idx < (int)marked.size() && marked[idx]);
        if (on)
            fill(lx + 2, y, lw - 4, kRowH, title.pix);
        unsigned long col = on ? white.pix : fg.pix;
        if (e.is_dir)
            draw_folder_icon(lx + 6, y);
        else
            draw_file_icon(lx + 6, y);
        clip_str(lx + 24, y, ww - 300, kRowH, e.name.c_str(), col);
        auto sz = fmt_size(e);
        clip_str(ww - 260, y, 90, kRowH, sz.c_str(), col);
        clip_str(ww - 160, y, 140, kRowH, fmt_type(e), col);
    }
    if (dragging) {
        XRectangle r{(short)lx, (short)ly, (unsigned short)std::max(0, lw), (unsigned short)std::max(0, lh)};
        XSetClipRectangles(dpy, gc, 0, 0, &r, 1, Unsorted);
        draw_dotted_rect(drag_x0, drag_y0, drag_x1, drag_y1);
        XSetClipMask(dpy, gc, None);
    }

    int sy = wh - kStatH;
    fill(0, sy, ww, kStatH, face.pix);
    bevel(2, sy + 2, ww - 4, kStatH - 4, false);
    std::string st = status;
    if (prompt == Prompt::Del)
        st = "Delete " + pbuf + "?  Y / N";
    else if (prompt == Prompt::Rename)
        st = "Rename: " + pbuf + "_";
    else if (prompt == Prompt::Mkdir)
        st = "New folder: " + pbuf + "_";
    clip_str(8, sy + 2, ww - 16, kStatH - 4, st.c_str(), fg.pix);
    draw_popup();
    if (about)
        draw_about();
}

void go_up()
{
    if (cwd == "/" || cwd.empty())
        return;
    auto slash = cwd.rfind('/');
    load_dir((slash == 0 || slash == std::string::npos) ? "/" : cwd.substr(0, slash));
}

void open_sel()
{
    if (ents.empty())
        return;
    int nmark = marked_count();
    if (nmark <= 1) {
        if (sel < 0 || sel >= (int)ents.size())
            return;
        const Entry &e = ents[sel];
        if (e.is_dir) {
            load_dir(e.path);
            return;
        }
        std::string q = shell_quote(e.path);
        std::string cmd = "editor " + q + " 2>/dev/null || aterm -e vi " + q + " || xterm -e vi " + q;
        if (access(e.path.c_str(), X_OK) == 0)
            cmd = q + " || " + cmd;
        spawn(cmd.c_str());
        status = "Opened " + e.name;
        return;
    }
    int opened = 0;
    for (int i = 0; i < (int)ents.size(); i++) {
        if (!marked[i] || ents[i].is_dir)
            continue;
        std::string q = shell_quote(ents[i].path);
        std::string cmd = "editor " + q + " 2>/dev/null || aterm -e vi " + q + " || xterm -e vi " + q;
        if (access(ents[i].path.c_str(), X_OK) == 0)
            cmd = q + " || " + cmd;
        spawn(cmd.c_str());
        opened++;
    }
    status = opened ? ("Opened " + std::to_string(opened) + " file(s)") : "Nothing to open";
}

void do_delete()
{
    if (ents.empty())
        return;
    int n = 0, failed = 0;
    for (int i = (int)ents.size() - 1; i >= 0; i--) {
        if (i >= (int)marked.size() || !marked[i])
            continue;
        const Entry &e = ents[i];
        if (e.is_parent)
            continue;
        int r = e.is_dir ? rmdir(e.path.c_str()) : unlink(e.path.c_str());
        if (r != 0)
            failed++;
        else
            n++;
    }
    if (n == 0 && failed == 0) {
        status = "Nothing to delete";
        return;
    }
    if (failed)
        status = std::to_string(n) + " deleted, " + std::to_string(failed) + " failed";
    else
        status = "Deleted " + std::to_string(n) + " object(s)";
    load_dir(cwd);
}

void do_mkdir(const std::string &name)
{
    if (name.empty())
        return;
    std::string p = cwd == "/" ? ("/" + name) : (cwd + "/" + name);
    if (mkdir(p.c_str(), 0755) != 0)
        status = std::string("mkdir failed: ") + strerror(errno);
    else {
        status = "Created " + name;
        load_dir(cwd);
    }
}

void do_rename(const std::string &name)
{
    if (ents.empty() || name.empty())
        return;
    const Entry &e = ents[sel];
    if (e.is_parent)
        return;
    std::string dest = cwd == "/" ? ("/" + name) : (cwd + "/" + name);
    if (rename(e.path.c_str(), dest.c_str()) != 0)
        status = std::string("Rename failed: ") + strerror(errno);
    else {
        status = "Renamed";
        load_dir(cwd);
    }
}

void start_mkdir()
{
    prompt = Prompt::Mkdir;
    pbuf = "New Folder";
}

void start_rename()
{
    if (ents.empty() || ents[sel].is_parent) {
        status = "Cannot rename";
        return;
    }
    prompt = Prompt::Rename;
    pbuf = ents[sel].name;
}

void start_del()
{
    int n = marked_count();
    if (n == 0 && !ents.empty() && sel >= 0 && sel < (int)ents.size())
        mark_only(sel);
    n = marked_count();
    if (n == 1 && sel >= 0 && sel < (int)ents.size() && ents[sel].is_parent) {
        status = "Cannot delete ..";
        return;
    }
    if (n < 1) {
        status = "Nothing to delete";
        return;
    }
    prompt = Prompt::Del;
    pbuf = n == 1 && sel >= 0 && sel < (int)ents.size() ? ents[sel].name : (std::to_string(n) + " objects");
}

void do_selall()
{
    marked.assign(ents.size(), 1);
    if (!ents.empty() && ents[0].is_parent)
        marked[0] = 0;
    status = std::to_string(marked_count()) + " object(s) selected";
}

void do_cmd(int id)
{
    close_menu();
    switch (id) {
    case ID_UP:
        go_up();
        break;
    case ID_COMPUTER:
        load_dir("/");
        break;
    case ID_HOME: {
        const char *home = getenv("HOME");
        load_dir(home && *home ? home : "/home/tc");
        break;
    }
    case ID_OPEN:
        open_sel();
        break;
    case ID_MKDIR:
        start_mkdir();
        break;
    case ID_DEL:
        start_del();
        break;
    case ID_RENAME:
        start_rename();
        break;
    case ID_CLOSE:
        running = false;
        break;
    case ID_SELALL:
        do_selall();
        break;
    case ID_REFRESH:
        load_dir(cwd.empty() ? "/" : cwd);
        break;
    case ID_HIDDEN:
        show_hidden = !show_hidden;
        load_dir(cwd.empty() ? "/" : cwd);
        status = show_hidden ? "Showing hidden files" : "Hidden files hidden";
        break;
    case ID_ABOUT:
        about = true;
        about_press = 0;
        break;
    default:
        break;
    }
}

void on_list_click(int x, int y, Time t, unsigned state, bool press)
{
    int row = (y - list_top() - 1) / kRowH;
    int idx = scroll + row;
    bool ctrl = (state & ControlMask) != 0;
    bool shift = (state & ShiftMask) != 0;
    if (press) {
        drag_x0 = drag_x1 = x;
        drag_y0 = drag_y1 = y;
        dragging = false;
        if (idx >= 0 && idx < (int)ents.size()) {
            if (ctrl) {
                if (idx < (int)marked.size())
                    marked[idx] = marked[idx] ? 0 : 1;
                sel = idx;
            } else if (shift)
                mark_range(mark_anchor, idx);
            else
                mark_only(idx);
            bool dbl = (idx == last_row && t - last_click < w95::kDblClickMs && !ctrl && !shift);
            last_row = idx;
            last_click = t;
            if (dbl)
                open_sel();
        } else if (!ctrl)
            mark_only(-1);
        drag_saved = marked;
        return;
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
    if (menu_open >= 0 || ctx_open) {
        if (ks == XK_Escape) {
            close_menu();
            return;
        }
        if (!ctx_open && menu_open >= 0) {
            if (ks == XK_Left)
                open_menu((menu_open + kTopN - 1) % kTopN);
            else if (ks == XK_Right)
                open_menu((menu_open + 1) % kTopN);
        }
        if (ks == XK_Down || ks == XK_Up) {
            int dir = (ks == XK_Down) ? 1 : -1;
            int i = menu_hover;
            for (int k = 0; k < pop_n + 1; k++) {
                i = (i + dir + pop_n) % pop_n;
                if (pop_items && pop_items[i].lab && cmd_on(pop_items[i].id)) {
                    menu_hover = i;
                    break;
                }
            }
            return;
        }
        if (ks == XK_Return || ks == XK_space) {
            if (menu_hover >= 0 && menu_hover < pop_n && pop_items && pop_items[menu_hover].lab &&
                cmd_on(pop_items[menu_hover].id))
                do_cmd(pop_items[menu_hover].id);
        }
        return;
    }

    if (prompt != Prompt::Idle) {
        if (ks == XK_Escape) {
            prompt = Prompt::Idle;
            status = "Cancelled";
            return;
        }
        if (prompt == Prompt::Del) {
            if (ks == XK_y || ks == XK_Y || ks == XK_Return) {
                prompt = Prompt::Idle;
                do_delete();
            } else if (ks == XK_n || ks == XK_N)
                prompt = Prompt::Idle;
            return;
        }
        if (ks == XK_Return) {
            if (prompt == Prompt::Mkdir)
                do_mkdir(pbuf);
            else
                do_rename(pbuf);
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

    bool ctrl = (e->state & ControlMask) != 0;
    bool shift = (e->state & ShiftMask) != 0;
    if (ctrl && (ks == XK_a || ks == XK_A)) {
        do_selall();
        return;
    }
    int prev = sel;
    if (ks == XK_Up)
        sel--;
    else if (ks == XK_Down)
        sel++;
    else if (ks == XK_Page_Up)
        sel -= rows_vis();
    else if (ks == XK_Page_Down)
        sel += rows_vis();
    else if (ks == XK_Home)
        sel = 0;
    else if (ks == XK_End)
        sel = (int)ents.size() - 1;
    else if (ks == XK_Return)
        open_sel();
    else if (ks == XK_BackSpace)
        go_up();
    else if (ks == XK_Delete)
        start_del();
    else if (ks == XK_F2)
        start_rename();
    else if (ks == XK_F5)
        load_dir(cwd.empty() ? "/" : cwd);
    else if (ks == XK_F7)
        start_mkdir();
    else if (ks == XK_space && ctrl && sel >= 0 && sel < (int)marked.size())
        marked[sel] = marked[sel] ? 0 : 1;
    ensure_sel();
    if (ks == XK_Up || ks == XK_Down || ks == XK_Page_Up || ks == XK_Page_Down || ks == XK_Home || ks == XK_End) {
        if (shift)
            mark_range(mark_anchor, sel);
        else if (!ctrl)
            mark_only(sel);
        else
            mark_anchor = prev;
    }
}

bool about_close_hit(int x, int y)
{
    int bx, by, bw, bh;
    about_box(bx, by, bw, bh);
    int cx = bx + bw - 6 - w95::kBtn, cy = by + 5;
    return x >= cx && y >= cy && x < cx + w95::kBtn && y < cy + w95::kBtnH;
}

bool popup_open() { return menu_open >= 0 || ctx_open; }

void fire_popup_at(int x, int y)
{
    int hi = popup_index(x, y);
    if (hi >= 0 && pop_items && pop_items[hi].lab && cmd_on(pop_items[hi].id))
        do_cmd(pop_items[hi].id);
}

void on_press(XButtonEvent *b)
{
    if (b->button == 4 || b->button == 5) {
        if (about || popup_open())
            return;
        if (b->button == 4)
            scroll = std::max(0, scroll - 3);
        else
            scroll = std::min(std::max(0, (int)ents.size() - rows_vis()), scroll + 3);
        return;
    }

    if (about) {
        if (b->button != 1)
            return;
        int ox, oy, ow, oh;
        about_ok(ox, oy, ow, oh);
        if (b->x >= ox && b->y >= oy && b->x < ox + ow && b->y < oy + oh)
            about_press = ID_ABOUT_OK;
        else if (about_close_hit(b->x, b->y))
            about = false;
        return;
    }

    if (popup_open()) {
        if (b->button == 3)
            return;
        int hi = popup_index(b->x, b->y);
        if (hi >= 0) {
            menu_hover = hi;
            return;
        }
        int t = hit_top(b->x, b->y);
        if (t >= 0 && !ctx_open) {
            if (t == menu_open)
                close_menu();
            else
                open_menu(t);
            return;
        }
        close_menu();
        if (b->button != 1)
            return;
    }

    if (b->button == 3) {
        if (b->y >= list_top() && b->y < list_top() + list_h()) {
            int row = (b->y - list_top() - 1) / kRowH;
            int idx = scroll + row;
            if (idx >= 0 && idx < (int)ents.size()) {
                if (idx >= (int)marked.size() || !marked[idx])
                    mark_only(idx);
                sel = idx;
            }
            open_ctx(b->x, b->y);
        }
        return;
    }

    if (b->button != 1)
        return;

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

    if (b->y >= list_top() && b->y < list_top() + list_h())
        on_list_click(b->x, b->y, b->time, b->state, true);
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
    if (popup_open()) {
        fire_popup_at(b->x, b->y);
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
    if (dragging) {
        drag_x1 = b->x;
        drag_y1 = b->y;
        apply_marquee();
        dragging = false;
    }
}

void on_motion(XMotionEvent *m)
{
    if (about) {
        if (about_press == ID_ABOUT_OK || about_press == -ID_ABOUT_OK) {
            int ox, oy, ow, oh;
            about_ok(ox, oy, ow, oh);
            bool in = m->x >= ox && m->y >= oy && m->x < ox + ow && m->y < oy + oh;
            about_press = in ? ID_ABOUT_OK : -ID_ABOUT_OK;
        }
        return;
    }
    if (popup_open()) {
        int t = hit_top(m->x, m->y);
        if (t >= 0 && !ctx_open && t != menu_open)
            open_menu(t);
        else
            menu_hover = popup_index(m->x, m->y);
        return;
    }
    if (tool_press >= 0) {
        tool_in = hit_tool(m->x, m->y) == tool_press;
        return;
    }
    if (m->state & Button1Mask) {
        int dx = m->x - drag_x0, dy = m->y - drag_y0;
        if (!dragging && dx * dx + dy * dy > 16)
            dragging = true;
        if (dragging) {
            drag_x1 = m->x;
            drag_y1 = m->y;
            apply_marquee();
        }
    }
}

} // namespace

int main(int argc, char **argv)
{
    signal(SIGCHLD, SIG_IGN);
    dpy = XOpenDisplay(nullptr);
    if (!dpy) {
        std::fprintf(stderr, "cabinet: no display\n");
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
    yellow.pix = alloc_rgb(w95::rgb_yellow);

    XSetWindowAttributes swa{};
    swa.background_pixel = face.pix;
    swa.event_mask = ExposureMask | KeyPressMask | ButtonPressMask | ButtonReleaseMask | ButtonMotionMask |
                     PointerMotionMask | StructureNotifyMask | LeaveWindowMask;
    win = XCreateWindow(dpy, RootWindow(dpy, screen), 80, 60, kWinW, kWinH, 0, CopyFromParent, InputOutput,
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
    ch.res_name = (char *)"cabinet";
    ch.res_class = (char *)"Cabinet";
    XSetClassHint(dpy, win, &ch);

    load_dir(argc >= 2 ? argv[1] : "/");

    XMapWindow(dpy, win);
    while (running) {
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
            if (about || popup_open() || tool_press >= 0 || (e.xmotion.state & Button1Mask)) {
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
        } else if (e.type == ClientMessage && (Atom)e.xclient.data.l[0] == wm_delete)
            running = false;
    }
    XCloseDisplay(dpy);
    return 0;
}
