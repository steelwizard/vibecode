// Cabinet — Chime's Explorer-style file manager.
// A single X window with a fake menu bar, toolbar, address well, details list,
// and status line. Directory IO is POSIX; opening a file shells out to editor
// or vi. Layout numbers must stay in sync with on_tool() hit boxes.

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
constexpr int kMenuH = 20;  // Decorative "File Edit View Help" strip (not wired)
constexpr int kToolH = 26;
constexpr int kAddrH = 22;
constexpr int kHeadH = 18;  // Name / Size / Type column headers
constexpr int kStatH = 20;
constexpr int kRowH = 16;

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
    unsigned long tl = raised ? hi.pix : lo.pix;
    unsigned long br = raised ? dk.pix : hi.pix;
    XSetForeground(dpy, gc, tl);
    XDrawLine(dpy, win, gc, x, y, x + w - 1, y);
    XDrawLine(dpy, win, gc, x, y, x, y + h - 1);
    XSetForeground(dpy, gc, br);
    XDrawLine(dpy, win, gc, x, y + h - 1, x + w - 1, y + h - 1);
    XDrawLine(dpy, win, gc, x + w - 1, y, x + w - 1, y + h - 1);
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

void btn(int x, int y, int w, int h, const char *lab)
{
    fill(x, y, w, h, face.pix);
    bevel(x, y, w, h, true);
    int tw = text_w(lab);
    draw_str(x + (w - tw) / 2, y, h, lab, fg.pix);
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
        if (de->d_name[0] == '.')
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

void redraw()
{
    fill(0, 0, ww, wh, face.pix);
    draw_str(8, 0, kMenuH, "File   Edit   View   Help", fg.pix);

    int ty = kMenuH + 3;
    btn(6, ty, 36, 20, "Up");
    btn(46, ty, 72, 20, "Computer");
    btn(122, ty, 48, 20, "Home");
    btn(178, ty, 80, 20, "New Folder");
    btn(262, ty, 52, 20, "Delete");

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

// Toolbar buttons are painted at fixed x; keep these widths matching btn() above.
void on_tool(int x, int y)
{
    if (y < kMenuH || y >= kMenuH + kToolH)
        return;
    auto hit = [&](int bx, int bw) { return x >= bx && x < bx + bw; };
    if (hit(6, 36))
        go_up();
    else if (hit(46, 72))
        load_dir("/");
    else if (hit(122, 48)) {
        const char *home = getenv("HOME");
        load_dir(home && *home ? home : "/home/tc");
    } else if (hit(178, 80))
        start_mkdir();
    else if (hit(262, 52))
        start_del();
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
        marked.assign(ents.size(), 1);
        if (!ents.empty() && ents[0].is_parent)
            marked[0] = 0;
        status = std::to_string(marked_count()) + " object(s) selected";
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
                     StructureNotifyMask;
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
    for (;;) {
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
                scroll = std::min(std::max(0, (int)ents.size() - rows_vis()), scroll + 3);
                redraw();
            } else if (e.xbutton.button == 1) {
                int y = e.xbutton.y;
                if (y < kMenuH + kToolH)
                    on_tool(e.xbutton.x, y);
                else if (y >= list_top() && y < list_top() + list_h())
                    on_list_click(e.xbutton.x, y, e.xbutton.time, e.xbutton.state, true);
                redraw();
            } else if (e.xbutton.button == 3 && e.xbutton.y >= list_top()) {
                on_list_click(e.xbutton.x, e.xbutton.y, e.xbutton.time, e.xbutton.state, true);
                start_del();
                redraw();
            }
        } else if (e.type == MotionNotify && (e.xmotion.state & Button1Mask)) {
            int dx = e.xmotion.x - drag_x0, dy = e.xmotion.y - drag_y0;
            if (!dragging && dx * dx + dy * dy > 16)
                dragging = true;
            if (dragging) {
                drag_x1 = e.xmotion.x;
                drag_y1 = e.xmotion.y;
                apply_marquee();
                redraw();
            }
        } else if (e.type == ButtonRelease && e.xbutton.button == 1) {
            if (dragging) {
                drag_x1 = e.xbutton.x;
                drag_y1 = e.xbutton.y;
                apply_marquee();
                dragging = false;
                redraw();
            }
        } else if (e.type == KeyPress) {
            on_key(&e.xkey);
            redraw();
        } else if (e.type == ClientMessage && (Atom)e.xclient.data.l[0] == wm_delete)
            break;
    }
    XCloseDisplay(dpy);
    return 0;
}
