#include "theme.h"

#include <X11/Xlib.h>
#include <X11/Xatom.h>
#include <X11/Xutil.h>
#include <X11/keysym.h>

#include <algorithm>
#include <csignal>
#include <cstdio>
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
constexpr int kHeadH = 18;
constexpr int kStatH = 20;
constexpr int kRowH = 16;

struct Col {
    unsigned long pix;
};

struct Entry {
    std::string name;
    std::string path;
    bool is_dir = false;
    bool is_parent = false;
    off_t size = 0;
    time_t mtime = 0;
};

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
std::string cwd;
Prompt prompt = Prompt::Idle;
std::string pbuf, status;
Time last_click = 0;
int last_row = -1;
Atom wm_delete, wm_protocols;

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

void spawn(const char *cmd)
{
    if (fork() == 0) {
        setsid();
        execl("/bin/sh", "sh", "-c", cmd, (char *)nullptr);
        _exit(127);
    }
}

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
        bool on = (idx == sel);
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
    if (ents.empty() || sel < 0 || sel >= (int)ents.size())
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
}

void do_delete()
{
    if (ents.empty())
        return;
    const Entry &e = ents[sel];
    if (e.is_parent) {
        status = "Cannot delete ..";
        return;
    }
    int r = e.is_dir ? rmdir(e.path.c_str()) : unlink(e.path.c_str());
    if (r != 0)
        status = std::string("Delete failed: ") + strerror(errno);
    else {
        status = "Deleted " + e.name;
        load_dir(cwd);
    }
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
    if (ents.empty() || ents[sel].is_parent) {
        status = "Nothing to delete";
        return;
    }
    prompt = Prompt::Del;
    pbuf = ents[sel].name;
}

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

void on_list_click(int y, Time t, bool dbl_hint)
{
    int row = (y - list_top() - 1) / kRowH;
    int idx = scroll + row;
    if (idx < 0 || idx >= (int)ents.size())
        return;
    sel = idx;
    bool dbl = dbl_hint || (idx == last_row && t - last_click < w95::kDblClickMs);
    last_row = idx;
    last_click = t;
    if (dbl)
        open_sel();
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
    ensure_sel();
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
    swa.event_mask = ExposureMask | KeyPressMask | ButtonPressMask | StructureNotifyMask;
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
                    on_list_click(y, e.xbutton.time, e.xbutton.button == 1 && false);
                redraw();
            } else if (e.xbutton.button == 3 && e.xbutton.y >= list_top()) {
                on_list_click(e.xbutton.y, e.xbutton.time, false);
                start_del();
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
