// Chime window manager: ICCCM/EWMH reparenting WM with a Win95 shell
// (desktop, taskbar, Start menu, system tray) on every RandR/Xinerama head.
// This file owns session setup, clients, snapping, tray, settings, and the
// X event loop. Painting lives in draw.cpp.

#include "wm.h"

#include <X11/Xproto.h>

#include <algorithm>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <dirent.h>
#include <strings.h>
#include <string>
#include <sys/select.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>

static WM *g_wm;       // Used only so signal/error paths can find the instance
static bool g_other_wm; // Set if SubstructureRedirect is already taken

// Temporary handler while we try to become WM: BadAccess means another WM won.
static int xerr_start(Display *, XErrorEvent *e)
{
    if (e->error_code == BadAccess)
        g_other_wm = true;
    return 0;
}

// Steady-state handler. Racey Destroy/Unmap on client windows is normal;
// swallowing BadWindow/BadDrawable/BadMatch keeps the session alive.
static int xerr(Display *, XErrorEvent *e)
{
    if (e->error_code == BadWindow || e->error_code == BadDrawable || e->error_code == BadMatch)
        return 0;
    return 0;
}

static long now_ms()
{
    timeval tv{};
    gettimeofday(&tv, nullptr);
    return tv.tv_sec * 1000L + tv.tv_usec / 1000;
}

static std::string shell_quote(const std::string &s)
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

static const char *kEditorCmd = "editor || aterm -e vi || xterm -e vi || true";
static const char *kTermCmd = "aterm || xterm || x-terminal-emulator || true";

static uint16_t read_u16(FILE *f)
{
    int a = fgetc(f), b = fgetc(f);
    if (a < 0 || b < 0)
        return 0;
    return (uint16_t)(a | (b << 8));
}

static uint32_t read_u32(FILE *f)
{
    uint16_t lo = read_u16(f), hi = read_u16(f);
    return lo | ((uint32_t)hi << 16);
}

static bool ppm_skip(FILE *f)
{
    int c;
    do {
        c = fgetc(f);
        if (c == '#')
            while (c != '\n' && c != EOF)
                c = fgetc(f);
    } while (c == ' ' || c == '\n' || c == '\t' || c == '\r');
    if (c == EOF)
        return false;
    ungetc(c, f);
    return true;
}

static bool load_ppm(FILE *f, Raster &out)
{
    char mag[3] = {};
    if (fread(mag, 1, 2, f) != 2 || mag[0] != 'P' || mag[1] != '6')
        return false;
    int w = 0, h = 0, maxv = 0;
    if (!ppm_skip(f) || fscanf(f, "%d", &w) != 1)
        return false;
    if (!ppm_skip(f) || fscanf(f, "%d", &h) != 1)
        return false;
    if (!ppm_skip(f) || fscanf(f, "%d", &maxv) != 1 || w < 1 || h < 1 || w > 4096 || h > 4096)
        return false;
    fgetc(f);
    out.w = w;
    out.h = h;
    out.rgb.resize((size_t)w * h * 3);
    return fread(out.rgb.data(), 1, out.rgb.size(), f) == out.rgb.size();
}

static bool load_bmp(FILE *f, Raster &out)
{
    if (fgetc(f) != 'B' || fgetc(f) != 'M')
        return false;
    read_u32(f);
    read_u16(f);
    read_u16(f);
    uint32_t off = read_u32(f);
    uint32_t hdr = read_u32(f);
    if (hdr < 40)
        return false;
    int32_t width = (int32_t)read_u32(f);
    int32_t height = (int32_t)read_u32(f);
    read_u16(f);
    uint16_t bpp = read_u16(f);
    uint32_t comp = read_u32(f);
    if (comp != 0 || (bpp != 24 && bpp != 32) || width < 1 || width > 4096)
        return false;
    int h = height < 0 ? -height : height;
    if (h < 1 || h > 4096)
        return false;
    bool topdown = height < 0;
    if (fseek(f, (long)off, SEEK_SET) != 0)
        return false;
    out.w = width;
    out.h = h;
    out.rgb.assign((size_t)width * h * 3, 0);
    int bppb = bpp / 8;
    int rowb = (width * bppb + 3) & ~3;
    std::vector<uint8_t> row((size_t)rowb);
    for (int y = 0; y < h; y++) {
        if (fread(row.data(), 1, (size_t)rowb, f) != (size_t)rowb)
            return false;
        int dy = topdown ? y : (h - 1 - y);
        for (int x = 0; x < width; x++) {
            int s = x * bppb;
            size_t d = ((size_t)dy * width + x) * 3;
            out.rgb[d] = row[s + 2];
            out.rgb[d + 1] = row[s + 1];
            out.rgb[d + 2] = row[s];
        }
    }
    return true;
}

static bool is_image_name(const char *n)
{
    size_t len = strlen(n);
    if (len < 4)
        return false;
    const char *e4 = n + len - 4;
    if (!strcasecmp(e4, ".ppm") || !strcasecmp(e4, ".bmp") || !strcasecmp(e4, ".png") || !strcasecmp(e4, ".jpg") ||
        !strcasecmp(e4, ".gif") || !strcasecmp(e4, ".pnm"))
        return true;
    return len >= 5 && !strcasecmp(n + len - 5, ".jpeg");
}

static bool convert_to_ppm(const char *src, const char *dst)
{
    std::string q = shell_quote(src);
    std::string d = shell_quote(dst);
    char cmd[2048];
    std::snprintf(cmd, sizeof(cmd),
                  "convert %s -depth 8 ppm:%s 2>/dev/null || "
                  "magick %s -depth 8 ppm:%s 2>/dev/null || "
                  "ffmpeg -nostdin -y -i %s -pix_fmt rgb24 %s >/dev/null 2>&1",
                  q.c_str(), d.c_str(), q.c_str(), d.c_str(), q.c_str(), d.c_str());
    return system(cmd) == 0;
}

static bool load_image_path(const char *path, Raster &out)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return false;
    char mag[2] = {};
    if (fread(mag, 1, 2, f) != 2) {
        fclose(f);
        return false;
    }
    rewind(f);
    bool ok = false;
    if (mag[0] == 'P' && mag[1] == '6')
        ok = load_ppm(f, out);
    else if (mag[0] == 'B' && mag[1] == 'M')
        ok = load_bmp(f, out);
    fclose(f);
    if (ok && out.w > 0)
        return true;
    char tmp[64];
    std::snprintf(tmp, sizeof(tmp), "/tmp/chime-wall-%d.ppm", (int)getpid());
    if (!convert_to_ppm(path, tmp))
        return false;
    FILE *g = fopen(tmp, "rb");
    if (!g) {
        unlink(tmp);
        return false;
    }
    ok = load_ppm(g, out);
    fclose(g);
    unlink(tmp);
    return ok && out.w > 0;
}

static bool save_ppm(const char *path, const Raster &r)
{
    FILE *f = fopen(path, "wb");
    if (!f)
        return false;
    std::fprintf(f, "P6\n%d %d\n255\n", r.w, r.h);
    bool ok = fwrite(r.rgb.data(), 1, r.rgb.size(), f) == r.rgb.size();
    fclose(f);
    return ok;
}

static Raster scale_nn(const Raster &s, int nw, int nh)
{
    Raster d;
    d.w = nw;
    d.h = nh;
    d.rgb.resize((size_t)nw * (size_t)nh * 3);
    if (s.w < 1 || s.h < 1)
        return d;
    for (int y = 0; y < nh; y++) {
        int sy = y * s.h / nh;
        for (int x = 0; x < nw; x++) {
            int sx = x * s.w / nw;
            memcpy(&d.rgb[((size_t)y * nw + x) * 3], &s.rgb[((size_t)sy * s.w + sx) * 3], 3);
        }
    }
    return d;
}

static std::string base_name(const std::string &p)
{
    auto sl = p.find_last_of('/');
    return sl == std::string::npos ? p : p.substr(sl + 1);
}

static ColorScheme scheme_from_builtin(const w95::Scheme &b)
{
    ColorScheme s;
    s.name = b.name;
    s.desktop = b.desktop;
    s.face = b.face;
    s.hi = b.hi;
    s.lo = b.lo;
    s.dk = b.dk;
    s.title = b.title;
    s.title_in = b.title_in;
    s.text = b.text;
    s.field = b.field;
    s.banner = b.banner;
    s.builtin = true;
    return s;
}

unsigned long WM::alloc_rgb(w95::Rgb c)
{
    XColor xc{};
    // 8-bit channel -> 16-bit XColor: 255*257 = 65535.
    xc.red = (unsigned short)(c.r * 257);
    xc.green = (unsigned short)(c.g * 257);
    xc.blue = (unsigned short)(c.b * 257);
    xc.flags = DoRed | DoGreen | DoBlue;
    XAllocColor(dpy, cmap, &xc);
    return xc.pixel;
}

// Create an InputOutput window on the root. override_redirect is set for
// shell chrome (taskbar, menus, dialogs) so we never try to manage ourselves.
Window WM::mkwin(int x, int y, int w, int h, unsigned long bg, long mask, bool override)
{
    XSetWindowAttributes swa{};
    swa.background_pixel = bg;
    swa.event_mask = mask;
    swa.override_redirect = override ? True : False;
    swa.colormap = cmap;
    swa.border_pixel = 0;
    swa.cursor = cur_left;
    return XCreateWindow(dpy, root, x, y, (unsigned)w, (unsigned)h, 0, depth, InputOutput, vis,
                         CWBackPixel | CWEventMask | CWOverrideRedirect | CWColormap | CWBorderPixel | CWCursor, &swa);
}

void WM::intern_atoms()
{
    // ICCCM names first, then EWMH, then freedesktop system tray / XEmbed.
    utf8 = XInternAtom(dpy, "UTF8_STRING", False);
    wm_protocols = XInternAtom(dpy, "WM_PROTOCOLS", False);
    wm_delete = XInternAtom(dpy, "WM_DELETE_WINDOW", False);
    wm_state = XInternAtom(dpy, "WM_STATE", False);
    wm_take_focus = XInternAtom(dpy, "WM_TAKE_FOCUS", False);
    net_supported = XInternAtom(dpy, "_NET_SUPPORTED", False);
    net_wm_name = XInternAtom(dpy, "_NET_WM_NAME", False);
    net_wm_state = XInternAtom(dpy, "_NET_WM_STATE", False);
    net_active = XInternAtom(dpy, "_NET_ACTIVE_WINDOW", False);
    net_client_list = XInternAtom(dpy, "_NET_CLIENT_LIST", False);
    net_supporting = XInternAtom(dpy, "_NET_SUPPORTING_WM_CHECK", False);
    net_wm_window_type = XInternAtom(dpy, "_NET_WM_WINDOW_TYPE", False);
    net_type_desktop = XInternAtom(dpy, "_NET_WM_WINDOW_TYPE_DESKTOP", False);
    net_type_dock = XInternAtom(dpy, "_NET_WM_WINDOW_TYPE_DOCK", False);
    net_workarea = XInternAtom(dpy, "_NET_WORKAREA", False);
    net_number_desktops = XInternAtom(dpy, "_NET_NUMBER_OF_DESKTOPS", False);
    net_current_desktop = XInternAtom(dpy, "_NET_CURRENT_DESKTOP", False);
    net_desktop_geometry = XInternAtom(dpy, "_NET_DESKTOP_GEOMETRY", False);
    net_wm_state_hidden = XInternAtom(dpy, "_NET_WM_STATE_HIDDEN", False);
    net_wm_state_maxv = XInternAtom(dpy, "_NET_WM_STATE_MAXIMIZED_VERT", False);
    net_wm_state_maxh = XInternAtom(dpy, "_NET_WM_STATE_MAXIMIZED_HORZ", False);
    net_active_window = net_active;
    motif_hints = XInternAtom(dpy, "_MOTIF_WM_HINTS", False);
    // Selection name is per-screen: _NET_SYSTEM_TRAY_S0 on the first screen.
    char tray_sel[32];
    std::snprintf(tray_sel, sizeof(tray_sel), "_NET_SYSTEM_TRAY_S%d", screen);
    net_system_tray = XInternAtom(dpy, tray_sel, False);
    net_system_tray_opcode = XInternAtom(dpy, "_NET_SYSTEM_TRAY_OPCODE", False);
    net_system_tray_orientation = XInternAtom(dpy, "_NET_SYSTEM_TRAY_ORIENTATION", False);
    net_system_tray_visual = XInternAtom(dpy, "_NET_SYSTEM_TRAY_VISUAL", False);
    xembed = XInternAtom(dpy, "_XEMBED", False);
    xembed_info = XInternAtom(dpy, "_XEMBED_INFO", False);
    manager = XInternAtom(dpy, "MANAGER", False);
}

// Announce ourselves as the supporting WM and publish the subset of EWMH
// we actually implement (one virtual desktop, workarea, active window, …).
void WM::ewmh_init()
{
    checkwin = XCreateSimpleWindow(dpy, root, 0, 0, 1, 1, 0, 0, 0);
    const char *name = "chime";
    XChangeProperty(dpy, checkwin, net_supporting, XA_WINDOW, 32, PropModeReplace, (unsigned char *)&checkwin, 1);
    XChangeProperty(dpy, root, net_supporting, XA_WINDOW, 32, PropModeReplace, (unsigned char *)&checkwin, 1);
    XChangeProperty(dpy, checkwin, net_wm_name, utf8, 8, PropModeReplace, (unsigned char *)name, (int)strlen(name));
    Atom supported[] = {net_supported, net_wm_name, net_wm_state, net_active, net_client_list, net_supporting,
                        net_workarea, net_number_desktops, net_current_desktop, net_desktop_geometry,
                        net_wm_state_hidden, net_wm_state_maxv, net_wm_state_maxh, net_wm_window_type};
    XChangeProperty(dpy, root, net_supported, XA_ATOM, 32, PropModeReplace, (unsigned char *)supported,
                    (int)(sizeof(supported) / sizeof(supported[0])));
    long one = 1, zero = 0;
    XChangeProperty(dpy, root, net_number_desktops, XA_CARDINAL, 32, PropModeReplace, (unsigned char *)&one, 1);
    XChangeProperty(dpy, root, net_current_desktop, XA_CARDINAL, 32, PropModeReplace, (unsigned char *)&zero, 1);
}

void WM::ewmh_update()
{
    // Work area is the primary monitor minus the taskbar. Clients that honor
    // _NET_WORKAREA (few on this image) will avoid covering the bar.
    long geom[2] = {sw, sh};
    XChangeProperty(dpy, root, net_desktop_geometry, XA_CARDINAL, 32, PropModeReplace, (unsigned char *)geom, 2);
    long wa[4] = {0, 0, sw, sh - w95::kTaskbarH};
    if (!mons.empty()) {
        Monitor *p = &mons[0];
        for (auto &m : mons)
            if (m.primary)
                p = &m;
        wa[0] = p->x;
        wa[1] = p->y;
        wa[2] = p->w;
        wa[3] = p->h - w95::kTaskbarH;
    }
    XChangeProperty(dpy, root, net_workarea, XA_CARDINAL, 32, PropModeReplace, (unsigned char *)wa, 4);
    std::vector<Window> wins;
    for (auto &c : clients)
        if (!c->iconic)
            wins.push_back(c->win);
    Window none = None;
    XChangeProperty(dpy, root, net_client_list, XA_WINDOW, 32, PropModeReplace,
                    wins.empty() ? (unsigned char *)&none : (unsigned char *)wins.data(), (int)wins.size());
    Window aw = focused ? focused->win : None;
    XChangeProperty(dpy, root, net_active, XA_WINDOW, 32, PropModeReplace, (unsigned char *)&aw, 1);
}

// Run cmd via /bin/sh in a new session. SIGCHLD is ignored in init() so
// we never wait; "|| true" in callers keeps the child from looking failed.
void WM::launch(const char *cmd)
{
    if (fork() == 0) {
        setsid();
        execl("/bin/sh", "sh", "-c", cmd, (char *)nullptr);
        _exit(127);
    }
}

std::string WM::get_name(Window w)
{
    // Prefer UTF-8 EWMH title; fall back to ICCCM WM_NAME; else "Untitled".
    Atom type;
    int fmt;
    unsigned long n, extra;
    unsigned char *prop = nullptr;
    if (XGetWindowProperty(dpy, w, net_wm_name, 0, 256, False, utf8, &type, &fmt, &n, &extra, &prop) == Success && prop &&
        n) {
        std::string s((char *)prop, strnlen((char *)prop, n));
        XFree(prop);
        if (!s.empty())
            return s;
    }
    XTextProperty tp{};
    if (XGetWMName(dpy, w, &tp) && tp.value) {
        std::string s((char *)tp.value);
        XFree(tp.value);
        if (!s.empty())
            return s;
    }
    return "Untitled";
}

bool WM::has_proto(Window w, Atom a)
{
    // True if the client listed `a` in WM_PROTOCOLS (e.g. WM_DELETE_WINDOW).
    Atom *protos = nullptr;
    int n = 0;
    if (!XGetWMProtocols(dpy, w, &protos, &n) || !protos)
        return false;
    bool ok = false;
    for (int i = 0; i < n; i++)
        if (protos[i] == a)
            ok = true;
    XFree(protos);
    return ok;
}

void WM::send_client_message(Window w, Atom type, long a, long b, long c)
{
    // 32-bit ClientMessage with up to three longs (WM_PROTOCOLS, EWMH, …).
    XEvent ev{};
    ev.type = ClientMessage;
    ev.xclient.window = w;
    ev.xclient.message_type = type;
    ev.xclient.format = 32;
    ev.xclient.data.l[0] = a;
    ev.xclient.data.l[1] = b;
    ev.xclient.data.l[2] = c;
    XSendEvent(dpy, w, False, NoEventMask, &ev);
}

// RandR active monitors first, then Xinerama, then a single root-sized head.
// Xfbdev on the live ISO has no RandR outputs, so we almost always hit the
// last fallback there.
std::vector<Monitor> WM::query_monitors()
{
    std::vector<Monitor> out;
    if (have_randr) {
        int n = 0;
        XRRMonitorInfo *info = XRRGetMonitors(dpy, root, True, &n);
        if (info && n > 0) {
            for (int i = 0; i < n; i++) {
                if (info[i].width <= 0 || info[i].height <= 0)
                    continue;
                Monitor m;
                m.x = info[i].x;
                m.y = info[i].y;
                m.w = info[i].width;
                m.h = info[i].height;
                m.primary = info[i].primary;
                out.push_back(m);
            }
            XRRFreeMonitors(info);
        }
    }
    if (out.empty() && XineramaIsActive(dpy)) {
        int n = 0;
        XineramaScreenInfo *xs = XineramaQueryScreens(dpy, &n);
        if (xs && n > 0) {
            for (int i = 0; i < n; i++) {
                Monitor m;
                m.x = xs[i].x_org;
                m.y = xs[i].y_org;
                m.w = xs[i].width;
                m.h = xs[i].height;
                m.primary = (i == 0);
                out.push_back(m);
            }
            XFree(xs);
        }
    }
    if (out.empty()) {
        Monitor m;
        m.x = 0;
        m.y = 0;
        m.w = sw;
        m.h = sh;
        m.primary = true;
        out.push_back(m);
    }
    bool any = false;
    for (auto &m : out)
        any = any || m.primary;
    if (!any)
        out[0].primary = true;
    return out;
}

void WM::create_shell(Monitor &m)
{
    // Desktop fills the work area; taskbar sits on the bottom edge. Menus are
    // created off-screen-ish and mapped when opened. override_redirect on all.
    long dmask = ExposureMask | ButtonPressMask | ButtonReleaseMask | ButtonMotionMask | KeyPressMask;
    long tmask = ExposureMask | ButtonPressMask | ButtonReleaseMask | PointerMotionMask | LeaveWindowMask;
    long mmask = ExposureMask | ButtonPressMask | PointerMotionMask | LeaveWindowMask | EnterWindowMask | KeyPressMask |
                 KeyReleaseMask;
    m.desktop = mkwin(m.x, m.y, m.w, m.h - w95::kTaskbarH, desktop.pix, dmask, true);
    m.taskbar = mkwin(m.x, m.y + m.h - w95::kTaskbarH, m.w, w95::kTaskbarH, face.pix, tmask, true);
    int mw = w95::kBannerW + w95::kMenuBodyW;
    int mh = menu_height();
    m.startmenu = mkwin(m.x + 2, m.y + m.h - w95::kTaskbarH - mh, mw, mh, face.pix, mmask, true);
    m.submenu = mkwin(0, 0, 150, 8 + 3 * w95::kMenuItemH, face.pix, mmask, true);
    XChangeProperty(dpy, m.desktop, net_wm_window_type, XA_ATOM, 32, PropModeReplace, (unsigned char *)&net_type_desktop,
                    1);
    XChangeProperty(dpy, m.taskbar, net_wm_window_type, XA_ATOM, 32, PropModeReplace, (unsigned char *)&net_type_dock, 1);
    XMapWindow(dpy, m.desktop);
    XMapWindow(dpy, m.taskbar);
}

void WM::destroy_shell(Monitor &m)
{
    // Submenu first so we don't leave an orphan mapped after the Start menu dies.
    if (m.submenu)
        XDestroyWindow(dpy, m.submenu);
    if (m.startmenu)
        XDestroyWindow(dpy, m.startmenu);
    if (m.taskbar)
        XDestroyWindow(dpy, m.taskbar);
    if (m.desktop)
        XDestroyWindow(dpy, m.desktop);
    m.submenu = m.startmenu = m.taskbar = m.desktop = 0;
}

void WM::sync_monitors()
{
    // Called at startup and on RRScreenChangeNotify. If geometry is unchanged
    // we only restack; otherwise rebuild shell windows and clamp clients.
    sw = DisplayWidth(dpy, screen);
    sh = DisplayHeight(dpy, screen);
    auto geoms = query_monitors();
    bool same = geoms.size() == mons.size();
    if (same) {
        for (size_t i = 0; i < geoms.size(); i++) {
            if (geoms[i].x != mons[i].x || geoms[i].y != mons[i].y || geoms[i].w != mons[i].w || geoms[i].h != mons[i].h)
                same = false;
        }
    }
    if (same) {
        restack_shell();
        ewmh_update();
        return;
    }
    close_menus();
    tray_stash();
    for (auto &m : mons)
        destroy_shell(m);
    mons = std::move(geoms);
    for (auto &m : mons)
        create_shell(m);
    for (auto &c : clients) {
        if (c->maxed || c->tiled) {
            Snap s = c->maxed ? Snap::Top : c->snap;
            if (s == Snap::Off)
                s = Snap::Top;
            apply_snap(c.get(), s, c->x + c->w / 2, c->y + 8);
        } else {
            int mi = monitor_at(c->x + 16, c->y + 16);
            Monitor &mon = mons[mi];
            if (c->x + 80 > mon.x + mon.w)
                c->x = mon.x + mon.w - 80;
            if (c->y + 40 > mon.y + mon.h - w95::kTaskbarH)
                c->y = mon.y + mon.h - w95::kTaskbarH - 40;
            if (c->x < mon.x)
                c->x = mon.x;
            if (c->y < mon.y)
                c->y = mon.y;
            apply_geom(c.get());
        }
        c->last_mon = monitor_for(c.get());
    }
    restack_shell();
    tray_create();
    for (auto &m : mons) {
        draw_desktop(m);
        draw_taskbar(m);
    }
    ewmh_update();
}

void WM::restack_shell()
{
    // Desktop always at the bottom, taskbar always on top of clients, then
    // menus and modal dialogs. Raise order matters for click-through.
    for (auto &m : mons)
        if (m.desktop)
            XLowerWindow(dpy, m.desktop);
    for (auto &m : mons)
        if (m.taskbar)
            XRaiseWindow(dpy, m.taskbar);
    for (auto &m : mons) {
        if (m.start_open)
            XRaiseWindow(dpy, m.startmenu);
        if (m.sub_open)
            XRaiseWindow(dpy, m.submenu);
    }
    if (run_open)
        XRaiseWindow(dpy, rundlg);
    if (shut_open)
        XRaiseWindow(dpy, shutdlg);
    if (set_open)
        XRaiseWindow(dpy, setdlg);
    if (color_open)
        XRaiseWindow(dpy, colordlg);
    if (file_open)
        XRaiseWindow(dpy, filedlg);
}

int WM::monitor_at(int x, int y)
{
    // Point-in-rect, then nearest center if the pointer is in a gap/overlap.
    for (size_t i = 0; i < mons.size(); i++) {
        Monitor &m = mons[i];
        if (x >= m.x && y >= m.y && x < m.x + m.w && y < m.y + m.h)
            return (int)i;
    }
    int best = 0;
    long bestd = 1L << 30;
    for (size_t i = 0; i < mons.size(); i++) {
        Monitor &m = mons[i];
        int cx = m.x + m.w / 2, cy = m.y + m.h / 2;
        long d = (long)(cx - x) * (cx - x) + (long)(cy - y) * (cy - y);
        if (d < bestd) {
            bestd = d;
            best = (int)i;
        }
    }
    return best;
}

int WM::monitor_for(Client *c)
{
    // Iconic windows keep last_mon so their task button stays on the old head.
    if (!c)
        return 0;
    if (c->iconic)
        return std::max(0, std::min(c->last_mon, (int)mons.size() - 1));
    return monitor_at(c->x + c->w / 2, c->y + 8);
}

Monitor *WM::mon_by_window(Window w)
{
    // Desktop, taskbar, Start menu, or Programs flyout belonging to a head.
    for (auto &m : mons) {
        if (m.desktop == w || m.taskbar == w || m.startmenu == w || m.submenu == w)
            return &m;
    }
    return nullptr;
}

int WM::mon_index(const Monitor *m)
{
    for (size_t i = 0; i < mons.size(); i++)
        if (&mons[i] == m)
            return (int)i;
    return 0;
}

Monitor *WM::primary_mon()
{
    if (mons.empty())
        return nullptr;
    for (auto &m : mons)
        if (m.primary)
            return &m;
    return &mons[0];
}

int WM::pointer_mon()
{
    Window rr, cr;
    int rx, ry, wx, wy;
    unsigned mask;
    XQueryPointer(dpy, root, &rr, &cr, &rx, &ry, &wx, &wy, &mask);
    return monitor_at(rx, ry);
}

static void desk_cell(int i, int &x, int &y, int &w, int &h)
{
    x = 12;
    y = 12 + i * w95::kCellH;
    w = w95::kCellW;
    h = w95::kCellH;
}

static bool rects_hit(int ax, int ay, int aw, int ah, int bx, int by, int bw, int bh)
{
    return ax < bx + bw && bx < ax + aw && ay < by + bh && by < ay + ah;
}

void WM::desk_select_only(int i)
{
    if (i < 0 || i > 3) {
        selected_icon = -1;
        desk_mask = 0;
        return;
    }
    selected_icon = i;
    desk_mask = 1u << i;
}

void WM::select_desk_icons(int x0, int y0, int x1, int y1, unsigned base)
{
    int rx = std::min(x0, x1), ry = std::min(y0, y1);
    int rw = std::abs(x1 - x0), rh = std::abs(y1 - y0);
    unsigned m = base;
    int last = selected_icon;
    for (int i = 0; i < 4; i++) {
        int ix, iy, iw, ih;
        desk_cell(i, ix, iy, iw, ih);
        if (rects_hit(rx, ry, rw, rh, ix, iy, iw, ih)) {
            m |= 1u << i;
            last = i;
        }
    }
    desk_mask = m;
    if (m)
        selected_icon = last;
}

void WM::activate_desk_sel()
{
    if (!desk_mask && selected_icon >= 0)
        activate_desk_icon(selected_icon);
    else {
        for (int i = 0; i < 4; i++)
            if (desk_mask & (1u << i))
                activate_desk_icon(i);
    }
}

void WM::refresh_modes()
{
    modes.clear();
    auto add = [&](int w, int h) {
        if (w < 320 || h < 200 || w > 7680 || h > 4320)
            return;
        for (auto &m : modes)
            if (m.w == w && m.h == h)
                return;
        modes.push_back({w, h});
    };
    int nsz = 0;
    XRRScreenSize *ss = XRRSizes(dpy, screen, &nsz);
    if (ss) {
        for (int i = 0; i < nsz; i++)
            add(ss[i].width, ss[i].height);
    }
    if (have_randr) {
        XRRScreenResources *sr = XRRGetScreenResourcesCurrent(dpy, root);
        if (sr) {
            for (int i = 0; i < sr->nmode; i++)
                add((int)sr->modes[i].width, (int)sr->modes[i].height);
            XRRFreeScreenResources(sr);
        }
    }
    static const int kCommon[][2] = {{640, 480},   {800, 600},   {1024, 768},  {1280, 720},  {1280, 800},
                                     {1280, 1024}, {1366, 768},  {1440, 900},  {1600, 900},  {1600, 1200},
                                     {1920, 1080}, {1920, 1200}};
    for (auto &c : kCommon)
        add(c[0], c[1]);
    add(sw, sh);
    std::sort(modes.begin(), modes.end(), [](const VideoMode &a, const VideoMode &b) {
        return a.w != b.w ? a.w < b.w : a.h < b.h;
    });
    mode_i = 0;
    for (int i = 0; i < (int)modes.size(); i++)
        if (modes[i].w == sw && modes[i].h == sh)
            mode_i = i;
    mode_scroll = 0;
    mode_note.clear();
}

bool WM::apply_mode(int i)
{
    if (i < 0 || i >= (int)modes.size())
        return false;
    int w = modes[i].w, h = modes[i].h;
    mode_i = i;
    const char *home = getenv("HOME");
    if (home && *home) {
        mkdir((std::string(home) + "/.chime").c_str(), 0755);
        std::string path = std::string(home) + "/.chime/xmode";
        FILE *f = fopen(path.c_str(), "w");
        if (f) {
            std::fprintf(f, "%dx%d\n", w, h);
            fclose(f);
        }
    }
    int nsz = 0;
    XRRScreenSize *ss = XRRSizes(dpy, screen, &nsz);
    if (ss) {
        for (int s = 0; s < nsz; s++) {
            if (ss[s].width != w || ss[s].height != h)
                continue;
            XRRScreenConfiguration *sc = XRRGetScreenInfo(dpy, root);
            if (!sc)
                break;
            Rotation rot = RR_Rotate_0;
            XRRConfigCurrentConfiguration(sc, &rot);
            Status st = XRRSetScreenConfig(dpy, sc, root, (int)s, rot, CurrentTime);
            XRRFreeScreenConfigInfo(sc);
            if (st == Success) {
                mode_note.clear();
                return true;
            }
        }
    }
    char cmd[160];
    std::snprintf(cmd, sizeof cmd, "xrandr -s %dx%d >/dev/null 2>&1", w, h);
    if (system(cmd) == 0) {
        mode_note.clear();
        return true;
    }
    mode_note = "Saved. Log out to apply on this display.";
    return false;
}

int WM::tray_width(const Monitor &m)
{
    // Only the primary bar grows for docked icons; other bars stay clock-sized.
    int w = w95::kClockW;
    if (m.primary && !tray_icons.empty())
        w += 4 + (int)tray_icons.size() * w95::kTraySlot;
    return w;
}

bool WM::is_tray_icon(Window w)
{
    for (Window t : tray_icons)
        if (t == w)
            return true;
    return false;
}

void WM::tray_send_xembed(Window w, long msg, long detail, long d1, long d2)
{
    // XEmbed client message: l[1] is the opcode (0 = EMBEDDED_NOTIFY).
    XEvent ev{};
    ev.xclient.type = ClientMessage;
    ev.xclient.window = w;
    ev.xclient.message_type = xembed;
    ev.xclient.format = 32;
    ev.xclient.data.l[0] = CurrentTime;
    ev.xclient.data.l[1] = msg;
    ev.xclient.data.l[2] = detail;
    ev.xclient.data.l[3] = d1;
    ev.xclient.data.l[4] = d2;
    XSendEvent(dpy, w, False, NoEventMask, &ev);
}

void WM::tray_stash()
{
    // Park icons on the root while we destroy/recreate traywin on monitor sync.
    for (Window w : tray_icons) {
        XSelectInput(dpy, w, StructureNotifyMask);
        XReparentWindow(dpy, w, root, 0, 0);
        XUnmapWindow(dpy, w);
    }
    traywin = 0;
}

void WM::tray_claim()
{
    // Own _NET_SYSTEM_TRAY_S<n> and broadcast MANAGER so tray clients (volicon)
    // can find us. Launch volicon once after we hold the selection.
    if (!traywin)
        return;
    long orient = 0;
    XChangeProperty(dpy, traywin, net_system_tray_orientation, XA_CARDINAL, 32, PropModeReplace,
                    (unsigned char *)&orient, 1);
    VisualID vid = XVisualIDFromVisual(vis);
    XChangeProperty(dpy, traywin, net_system_tray_visual, XA_VISUALID, 32, PropModeReplace, (unsigned char *)&vid, 1);
    XSetSelectionOwner(dpy, net_system_tray, traywin, CurrentTime);
    if (XGetSelectionOwner(dpy, net_system_tray) != traywin)
        return;
    XEvent ev{};
    ev.xclient.type = ClientMessage;
    ev.xclient.window = root;
    ev.xclient.message_type = manager;
    ev.xclient.format = 32;
    ev.xclient.data.l[0] = CurrentTime;
    ev.xclient.data.l[1] = (long)net_system_tray;
    ev.xclient.data.l[2] = (long)traywin;
    XSendEvent(dpy, root, False, StructureNotifyMask, &ev);
    if (!volicon_launched) {
        volicon_launched = true;
        launch("volicon || true");
    }
}

void WM::tray_create()
{
    // Child of the primary taskbar, right-aligned. Reparents any stashed icons.
    Monitor *p = primary_mon();
    if (!p || !p->taskbar)
        return;
    int tw = std::max(w95::kClockW, tray_width(*p));
    int th = w95::kTaskbarH - 6;
    int x = p->w - tw - 4;
    XSetWindowAttributes swa{};
    swa.background_pixel = face.pix;
    swa.event_mask = ExposureMask | SubstructureNotifyMask | SubstructureRedirectMask;
    swa.colormap = cmap;
    swa.border_pixel = 0;
    swa.cursor = cur_left;
    traywin = XCreateWindow(dpy, p->taskbar, x, 3, (unsigned)tw, (unsigned)th, 0, depth, InputOutput, vis,
                            CWBackPixel | CWEventMask | CWColormap | CWBorderPixel | CWCursor, &swa);
    XMapWindow(dpy, traywin);
    for (Window w : tray_icons) {
        XReparentWindow(dpy, w, traywin, 0, 0);
        XMapWindow(dpy, w);
    }
    tray_claim();
    tray_layout();
}

void WM::tray_layout()
{
    // Resize the well, then pack icons left-to-right and the clock on the right.
    Monitor *p = primary_mon();
    if (!p || !traywin || !p->taskbar)
        return;
    int tw = tray_width(*p);
    int th = w95::kTaskbarH - 6;
    int x = p->w - tw - 4;
    XMoveResizeWindow(dpy, traywin, x, 3, (unsigned)tw, (unsigned)th);
    int ix = 3;
    int iy = (th - w95::kTrayIcon) / 2;
    for (Window w : tray_icons) {
        XMoveResizeWindow(dpy, w, ix, iy, w95::kTrayIcon, w95::kTrayIcon);
        XMapWindow(dpy, w);
        ix += w95::kTraySlot;
    }
    draw_tray();
    draw_taskbar(*p);
}

void WM::tray_dock(Window w)
{
    // SYSTEM_TRAY_REQUEST_DOCK. If the window was already managed as a client
    // (rare), drop the frame first so it becomes a 20x20 child of traywin.
    if (!w || w == traywin || is_internal(w) || is_tray_icon(w))
        return;
    if (Client *c = find_client(w))
        unmanage(c, false);
    if (!traywin)
        tray_create();
    if (!traywin)
        return;
    XSelectInput(dpy, w, StructureNotifyMask | PropertyChangeMask);
    XAddToSaveSet(dpy, w);
    XSetWindowBorderWidth(dpy, w, 0);
    XReparentWindow(dpy, w, traywin, 0, 0);
    XMapRaised(dpy, w);
    tray_send_xembed(w, 0, 0, (long)traywin, 0);
    tray_icons.push_back(w);
    tray_layout();
}

void WM::tray_undock(Window w)
{
    auto it = std::find(tray_icons.begin(), tray_icons.end(), w);
    if (it == tray_icons.end())
        return;
    tray_icons.erase(it);
    tray_layout();
}

bool WM::is_internal(Window w)
{
    // Anything we created (chrome, dialogs, tray) must not go through manage().
    if (w == rundlg || w == shutdlg || w == setdlg || w == checkwin || w == snapwin || w == traywin || w == root ||
        w == colordlg || w == filedlg)
        return true;
    return mon_by_window(w) != nullptr || is_tray_icon(w);
}

Client *WM::find_client(Window w)
{
    // Match the application's window, not the frame we wrapped it in.
    for (auto &c : clients)
        if (c->win == w)
            return c.get();
    return nullptr;
}

Client *WM::find_frame(Window w)
{
    for (auto &c : clients)
        if (c->frame == w)
            return c.get();
    return nullptr;
}

std::vector<Client *> WM::clients_on(int mi)
{
    // Taskbar buttons and Alt+Tab cycle only windows whose center is on this head.
    std::vector<Client *> out;
    for (auto &c : clients)
        if (monitor_for(c.get()) == mi)
            out.push_back(c.get());
    return out;
}

void WM::set_wm_state(Client *c, long state)
{
    // ICCCM WM_STATE: NormalState or IconicState. Second long is unused icon window.
    long data[2] = {state, None};
    XChangeProperty(dpy, c->win, wm_state, wm_state, 32, PropModeReplace, (unsigned char *)data, 2);
}

void WM::send_configure(Client *c)
{
    // Synthetic ConfigureNotify with the client (inner) geometry, as ICCCM
    // requires after we reparent/resize.
    XConfigureEvent ce{};
    ce.type = ConfigureNotify;
    ce.event = c->win;
    ce.window = c->win;
    ce.x = c->x + w95::kFrameB;
    ce.y = c->y + w95::kFrameB + w95::kTitleH;
    ce.width = c->w - 2 * w95::kFrameB;
    ce.height = c->h - 2 * w95::kFrameB - w95::kTitleH;
    ce.border_width = 0;
    ce.above = None;
    ce.override_redirect = False;
    XSendEvent(dpy, c->win, False, StructureNotifyMask, (XEvent *)&ce);
}

void WM::apply_geom(Client *c)
{
    // Outer frame on root, inner client inset by bevel + caption.
    if (c->w < 80)
        c->w = 80;
    if (c->h < 50)
        c->h = 50;
    XMoveResizeWindow(dpy, c->frame, c->x, c->y, (unsigned)c->w, (unsigned)c->h);
    int cw = c->w - 2 * w95::kFrameB;
    int ch = c->h - 2 * w95::kFrameB - w95::kTitleH;
    if (cw < 1)
        cw = 1;
    if (ch < 1)
        ch = 1;
    XMoveResizeWindow(dpy, c->win, w95::kFrameB, w95::kFrameB + w95::kTitleH, (unsigned)cw, (unsigned)ch);
    send_configure(c);
    draw_frame(c);
}

void WM::manage(Window w)
{
    // Reparent a MapRequest into a decorated frame. Position is a cascade on
    // the monitor that currently has the pointer. override_redirect windows
    // (menus, dmenu, our own chrome) are left alone.
    if (is_internal(w) || find_client(w) || find_frame(w) || is_tray_icon(w) || w == traywin)
        return;
    XWindowAttributes wa{};
    if (!XGetWindowAttributes(dpy, w, &wa))
        return;
    if (wa.override_redirect)
        return;

    auto c = std::make_unique<Client>();
    c->win = w;
    int cw = std::max(wa.width, 120);
    int ch = std::max(wa.height, 80);
    Window rr, cr;
    int rx, ry, wx, wy;
    unsigned mask;
    XQueryPointer(dpy, root, &rr, &cr, &rx, &ry, &wx, &wy, &mask);
    int mi = monitor_at(rx, ry);
    Monitor &mon = mons[mi];
    int step = cascade[mi]++ % 8;
    c->w = cw + 2 * w95::kFrameB;
    c->h = ch + 2 * w95::kFrameB + w95::kTitleH;
    c->x = mon.x + 24 + step * 22;
    c->y = mon.y + 24 + step * 22;
    if (c->x + c->w > mon.x + mon.w)
        c->x = mon.x + 16;
    if (c->y + c->h > mon.y + mon.h - w95::kTaskbarH)
        c->y = mon.y + 16;
    c->last_mon = mi;
    c->name = get_name(w);

    long emask = ExposureMask | ButtonPressMask | ButtonReleaseMask | ButtonMotionMask | PointerMotionMask |
                 SubstructureNotifyMask | SubstructureRedirectMask;
    c->frame = mkwin(c->x, c->y, c->w, c->h, face.pix, emask, false);
    XAddToSaveSet(dpy, w);
    XSetWindowBorderWidth(dpy, w, 0);
    XReparentWindow(dpy, w, c->frame, w95::kFrameB, w95::kFrameB + w95::kTitleH);
    XSelectInput(dpy, w, PropertyChangeMask | StructureNotifyMask);
    // Sync grab: we see the click first, raise/focus, then ReplayPointer.
    XGrabButton(dpy, AnyButton, AnyModifier, w, False, ButtonPressMask, GrabModeSync, GrabModeAsync, None, None);
    XResizeWindow(dpy, w, (unsigned)(c->w - 2 * w95::kFrameB), (unsigned)(c->h - 2 * w95::kFrameB - w95::kTitleH));
    XMapWindow(dpy, w);
    XMapWindow(dpy, c->frame);
    set_wm_state(c.get(), NormalState);
    Client *ptr = c.get();
    clients.push_back(std::move(c));
    raise_client(ptr);
    focus(ptr);
    draw_frame(ptr);
    for (auto &m : mons)
        draw_taskbar(m);
    ewmh_update();
}

void WM::unmanage(Client *c, bool destroyed)
{
    // destroyed==true: the client already vanished; don't reparent or ungrab.
    if (!c)
        return;
    if (focused == c)
        focused = nullptr;
    if (drag_c == c) {
        hide_snap_preview();
        drag = DragMode::Off;
        drag_c = nullptr;
        XUngrabPointer(dpy, CurrentTime);
    }
    if (!destroyed) {
        XUngrabButton(dpy, AnyButton, AnyModifier, c->win);
        XReparentWindow(dpy, c->win, root, c->x, c->y);
        XRemoveFromSaveSet(dpy, c->win);
    }
    XDestroyWindow(dpy, c->frame);
    clients.erase(std::remove_if(clients.begin(), clients.end(), [&](auto &p) { return p.get() == c; }), clients.end());
    if (!focused && !clients.empty())
        focus(clients.back().get());
    else if (!focused)
        ungrab_if_idle();
    for (auto &m : mons)
        draw_taskbar(m);
    ewmh_update();
}

void WM::focus(Client *c)
{
    // Click-to-focus: the last click target owns both the keyboard and the
    // active caption. Shell chrome (Start, desktop, dialogs) is routed in
    // ungrab_if_idle() after we update `focused`.
    Client *old = focused;
    focused = c;
    if (old && old != c)
        draw_frame(old);
    if (c && !c->iconic) {
        draw_frame(c);
        c->last_mon = monitor_for(c);
    }
    for (auto &m : mons)
        draw_taskbar(m);
    ewmh_update();
    ungrab_if_idle();
    if (c && !c->iconic && has_proto(c->win, wm_take_focus))
        send_client_message(c->win, wm_protocols, (long)wm_take_focus, CurrentTime);
}

void WM::raise_client(Client *c)
{
    // Raise the frame, then restack_shell so the taskbar stays above it.
    if (!c)
        return;
    XRaiseWindow(dpy, c->frame);
    restack_shell();
}

void WM::close_client(Client *c)
{
    // Prefer WM_DELETE_WINDOW so apps can save; XKillClient is the last resort.
    if (!c)
        return;
    if (has_proto(c->win, wm_delete))
        send_client_message(c->win, wm_protocols, (long)wm_delete, CurrentTime);
    else
        XKillClient(dpy, c->win);
}

void WM::minimize(Client *c)
{
    // ignore_unmap: the UnmapNotify from XUnmapWindow must not unmanage us.
    if (!c || c->iconic)
        return;
    c->last_mon = monitor_for(c);
    c->iconic = true;
    c->ignore_unmap++;
    XUnmapWindow(dpy, c->frame);
    set_wm_state(c, IconicState);
    if (focused == c)
        focus(nullptr);
    for (auto &m : mons)
        draw_taskbar(m);
}

void WM::restore(Client *c)
{
    // Map the frame again and give it focus. Counterpart to minimize().
    if (!c)
        return;
    c->iconic = false;
    XMapWindow(dpy, c->frame);
    set_wm_state(c, NormalState);
    raise_client(c);
    focus(c);
}

void WM::remember_float(Client *c)
{
    // Only snapshot if we are not already snapped; otherwise rx/ry would be the tile.
    if (!c || c->maxed || c->tiled)
        return;
    c->rx = c->x;
    c->ry = c->y;
    c->rw = c->w;
    c->rh = c->h;
}

void WM::float_for_drag(Client *c, int px, int py)
{
    // Pull a maximized/tiled window back to its remembered size, keeping the
    // pointer over the same relative X so the caption doesn't jump.
    if (!c || (!c->maxed && !c->tiled))
        return;
    int rw = c->rw > 80 ? c->rw : 320;
    int rh = c->rh > 50 ? c->rh : 240;
    int relx = rw / 2;
    if (c->w > 16) {
        relx = px - c->x;
        if (relx < 0)
            relx = 0;
        if (relx > c->w)
            relx = c->w;
        relx = (int)((long)relx * rw / c->w);
    }
    c->w = rw;
    c->h = rh;
    c->x = px - relx;
    c->y = py - (w95::kFrameB + w95::kTitleH / 2);
    c->maxed = false;
    c->tiled = false;
    c->snap = Snap::Off;
    apply_geom(c);
}

Snap WM::snap_at(int px, int py)
{
    // Corners beat edges. The taskbar strip counts as "bottom" so you can
    // snap BL/BR without hunting the last 24px of the work area.
    if (mons.empty())
        return Snap::Off;
    Monitor &m = mons[monitor_at(px, py)];
    int x = m.x, y = m.y, w = m.w, h = m.h - w95::kTaskbarH;
    const int E = w95::kSnapEdge;
    bool L = px < x + E;
    bool R = px >= x + w - E;
    bool T = py < y + E;
    bool B = py >= y + h - E || py >= m.y + m.h - w95::kTaskbarH;
    if (L && T)
        return Snap::TL;
    if (R && T)
        return Snap::TR;
    if (L && B)
        return Snap::BL;
    if (R && B)
        return Snap::BR;
    if (L)
        return Snap::Left;
    if (R)
        return Snap::Right;
    if (T)
        return Snap::Top;
    return Snap::Off;
}

void WM::snap_rect(Snap s, int px, int py, int &x, int &y, int &w, int &h)
{
    // Work area of the monitor under the pointer, then split into half/quarter.
    Monitor &m = mons[monitor_at(px, py)];
    x = m.x;
    y = m.y;
    w = m.w;
    h = m.h - w95::kTaskbarH;
    int hw = w / 2;
    int hh = h / 2;
    switch (s) {
    case Snap::Left:
        w = hw;
        break;
    case Snap::Right:
        x += hw;
        w -= hw;
        break;
    case Snap::TL:
        w = hw;
        h = hh;
        break;
    case Snap::TR:
        x += hw;
        w -= hw;
        h = hh;
        break;
    case Snap::BL:
        y += hh;
        w = hw;
        h -= hh;
        break;
    case Snap::BR:
        x += hw;
        y += hh;
        w -= hw;
        h -= hh;
        break;
    case Snap::Top:
    case Snap::Off:
        break;
    }
}

void WM::apply_snap(Client *c, Snap s, int px, int py)
{
    // Top => maxed; other edges => tiled. Both remember the previous float size.
    if (!c || s == Snap::Off || mons.empty())
        return;
    remember_float(c);
    int x, y, w, h;
    snap_rect(s, px, py, x, y, w, h);
    c->x = x;
    c->y = y;
    c->w = w;
    c->h = h;
    c->snap = s;
    c->maxed = (s == Snap::Top);
    c->tiled = (s != Snap::Top);
    apply_geom(c);
    c->last_mon = monitor_for(c);
    for (auto &mon : mons)
        draw_taskbar(mon);
}

void WM::show_snap_preview(Snap s, int px, int py)
{
    // Overlay a fake captioned window; keep the dragged frame and taskbars above it.
    if (!snapwin || s == Snap::Off) {
        hide_snap_preview();
        return;
    }
    int x, y, w, h;
    snap_rect(s, px, py, x, y, w, h);
    if (w < 8)
        w = 8;
    if (h < 8)
        h = 8;
    XMoveResizeWindow(dpy, snapwin, x, y, (unsigned)w, (unsigned)h);
    XMapWindow(dpy, snapwin);
    XRaiseWindow(dpy, snapwin);
    if (drag_c)
        XRaiseWindow(dpy, drag_c->frame);
    for (auto &m : mons)
        if (m.taskbar)
            XRaiseWindow(dpy, m.taskbar);
    draw_snap_preview();
}

void WM::hide_snap_preview()
{
    if (snapwin)
        XUnmapWindow(dpy, snapwin);
}

void WM::draw_snap_preview()
{
    // Mini framed window: navy fill, raised bevel, fake caption bar.
    if (!snapwin)
        return;
    XWindowAttributes wa{};
    if (!XGetWindowAttributes(dpy, snapwin, &wa) || wa.map_state == IsUnmapped)
        return;
    int w = wa.width, h = wa.height;
    fill(snapwin, 0, 0, w, h, title.pix);
    bevel(snapwin, 0, 0, w, h, true);
    if (w > 16 && h > 16) {
        fill(snapwin, 4, 4, w - 8, h - 8, face.pix);
        bevel(snapwin, 4, 4, w - 8, h - 8, false);
        fill(snapwin, 6, 6, w - 12, w95::kTitleH, title.pix);
    }
}

void WM::maximize_toggle(Client *c)
{
    // Restore uses the stored float rect; maximize snaps to Top on the pointer's head.
    if (!c)
        return;
    if (c->maxed) {
        c->x = c->rx;
        c->y = c->ry;
        c->w = c->rw;
        c->h = c->rh;
        c->maxed = false;
        c->tiled = false;
        c->snap = Snap::Off;
        apply_geom(c);
        c->last_mon = monitor_for(c);
    } else {
        Window rr, cr;
        int px, py, wx, wy;
        unsigned mask;
        XQueryPointer(dpy, root, &rr, &cr, &px, &py, &wx, &wy, &mask);
        apply_snap(c, Snap::Top, px, py);
    }
    for (auto &mon : mons)
        draw_taskbar(mon);
}

Hit WM::hit_frame(Client *c, int x, int y)
{
    // Coordinates are relative to the frame. Corners get a 12px extra grab.
    const int B = w95::kFrameB;
    const int T = w95::kTitleH;
    if (y < B) {
        if (x < B + 12)
            return Hit::EdgeNW;
        if (x >= c->w - B - 12)
            return Hit::EdgeNE;
        return Hit::EdgeN;
    }
    if (y >= c->h - B) {
        if (x < B + 12)
            return Hit::EdgeSW;
        if (x >= c->w - B - 12)
            return Hit::EdgeSE;
        return Hit::EdgeS;
    }
    if (x < B)
        return Hit::EdgeW;
    if (x >= c->w - B)
        return Hit::EdgeE;
    if (y < B + T) {
        int bx = c->w - B - 2 - w95::kBtn;
        int by = B + (T - w95::kBtnH) / 2;
        auto inbtn = [&](int bx0) {
            return x >= bx0 && x < bx0 + w95::kBtn && y >= by && y < by + w95::kBtnH;
        };
        if (inbtn(bx))
            return Hit::Close;
        bx -= w95::kBtn + 2;
        if (inbtn(bx))
            return Hit::Max;
        bx -= w95::kBtn + 2;
        if (inbtn(bx))
            return Hit::Min;
        if (x < B + 18)
            return Hit::Sys;
        return Hit::Title;
    }
    return Hit::Miss;
}

Cursor WM::cursor_for(Hit h)
{
    // Title uses fleur (move); edges use the matching XC_* font cursor.
    switch (h) {
    case Hit::EdgeN:
        return cur_n;
    case Hit::EdgeS:
        return cur_s;
    case Hit::EdgeE:
        return cur_e;
    case Hit::EdgeW:
        return cur_w;
    case Hit::EdgeNE:
        return cur_ne;
    case Hit::EdgeNW:
        return cur_nw;
    case Hit::EdgeSE:
        return cur_se;
    case Hit::EdgeSW:
        return cur_sw;
    case Hit::Title:
        return cur_move;
    default:
        return cur_left;
    }
}

void WM::close_menus()
{
    // Unmap Start + Programs on every head. Hover state is reset so the next
    // open does not paint a stale highlight.
    bool had = false;
    for (auto &m : mons) {
        if (m.start_open || m.sub_open)
            had = true;
        if (m.start_open)
            XUnmapWindow(dpy, m.startmenu);
        if (m.sub_open)
            XUnmapWindow(dpy, m.submenu);
        m.start_open = m.sub_open = false;
        m.hover = m.subhover = -1;
        draw_taskbar(m);
    }
    if (had)
        ungrab_if_idle();
}

void WM::open_start(Monitor &m)
{
    // Clicking Start while it is open closes it (toggle). close_menus() first
    // so we never leave two heads' Start menus mapped.
    bool was = m.start_open;
    close_menus();
    if (was)
        return;
    int mw = w95::kBannerW + w95::kMenuBodyW;
    int mh = menu_height();
    int mx = m.x + 2;
    int my = m.y + m.h - w95::kTaskbarH - mh;
    XMoveResizeWindow(dpy, m.startmenu, mx, my, (unsigned)mw, (unsigned)mh);
    m.start_open = true;
    m.hover = menu_step(-1, 1);
    m.subhover = -1;
    XMapRaised(dpy, m.startmenu);
    // Deactivate the client caption; keyboard + highlight follow this menu.
    focus(nullptr);
    sync_start_pointer(m);
    draw_startmenu(m);
    draw_taskbar(m);
}

void WM::close_run()
{
    if (!run_open)
        return;
    run_open = false;
    XUnmapWindow(dpy, rundlg);
    ungrab_if_idle();
}

void WM::open_run(int mi)
{
    // Centered on monitor mi. Keyboard grab so keys don't go to the focused client.
    close_menus();
    if (mons.empty())
        return;
    if (mi < 0 || mi >= (int)mons.size())
        mi = 0;
    dialog_mon = mi;
    Monitor &m = mons[mi];
    int w = w95::kRunW, h = w95::kRunH;
    int x = m.x + (m.w - w) / 2;
    int y = m.y + (m.h - w95::kTaskbarH - h) / 3;
    XMoveResizeWindow(dpy, rundlg, x, y, (unsigned)w, (unsigned)h);
    if (!run_open) {
        run_text.clear();
        run_cursor = 0;
    }
    run_open = true;
    XMapRaised(dpy, rundlg);
    focus(nullptr);
    draw_rundlg();
}

void WM::open_shut(int mi)
{
    close_menus();
    dialog_mon = mi;
    Monitor &m = mons[mi];
    int w = 320, h = 120;
    int x = m.x + (m.w - w) / 2;
    int y = m.y + (m.h - w95::kTaskbarH - h) / 3;
    XMoveResizeWindow(dpy, shutdlg, x, y, (unsigned)w, (unsigned)h);
    shut_open = true;
    XMapRaised(dpy, shutdlg);
    focus(nullptr);
    draw_shutdlg();
}

static void add_wall_dir(std::vector<WallChoice> &walls, const char *dir)
{
    DIR *d = opendir(dir);
    if (!d)
        return;
    while (dirent *e = readdir(d)) {
        const char *n = e->d_name;
        if (!is_image_name(n))
            continue;
        WallChoice w;
        w.name = n;
        w.pattern = 0;
        w.file = std::string(dir) + "/" + n;
        walls.push_back(std::move(w));
    }
    closedir(d);
}

void WM::rebuild_walls()
{
    // Built-in patterns first so indices stay stable, then files from the image
    // and the user's ~/.chime/wallpapers.
    walls.clear();
    const char *pats[] = {"(None)", "Bricks", "Dots", "Weave", "Waves", "Checker"};
    for (int i = 0; i < 6; i++) {
        WallChoice w;
        w.name = pats[i];
        w.pattern = i;
        walls.push_back(w);
    }
    add_wall_dir(walls, "/opt/chime/wallpapers");
    const char *home = getenv("HOME");
    if (home && *home) {
        std::string p = std::string(home) + "/.chime/wallpapers";
        add_wall_dir(walls, p.c_str());
    }
}

void WM::make_wall_tile()
{
    // Build a pixmap to XCopyArea in tile_wall(). File wallpapers are P6 PPM
    // (binary RGB). Patterns 1–5 are 32x32 doodles in the current scheme colors.
    if (wall_tile) {
        XFreePixmap(dpy, wall_tile);
        wall_tile = 0;
    }
    wall_tw = wall_th = 0;
    if (wall_i < 0 || wall_i >= (int)walls.size())
        return;
    const WallChoice &W = walls[wall_i];
    if (!W.file.empty()) {
        Raster img;
        if (!load_image_path(W.file.c_str(), img))
            return;
        Monitor *p = primary_mon();
        int dw = p ? p->w : sw;
        int dh = p ? p->h - w95::kTaskbarH : sh - w95::kTaskbarH;
        if (dh < 8)
            dh = 8;
        // Photos fill the monitor; small tiles (patterns, textures) repeat.
        if (img.w >= 128 || img.h >= 128) {
            img = scale_nn(img, dw, dh);
        }
        wall_tw = img.w;
        wall_th = img.h;
        wall_tile = XCreatePixmap(dpy, root, (unsigned)img.w, (unsigned)img.h, (unsigned)depth);
        blit_rgb(wall_tile, img.rgb.data(), img.w, img.h);
        return;
    }
    if (W.pattern <= 0)
        return;
    wall_tw = wall_th = 32;
    wall_tile = XCreatePixmap(dpy, root, 32, 32, (unsigned)depth);
    fill(wall_tile, 0, 0, 32, 32, desktop.pix);
    if (W.pattern == 1) {
        // Bricks: mortar lines, staggered every other row.
        XSetForeground(dpy, gc, lo.pix);
        for (int y = 0; y < 32; y += 8) {
            XDrawLine(dpy, wall_tile, gc, 0, y, 31, y);
            int off = (y / 8) & 1 ? 16 : 0;
            XDrawLine(dpy, wall_tile, gc, off, y, off, y + 7);
        }
        XSetForeground(dpy, gc, hi.pix);
        for (int y = 1; y < 32; y += 8)
            XDrawLine(dpy, wall_tile, gc, 0, y, 31, y);
    } else if (W.pattern == 2) {
        // Dots.
        XSetForeground(dpy, gc, dk.pix);
        for (int y = 2; y < 32; y += 4)
            for (int x = 2; x < 32; x += 4)
                XDrawPoint(dpy, wall_tile, gc, x, y);
    } else if (W.pattern == 3) {
        // Weave: paired diagonal lines in lo/hi.
        XSetForeground(dpy, gc, lo.pix);
        for (int i = -32; i < 32; i += 4)
            XDrawLine(dpy, wall_tile, gc, i, 0, i + 32, 32);
        XSetForeground(dpy, gc, hi.pix);
        for (int i = -30; i < 32; i += 4)
            XDrawLine(dpy, wall_tile, gc, i, 0, i + 32, 32);
    } else if (W.pattern == 4) {
        // Waves: a triangle-wave column of points.
        XSetForeground(dpy, gc, title.pix);
        for (int y = 0; y < 32; y++) {
            int x = 16 + (int)((y % 16 < 8 ? y % 8 : 8 - (y % 8)) * 1.5);
            XDrawPoint(dpy, wall_tile, gc, x, y);
            XDrawPoint(dpy, wall_tile, gc, x + 1, y);
        }
    } else if (W.pattern == 5) {
        // Checker: 8x8 cells in lo over the desktop color.
        for (int y = 0; y < 32; y += 8)
            for (int x = 0; x < 32; x += 8)
                if (((x / 8) + (y / 8)) & 1)
                    fill(wall_tile, x, y, 8, 8, lo.pix);
    }
}

void WM::apply_scheme(int i)
{
    if (schemes.empty())
        init_schemes();
    if (i < 0 || i >= (int)schemes.size())
        i = 0;
    scheme_i = i;
    const ColorScheme &s = schemes[i];
    desktop.pix = alloc_rgb(s.desktop);
    face.pix = alloc_rgb(s.face);
    hi.pix = alloc_rgb(s.hi);
    lo.pix = alloc_rgb(s.lo);
    dk.pix = alloc_rgb(s.dk);
    title.pix = alloc_rgb(s.title);
    title_in.pix = alloc_rgb(s.title_in);
    fg.pix = alloc_rgb(s.text);
    field.pix = alloc_rgb(s.field);
    banner.pix = alloc_rgb(s.banner);
    make_wall_tile();
    refresh_chrome();
}

void WM::apply_wall(int i)
{
    if (walls.empty())
        rebuild_walls();
    if (i < 0 || i >= (int)walls.size())
        i = 0;
    wall_i = i;
    make_wall_tile();
    refresh_chrome();
}

void WM::refresh_chrome()
{
    // After a scheme/wallpaper change, poke every shell window's background
    // pixel and repaint. Frames are redrawn so captions pick up the new title color.
    if (!dpy)
        return;
    XSetWindowBackground(dpy, root, desktop.pix);
    if (rundlg)
        XSetWindowBackground(dpy, rundlg, face.pix);
    if (shutdlg)
        XSetWindowBackground(dpy, shutdlg, face.pix);
    if (setdlg)
        XSetWindowBackground(dpy, setdlg, face.pix);
    if (colordlg)
        XSetWindowBackground(dpy, colordlg, face.pix);
    if (filedlg)
        XSetWindowBackground(dpy, filedlg, face.pix);
    if (traywin)
        XSetWindowBackground(dpy, traywin, face.pix);
    for (auto &m : mons) {
        if (m.desktop)
            XSetWindowBackground(dpy, m.desktop, desktop.pix);
        if (m.taskbar)
            XSetWindowBackground(dpy, m.taskbar, face.pix);
        if (m.startmenu)
            XSetWindowBackground(dpy, m.startmenu, face.pix);
        if (m.submenu)
            XSetWindowBackground(dpy, m.submenu, face.pix);
        if (m.desktop)
            draw_desktop(m);
        if (m.taskbar)
            draw_taskbar(m);
    }
    for (auto &c : clients)
        draw_frame(c.get());
    if (run_open)
        draw_rundlg();
    if (shut_open)
        draw_shutdlg();
    if (set_open)
        draw_setdlg();
    if (color_open)
        draw_colordlg();
    if (file_open)
        draw_filedlg();
    if (traywin)
        draw_tray();
}

void WM::save_display()
{
    // ~/.chime/display is two lines: scheme=<name> and wall=<name>.
    const char *home = getenv("HOME");
    if (!home || !*home)
        return;
    std::string dir = std::string(home) + "/.chime";
    mkdir(dir.c_str(), 0755);
    std::string path = dir + "/display";
    FILE *f = fopen(path.c_str(), "w");
    if (!f)
        return;
    const char *sn = (scheme_i >= 0 && scheme_i < (int)schemes.size()) ? schemes[scheme_i].name.c_str() : "Chicago";
    const char *wn = (wall_i >= 0 && wall_i < (int)walls.size()) ? walls[wall_i].name.c_str() : "(None)";
    int mw = (mode_i >= 0 && mode_i < (int)modes.size()) ? modes[mode_i].w : sw;
    int mh = (mode_i >= 0 && mode_i < (int)modes.size()) ? modes[mode_i].h : sh;
    std::fprintf(f, "scheme=%s\nwall=%s\nmode=%dx%d\n", sn, wn, mw, mh);
    fclose(f);
    save_schemes();
}

void WM::load_display()
{
    // Missing file is fine: Chicago + "(None)" stay at index 0.
    init_schemes();
    rebuild_walls();
    refresh_modes();
    const char *home = getenv("HOME");
    if (home && *home) {
        std::string path = std::string(home) + "/.chime/display";
        FILE *f = fopen(path.c_str(), "r");
        if (f) {
            char line[256];
            while (fgets(line, sizeof(line), f)) {
                char *nl = strchr(line, '\n');
                if (nl)
                    *nl = 0;
                if (!strncmp(line, "scheme=", 7)) {
                    for (int i = 0; i < (int)schemes.size(); i++)
                        if (schemes[i].name == (line + 7))
                            scheme_i = i;
                } else if (!strncmp(line, "wall=", 5)) {
                    for (int i = 0; i < (int)walls.size(); i++)
                        if (walls[i].name == (line + 5))
                            wall_i = i;
                } else if (!strncmp(line, "mode=", 5)) {
                    int mw = 0, mh = 0;
                    if (sscanf(line + 5, "%dx%d", &mw, &mh) == 2) {
                        for (int i = 0; i < (int)modes.size(); i++)
                            if (modes[i].w == mw && modes[i].h == mh)
                                mode_i = i;
                    }
                }
            }
            fclose(f);
        }
    }
    apply_scheme(scheme_i);
    apply_wall(wall_i);
}

void WM::close_settings(bool revert)
{
    // Cancel passes revert=true and restores the snapshot taken in open_settings.
    close_colordlg(false);
    close_filedlg();
    if (!set_open)
        return;
    set_open = false;
    XUnmapWindow(dpy, setdlg);
    if (revert) {
        schemes = set_save_schemes;
        apply_scheme(set_save_scheme);
        apply_wall(set_save_wall);
        mode_i = set_save_mode;
        mode_note.clear();
    }
    ungrab_if_idle();
}

void WM::open_settings(int mi)
{
    // Snapshot scheme/wall so Cancel can revert live preview changes.
    close_menus();
    if (mons.empty())
        return;
    if (mi < 0 || mi >= (int)mons.size())
        mi = 0;
    dialog_mon = mi;
    rebuild_walls();
    set_save_scheme = scheme_i;
    set_save_wall = wall_i;
    set_save_schemes = schemes;
    refresh_modes();
    set_save_mode = mode_i;
    wall_scroll = scheme_scroll = mode_scroll = 0;
    set_list = 1;
    mode_note.clear();
    Monitor &m = mons[mi];
    int w = w95::kSetW, h = w95::kSetH;
    int x = m.x + (m.w - w) / 2;
    int y = m.y + (m.h - w95::kTaskbarH - h) / 3;
    if (y < m.y + 8)
        y = m.y + 8;
    XMoveResizeWindow(dpy, setdlg, x, y, (unsigned)w, (unsigned)h);
    set_open = true;
    XMapRaised(dpy, setdlg);
    focus(nullptr);
    draw_setdlg();
}

void WM::grab_dialog(Window w)
{
    if (!w)
        return;
    XSetInputFocus(dpy, w, RevertToPointerRoot, CurrentTime);
    XGrabKeyboard(dpy, w, True, GrabModeAsync, GrabModeAsync, CurrentTime);
}

void WM::ungrab_if_idle()
{
    // Keyboard follows the last click: dialog, Start menu, focused client, or
    // the desktop (so arrow keys hit icons after you click the wallpaper).
    if (run_open)
        grab_dialog(rundlg);
    else if (file_open)
        grab_dialog(filedlg);
    else if (color_open)
        grab_dialog(colordlg);
    else if (set_open)
        grab_dialog(setdlg);
    else if (shut_open)
        grab_dialog(shutdlg);
    else if (Monitor *sm = open_start_mon()) {
        if (sm->sub_open && sm->subhover >= 0)
            grab_dialog(sm->submenu);
        else
            grab_dialog(sm->startmenu);
    } else if (focused && !focused->iconic) {
        XUngrabKeyboard(dpy, CurrentTime);
        XSetInputFocus(dpy, focused->win, RevertToPointerRoot, CurrentTime);
    } else if (Monitor *p = primary_mon(); p && p->desktop)
        grab_dialog(p->desktop);
    else {
        XUngrabKeyboard(dpy, CurrentTime);
        XSetInputFocus(dpy, root, RevertToPointerRoot, CurrentTime);
    }
}

void WM::ensure_list_scroll(int &scroll, int sel, int vis, int n)
{
    if (vis < 1)
        vis = 1;
    int maxs = std::max(0, n - vis);
    if (sel >= 0) {
        if (sel < scroll)
            scroll = sel;
        if (sel >= scroll + vis)
            scroll = sel - vis + 1;
    }
    if (scroll < 0)
        scroll = 0;
    if (scroll > maxs)
        scroll = maxs;
}

unsigned long WM::pixel_from_rgb(std::uint8_t r, std::uint8_t g, std::uint8_t b)
{
    if (vis && (vis->c_class == TrueColor || vis->c_class == DirectColor)) {
        auto pack = [](unsigned v, unsigned long mask) -> unsigned long {
            if (!mask)
                return 0;
            int shift = 0;
            unsigned long m = mask;
            while ((m & 1) == 0) {
                m >>= 1;
                shift++;
            }
            int bits = 0;
            while (m & 1) {
                m >>= 1;
                bits++;
            }
            unsigned long maxv = (1UL << bits) - 1;
            return ((v * maxv) / 255) << shift;
        };
        return pack(r, vis->red_mask) | pack(g, vis->green_mask) | pack(b, vis->blue_mask);
    }
    return alloc_rgb({r, g, b});
}

void WM::blit_rgb(Pixmap dst, const std::uint8_t *rgb, int w, int h)
{
    if (!rgb || w < 1 || h < 1)
        return;
    XImage *im = XCreateImage(dpy, vis, (unsigned)depth, ZPixmap, 0, nullptr, (unsigned)w, (unsigned)h, 32, 0);
    if (!im)
        return;
    im->data = (char *)calloc((size_t)im->bytes_per_line * (size_t)h, 1);
    if (!im->data) {
        im->data = nullptr;
        XDestroyImage(im);
        return;
    }
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            const std::uint8_t *p = rgb + ((size_t)y * w + x) * 3;
            XPutPixel(im, x, y, pixel_from_rgb(p[0], p[1], p[2]));
        }
    }
    XPutImage(dpy, dst, gc, im, 0, 0, 0, 0, (unsigned)w, (unsigned)h);
    XDestroyImage(im);
}

w95::Rgb &WM::scheme_role(ColorScheme &s, int role)
{
    switch (role) {
    case 0:
        return s.desktop;
    case 1:
        return s.face;
    case 2:
        return s.hi;
    case 3:
        return s.lo;
    case 4:
        return s.dk;
    case 5:
        return s.title;
    case 6:
        return s.title_in;
    case 7:
        return s.text;
    case 8:
        return s.field;
    default:
        return s.banner;
    }
}

void WM::init_schemes()
{
    schemes.clear();
    for (int i = 0; i < w95::kSchemeN; i++)
        schemes.push_back(scheme_from_builtin(w95::kSchemes[i]));
    load_schemes();
}

std::string WM::unique_scheme_name(const std::string &base)
{
    auto used = [&](const std::string &n) {
        for (auto &s : schemes)
            if (s.name == n)
                return true;
        return false;
    };
    if (!used(base))
        return base;
    for (int i = 2; i < 100; i++) {
        std::string n = base + " " + std::to_string(i);
        if (!used(n))
            return n;
    }
    return base + " copy";
}

void WM::save_schemes()
{
    const char *home = getenv("HOME");
    if (!home || !*home)
        return;
    std::string dir = std::string(home) + "/.chime";
    mkdir(dir.c_str(), 0755);
    std::string path = dir + "/schemes";
    FILE *f = fopen(path.c_str(), "w");
    if (!f)
        return;
    for (auto &s : schemes) {
        if (s.builtin)
            continue;
        auto rgb = [](FILE *fp, const char *k, w95::Rgb c) {
            std::fprintf(fp, "%s=%d,%d,%d\n", k, c.r, c.g, c.b);
        };
        std::fprintf(f, "name=%s\n", s.name.c_str());
        rgb(f, "desktop", s.desktop);
        rgb(f, "face", s.face);
        rgb(f, "hi", s.hi);
        rgb(f, "lo", s.lo);
        rgb(f, "dk", s.dk);
        rgb(f, "title", s.title);
        rgb(f, "title_in", s.title_in);
        rgb(f, "text", s.text);
        rgb(f, "field", s.field);
        rgb(f, "banner", s.banner);
        std::fprintf(f, "\n");
    }
    fclose(f);
}

void WM::load_schemes()
{
    const char *home = getenv("HOME");
    if (!home || !*home)
        return;
    std::string path = std::string(home) + "/.chime/schemes";
    FILE *f = fopen(path.c_str(), "r");
    if (!f)
        return;
    ColorScheme cur;
    bool have = false;
    auto finish = [&]() {
        if (!have || cur.name.empty())
            return;
        for (auto &s : schemes)
            if (s.name == cur.name)
                return;
        cur.builtin = false;
        schemes.push_back(cur);
    };
    auto parse_rgb = [](const char *p, w95::Rgb &c) {
        int r = 0, g = 0, b = 0;
        if (sscanf(p, "%d,%d,%d", &r, &g, &b) == 3) {
            c.r = (uint8_t)std::clamp(r, 0, 255);
            c.g = (uint8_t)std::clamp(g, 0, 255);
            c.b = (uint8_t)std::clamp(b, 0, 255);
        }
    };
    char line[256];
    while (fgets(line, sizeof(line), f)) {
        char *nl = strchr(line, '\n');
        if (nl)
            *nl = 0;
        if (!line[0]) {
            finish();
            cur = ColorScheme{};
            have = false;
            continue;
        }
        if (!strncmp(line, "name=", 5)) {
            finish();
            cur = ColorScheme{};
            cur.name = line + 5;
            have = true;
        } else if (!strncmp(line, "desktop=", 8))
            parse_rgb(line + 8, cur.desktop);
        else if (!strncmp(line, "face=", 5))
            parse_rgb(line + 5, cur.face);
        else if (!strncmp(line, "hi=", 3))
            parse_rgb(line + 3, cur.hi);
        else if (!strncmp(line, "lo=", 3))
            parse_rgb(line + 3, cur.lo);
        else if (!strncmp(line, "dk=", 3))
            parse_rgb(line + 3, cur.dk);
        else if (!strncmp(line, "title_in=", 9))
            parse_rgb(line + 9, cur.title_in);
        else if (!strncmp(line, "title=", 6))
            parse_rgb(line + 6, cur.title);
        else if (!strncmp(line, "text=", 5))
            parse_rgb(line + 5, cur.text);
        else if (!strncmp(line, "field=", 6))
            parse_rgb(line + 6, cur.field);
        else if (!strncmp(line, "banner=", 7))
            parse_rgb(line + 7, cur.banner);
    }
    finish();
    fclose(f);
}

Monitor *WM::open_start_mon()
{
    for (auto &m : mons)
        if (m.start_open)
            return &m;
    return nullptr;
}

void WM::position_submenu(Monitor &m)
{
    int mw = w95::kBannerW + w95::kMenuBodyW;
    int mh = menu_height();
    int sx = m.x + 2 + mw - 4;
    int sy = m.y + m.h - w95::kTaskbarH - mh + 4;
    if (sx + 150 > m.x + m.w)
        sx = m.x + 2 - 150 + 4;
    XMoveWindow(dpy, m.submenu, sx, sy);
    m.sub_open = true;
    XMapRaised(dpy, m.submenu);
    draw_submenu(m);
}

static int start_item_at(int x, int y)
{
    if (x < w95::kBannerW)
        return -1;
    int yy = 4;
    for (int i = 0; i < kMenuN; i++) {
        int ih = kMenu[i].sep ? 8 : w95::kMenuItemH;
        if (y >= yy && y < yy + ih)
            return kMenu[i].sep ? -1 : i;
        yy += ih;
    }
    return -1;
}

static int sub_item_at(int y)
{
    int i = (y - 4) / w95::kMenuItemH;
    if (i < 0 || i > 2)
        return -1;
    return i;
}

void WM::set_start_hover(Monitor &m, int idx, bool open_sub)
{
    if (idx != m.hover) {
        m.hover = idx;
        draw_startmenu(m);
    }
    bool want_sub = open_sub && idx == 0 && kMenu[0].enabled;
    if (want_sub && !m.sub_open)
        position_submenu(m);
    else if (!want_sub && m.sub_open) {
        XUnmapWindow(dpy, m.submenu);
        m.sub_open = false;
        m.subhover = -1;
    }
}

void WM::sync_start_pointer(Monitor &m)
{
    // Highlight the row under the cursor so mouse and keyboard share one selection.
    Window rr, cr;
    int rx, ry, wx, wy;
    unsigned mask;
    if (m.sub_open && XQueryPointer(dpy, m.submenu, &rr, &cr, &rx, &ry, &wx, &wy, &mask)) {
        XWindowAttributes wa{};
        if (XGetWindowAttributes(dpy, m.submenu, &wa) && wx >= 0 && wy >= 0 && wx < wa.width && wy < wa.height) {
            int i = sub_item_at(wy);
            if (i != m.subhover) {
                m.subhover = i;
                draw_submenu(m);
            }
            ungrab_if_idle();
            return;
        }
    }
    if (!XQueryPointer(dpy, m.startmenu, &rr, &cr, &rx, &ry, &wx, &wy, &mask))
        return;
    XWindowAttributes wa{};
    if (!XGetWindowAttributes(dpy, m.startmenu, &wa) || wx < 0 || wy < 0 || wx >= wa.width || wy >= wa.height)
        return;
    int idx = start_item_at(wx, wy);
    if (idx < 0)
        return;
    m.subhover = -1;
    set_start_hover(m, idx, idx == 0);
    ungrab_if_idle();
}

int WM::menu_step(int from, int dir)
{
    int i = from < 0 ? (dir > 0 ? -1 : 0) : from;
    for (int n = 0; n < kMenuN; n++) {
        i = (i + dir + kMenuN) % kMenuN;
        if (!kMenu[i].sep && kMenu[i].enabled)
            return i;
    }
    return from < 0 ? 0 : from;
}

void WM::activate_sub_item(int i)
{
    close_menus();
    if (i == 0)
        launch(kTermCmd);
    else if (i == 1)
        launch(kEditorCmd);
    else if (i == 2)
        launch("cabinet /");
}

void WM::activate_start_item(Monitor &m, int idx)
{
    if (idx < 0 || idx >= kMenuN || kMenu[idx].sep || !kMenu[idx].enabled)
        return;
    if (idx == 0) {
        position_submenu(m);
        m.subhover = 0;
        draw_submenu(m);
        ungrab_if_idle();
        return;
    }
    int mi = mon_index(&m);
    close_menus();
    if (idx == 1)
        launch("cabinet \"$HOME\"");
    else if (idx == 2)
        open_settings(mi);
    else if (idx == 4)
        launch("aterm -e sh -c 'echo Chime desktop; read x' || true");
    else if (idx == 5)
        open_run(mi);
    else if (idx == 7)
        open_shut(mi);
}

void WM::activate_desk_icon(int i)
{
    if (i == 0)
        launch("cabinet /");
    else if (i == 1)
        launch("cabinet \"$HOME\"");
    else if (i == 2)
        launch(kTermCmd);
    else if (i == 3)
        launch(kEditorCmd);
}

void WM::menu_key(KeySym ks)
{
    Monitor *mon = open_start_mon();
    if (!mon)
        return;
    bool in_sub = mon->sub_open && mon->subhover >= 0;
    if (in_sub) {
        if (ks == XK_Escape || ks == XK_Left) {
            XUnmapWindow(dpy, mon->submenu);
            mon->sub_open = false;
            mon->subhover = -1;
            draw_startmenu(*mon);
            ungrab_if_idle();
            return;
        }
        if (ks == XK_Down) {
            if (mon->subhover < 0)
                mon->subhover = 0;
            else
                mon->subhover = std::min(2, mon->subhover + 1);
            draw_submenu(*mon);
        } else if (ks == XK_Up) {
            if (mon->subhover < 0)
                mon->subhover = 0;
            else
                mon->subhover = std::max(0, mon->subhover - 1);
            draw_submenu(*mon);
        } else if (ks == XK_Return || ks == XK_KP_Enter)
            activate_sub_item(mon->subhover);
        return;
    }
    if (ks == XK_Escape) {
        close_menus();
        return;
    }
    if (ks == XK_Down)
        set_start_hover(*mon, menu_step(mon->hover, 1), false);
    else if (ks == XK_Up)
        set_start_hover(*mon, menu_step(mon->hover, -1), false);
    else if (ks == XK_Home)
        set_start_hover(*mon, menu_step(-1, 1), false);
    else if (ks == XK_End)
        set_start_hover(*mon, menu_step(0, -1), false);
    else if (ks == XK_Right && mon->hover == 0) {
        position_submenu(*mon);
        mon->subhover = 0;
        draw_startmenu(*mon);
        draw_submenu(*mon);
        ungrab_if_idle();
        return;
    } else if (ks == XK_Return || ks == XK_KP_Enter) {
        activate_start_item(*mon, mon->hover);
        return;
    }
}

void WM::desktop_key(KeySym ks, unsigned st)
{
    if (focused)
        return;
    bool ctrl = (st & ControlMask) != 0;
    bool shift = (st & ShiftMask) != 0;
    if (ks == XK_a && ctrl) {
        desk_mask = 0xf;
        if (selected_icon < 0)
            selected_icon = 0;
    } else if (ks == XK_Down || ks == XK_Right) {
        if (selected_icon < 0)
            selected_icon = 0;
        else
            selected_icon = std::min(3, selected_icon + 1);
        if (shift && desk_mask)
            desk_mask |= 1u << selected_icon;
        else if (!ctrl)
            desk_mask = 1u << selected_icon;
    } else if (ks == XK_Up || ks == XK_Left) {
        if (selected_icon < 0)
            selected_icon = 0;
        else
            selected_icon = std::max(0, selected_icon - 1);
        if (shift && desk_mask)
            desk_mask |= 1u << selected_icon;
        else if (!ctrl)
            desk_mask = 1u << selected_icon;
    } else if (ks == XK_Home) {
        selected_icon = 0;
        if (!ctrl)
            desk_mask = 1u;
    } else if (ks == XK_End) {
        selected_icon = 3;
        if (!ctrl)
            desk_mask = 1u << 3;
    } else if (ks == XK_space && ctrl) {
        if (selected_icon >= 0)
            desk_mask ^= 1u << selected_icon;
    } else if (ks == XK_Return || ks == XK_KP_Enter || (ks == XK_space && !ctrl)) {
        activate_desk_sel();
        return;
    } else if (ks == XK_Escape) {
        desk_select_only(-1);
    } else
        return;
    if (Monitor *p = primary_mon())
        draw_desktop(*p);
}

void WM::open_colordlg()
{
    if (mons.empty() || scheme_i < 0 || scheme_i >= (int)schemes.size())
        return;
    if (schemes[scheme_i].builtin) {
        ColorScheme s = schemes[scheme_i];
        s.builtin = false;
        s.name = unique_scheme_name(s.name);
        schemes.push_back(s);
        scheme_i = (int)schemes.size() - 1;
    }
    color_name_buf = schemes[scheme_i].name;
    color_name_edit = false;
    color_role = 0;
    color_chan = 0;
    Monitor &m = mons[dialog_mon < (int)mons.size() ? dialog_mon : 0];
    int w = w95::kColorW, h = w95::kColorH;
    int x = m.x + (m.w - w) / 2;
    int y = m.y + (m.h - w95::kTaskbarH - h) / 3;
    XMoveResizeWindow(dpy, colordlg, x, y, (unsigned)w, (unsigned)h);
    color_open = true;
    XMapRaised(dpy, colordlg);
    ungrab_if_idle();
    apply_scheme(scheme_i);
    draw_colordlg();
    if (set_open)
        draw_setdlg();
}

void WM::close_colordlg(bool apply)
{
    if (!color_open)
        return;
    color_open = false;
    XUnmapWindow(dpy, colordlg);
    if (apply && scheme_i >= 0 && scheme_i < (int)schemes.size()) {
        if (!color_name_buf.empty())
            schemes[scheme_i].name = color_name_buf;
        apply_scheme(scheme_i);
    }
    ungrab_if_idle();
    if (set_open)
        draw_setdlg();
}

void WM::load_file_dir(const std::string &path)
{
    DIR *d = opendir(path.c_str());
    if (!d)
        return;
    file_dir = path;
    while (file_dir.size() > 1 && file_dir.back() == '/')
        file_dir.pop_back();
    file_ents.clear();
    if (file_dir != "/") {
        FileEnt p;
        p.name = "..";
        p.dir = true;
        file_ents.push_back(p);
    }
    while (dirent *e = readdir(d)) {
        if (e->d_name[0] == '.')
            continue;
        FileEnt fe;
        fe.name = e->d_name;
        std::string full = file_dir == "/" ? (std::string("/") + fe.name) : (file_dir + "/" + fe.name);
        struct stat st{};
        if (stat(full.c_str(), &st) != 0)
            continue;
        fe.dir = S_ISDIR(st.st_mode);
        if (!fe.dir && !is_image_name(e->d_name))
            continue;
        file_ents.push_back(std::move(fe));
    }
    closedir(d);
    std::sort(file_ents.begin(), file_ents.end(), [](const FileEnt &a, const FileEnt &b) {
        if (a.name == "..")
            return true;
        if (b.name == "..")
            return false;
        if (a.dir != b.dir)
            return a.dir;
        return strcasecmp(a.name.c_str(), b.name.c_str()) < 0;
    });
    file_sel = 0;
    file_scroll = 0;
}

void WM::open_filedlg()
{
    if (mons.empty())
        return;
    const char *home = getenv("HOME");
    load_file_dir(home && *home ? home : "/");
    file_typed.clear();
    Monitor &m = mons[dialog_mon < (int)mons.size() ? dialog_mon : 0];
    int w = w95::kFileW, h = w95::kFileH;
    int x = m.x + (m.w - w) / 2;
    int y = m.y + (m.h - w95::kTaskbarH - h) / 3;
    XMoveResizeWindow(dpy, filedlg, x, y, (unsigned)w, (unsigned)h);
    file_open = true;
    XMapRaised(dpy, filedlg);
    ungrab_if_idle();
    draw_filedlg();
}

void WM::close_filedlg()
{
    if (!file_open)
        return;
    file_open = false;
    XUnmapWindow(dpy, filedlg);
    ungrab_if_idle();
}

bool WM::import_wallpaper(const std::string &src)
{
    Raster img;
    if (!load_image_path(src.c_str(), img))
        return false;
    const char *home = getenv("HOME");
    if (!home || !*home)
        return false;
    std::string dir = std::string(home) + "/.chime/wallpapers";
    mkdir((std::string(home) + "/.chime").c_str(), 0755);
    mkdir(dir.c_str(), 0755);
    std::string base = base_name(src);
    auto dot = base.find_last_of('.');
    if (dot != std::string::npos)
        base = base.substr(0, dot);
    if (base.empty())
        base = "wallpaper";
    std::string dest = dir + "/" + base + ".ppm";
    int n = 2;
    while (access(dest.c_str(), F_OK) == 0) {
        dest = dir + "/" + base + "-" + std::to_string(n++) + ".ppm";
    }
    if (!save_ppm(dest.c_str(), img))
        return false;
    std::string keep = base_name(dest);
    rebuild_walls();
    wall_i = 0;
    for (int i = 0; i < (int)walls.size(); i++)
        if (walls[i].name == keep)
            wall_i = i;
    apply_wall(wall_i);
    return true;
}

static std::string file_full(const std::string &dir, const std::string &name)
{
    if (name == "..") {
        auto sl = dir.rfind('/');
        if (sl == 0 || sl == std::string::npos)
            return "/";
        return dir.substr(0, sl);
    }
    return dir == "/" ? (std::string("/") + name) : (dir + "/" + name);
}

void WM::file_key(KeySym ks, const char *buf, int n)
{
    if (ks == XK_Escape) {
        close_filedlg();
        return;
    }
    if (ks == XK_Up) {
        file_sel = std::max(0, file_sel - 1);
        if (file_sel < (int)file_ents.size() && !file_ents[file_sel].dir)
            file_typed = file_ents[file_sel].name;
        draw_filedlg();
        return;
    }
    if (ks == XK_Down) {
        if (!file_ents.empty())
            file_sel = std::min((int)file_ents.size() - 1, file_sel + 1);
        if (file_sel < (int)file_ents.size() && !file_ents[file_sel].dir)
            file_typed = file_ents[file_sel].name;
        draw_filedlg();
        return;
    }
    if (ks == XK_BackSpace && (n != 1 || !buf[0] || buf[0] < 32)) {
        load_file_dir(file_full(file_dir, ".."));
        draw_filedlg();
        return;
    }
    if (ks == XK_Return || ks == XK_KP_Enter) {
        if (!file_typed.empty() && file_typed.find('/') != std::string::npos) {
            if (import_wallpaper(file_typed))
                close_filedlg();
            return;
        }
        if (file_sel < 0 || file_sel >= (int)file_ents.size())
            return;
        FileEnt &e = file_ents[file_sel];
        std::string full = file_full(file_dir, e.name);
        if (e.dir) {
            load_file_dir(full);
            draw_filedlg();
        } else if (import_wallpaper(full))
            close_filedlg();
        return;
    }
    if (ks == XK_BackSpace) {
        if (!file_typed.empty())
            file_typed.pop_back();
        draw_filedlg();
        return;
    }
    if (n == 1 && buf[0] >= 32 && buf[0] < 127) {
        file_typed.push_back(buf[0]);
        draw_filedlg();
    }
}

void WM::color_key(KeySym ks, const char *buf, int n)
{
    if (ks == XK_Escape) {
        close_colordlg(false);
        return;
    }
    if (ks == XK_Return && !color_name_edit) {
        close_colordlg(true);
        return;
    }
    if (ks == XK_Tab) {
        color_name_edit = !color_name_edit;
        draw_colordlg();
        return;
    }
    if (color_name_edit) {
        if (ks == XK_Return) {
            color_name_edit = false;
            draw_colordlg();
            return;
        }
        if (ks == XK_BackSpace) {
            if (!color_name_buf.empty())
                color_name_buf.pop_back();
            draw_colordlg();
            return;
        }
        if (n == 1 && buf[0] >= 32 && buf[0] < 127) {
            color_name_buf.push_back(buf[0]);
            draw_colordlg();
        }
        return;
    }
    if (ks == XK_Down)
        color_role = std::min(w95::kColorRoleN - 1, color_role + 1);
    else if (ks == XK_Up)
        color_role = std::max(0, color_role - 1);
    else if (ks == XK_Left || ks == XK_Right) {
        if (scheme_i < 0 || scheme_i >= (int)schemes.size())
            return;
        w95::Rgb &c = scheme_role(schemes[scheme_i], color_role);
        uint8_t *ch = color_chan == 0 ? &c.r : color_chan == 1 ? &c.g : &c.b;
        int v = *ch + (ks == XK_Right ? 5 : -5);
        *ch = (uint8_t)std::clamp(v, 0, 255);
        apply_scheme(scheme_i);
    } else if (ks == XK_bracketleft)
        color_chan = std::max(0, color_chan - 1);
    else if (ks == XK_bracketright)
        color_chan = std::min(2, color_chan + 1);
    else if (ks == XK_r || ks == XK_R)
        color_chan = 0;
    else if (ks == XK_g || ks == XK_G)
        color_chan = 1;
    else if (ks == XK_b || ks == XK_B)
        color_chan = 2;
    else
        return;
    draw_colordlg();
}

void WM::settings_key(KeySym ks, const char *, int)
{
    if (ks == XK_Return) {
        save_display();
        apply_mode(mode_i);
        set_save_scheme = scheme_i;
        set_save_wall = wall_i;
        set_save_mode = mode_i;
        set_save_schemes = schemes;
        close_settings(false);
        return;
    }
    if (ks == XK_Escape) {
        close_settings(true);
        return;
    }
    if (ks == XK_Tab) {
        set_list = (set_list + 1) % 3;
        draw_setdlg();
        return;
    }
    int *sel = &scheme_i;
    int n = (int)schemes.size();
    if (set_list == 0) {
        sel = &wall_i;
        n = (int)walls.size();
    } else if (set_list == 2) {
        sel = &mode_i;
        n = (int)modes.size();
    }
    if (n < 1)
        return;
    if (ks == XK_Down)
        *sel = std::min(n - 1, *sel + 1);
    else if (ks == XK_Up)
        *sel = std::max(0, *sel - 1);
    else if (ks == XK_Home)
        *sel = 0;
    else if (ks == XK_End)
        *sel = n - 1;
    else
        return;
    if (set_list == 1)
        apply_scheme(scheme_i);
    else if (set_list == 0)
        apply_wall(wall_i);
    draw_setdlg();
}

static int dir_from_hit(Hit h)
{
    // Bit 0 N, bit 1 S, bit 2 E, bit 3 W — matches drag_dir in on_motion.
    int d = 0;
    if (h == Hit::EdgeN || h == Hit::EdgeNE || h == Hit::EdgeNW)
        d |= 1;
    if (h == Hit::EdgeS || h == Hit::EdgeSE || h == Hit::EdgeSW)
        d |= 2;
    if (h == Hit::EdgeE || h == Hit::EdgeNE || h == Hit::EdgeSE)
        d |= 4;
    if (h == Hit::EdgeW || h == Hit::EdgeNW || h == Hit::EdgeSW)
        d |= 8;
    return d;
}

void WM::on_button_press(XEvent *e)
{
    // Dispatch by window: client (click-to-focus), dialogs, menus, taskbar,
    // desktop icons, then frame chrome (buttons / move / resize).
    XButtonEvent *b = &e->xbutton;
    if (run_open && b->window != rundlg) {
        if (b->window != shutdlg) {
            /* keep run dialog until click inside / buttons */
        }
    }

    Monitor *mon = mon_by_window(b->window);
    Client *fr = find_frame(b->window);
    Client *cl = find_client(b->window);

    if (cl) {
        // Click was on the application window (we grab AnyButton in Sync mode
        // so we can raise/focus then ReplayPointer to the client).
        close_menus();
        raise_client(cl);
        focus(cl);
        XAllowEvents(dpy, ReplayPointer, CurrentTime);
        return;
    }

    if (b->window == rundlg) {
        const int w = w95::kRunW, h = w95::kRunH;
        const int bw = w95::kDlgBtnW, bh = w95::kDlgBtnH;
        const int by = h - 12 - bh;
        const int cancel_x = w - 12 - bw;
        const int ok_x = cancel_x - 8 - bw;
        if (b->x >= w - 6 - w95::kBtn && b->y >= 4 && b->y < 4 + w95::kTitleH) {
            close_run();
            return;
        }
        if (b->y >= by && b->y < by + bh) {
            if (b->x >= ok_x && b->x < ok_x + bw) {
                if (!run_text.empty())
                    launch(run_text.c_str());
                close_run();
            } else if (b->x >= cancel_x && b->x < cancel_x + bw) {
                close_run();
            }
        }
        return;
    }
    if (b->window == shutdlg) {
        int w = 320, h = 120, bw = 74, bh = 24;
        if (b->y >= h - 36 && b->y < h - 36 + bh) {
            if (b->x >= w / 2 - bw - 8 && b->x < w / 2 - 8) {
                running = false;
                launch("poweroff -f 2>/dev/null || sudo poweroff 2>/dev/null || sudo halt 2>/dev/null || true");
            } else if (b->x >= w / 2 + 8 && b->x < w / 2 + 8 + bw) {
                shut_open = false;
                XUnmapWindow(dpy, shutdlg);
                ungrab_if_idle();
            }
        }
        return;
    }

    if (b->window == colordlg) {
        const int w = w95::kColorW, h = w95::kColorH;
        const int bw = w95::kDlgBtnW, bh = w95::kDlgBtnH;
        const int by = h - 12 - bh;
        const int cancel_x = w - 12 - bw;
        const int ok_x = cancel_x - 8 - bw;
        if (b->x >= w - 6 - w95::kBtn && b->y >= 4 && b->y < 4 + w95::kTitleH) {
            close_colordlg(false);
            return;
        }
        if (b->x >= 60 && b->y >= 26 && b->x < w - 16 && b->y < 46) {
            color_name_edit = true;
            draw_colordlg();
            return;
        }
        if (b->x >= 16 && b->y >= 54 && b->x < 196 && b->y < 234) {
            int i = (b->y - 56) / w95::kListRow;
            if (i >= 0 && i < w95::kColorRoleN) {
                color_role = i;
                color_name_edit = false;
                draw_colordlg();
            }
            return;
        }
        auto hitbar = [&](int y) {
            return b->y >= y && b->y < y + 16 && b->x >= 232 && b->x < 392;
        };
        int chan = -1;
        if (hitbar(136))
            chan = 0;
        else if (hitbar(160))
            chan = 1;
        else if (hitbar(184))
            chan = 2;
        if (chan >= 0 && scheme_i >= 0 && scheme_i < (int)schemes.size()) {
            color_chan = chan;
            int v = (b->x - 232) * 255 / 160;
            v = std::clamp(v, 0, 255);
            w95::Rgb &c = scheme_role(schemes[scheme_i], color_role);
            if (chan == 0)
                c.r = (uint8_t)v;
            else if (chan == 1)
                c.g = (uint8_t)v;
            else
                c.b = (uint8_t)v;
            apply_scheme(scheme_i);
            draw_colordlg();
            return;
        }
        if (b->y >= by && b->y < by + bh) {
            if (b->x >= ok_x && b->x < ok_x + bw)
                close_colordlg(true);
            else if (b->x >= cancel_x && b->x < cancel_x + bw)
                close_colordlg(false);
        }
        return;
    }

    if (b->window == filedlg) {
        const int w = w95::kFileW, h = w95::kFileH;
        const int bw = w95::kDlgBtnW, bh = w95::kDlgBtnH;
        const int by = h - 12 - bh;
        const int cancel_x = w - 12 - bw;
        const int ok_x = cancel_x - 8 - bw;
        if (b->x >= w - 6 - w95::kBtn && b->y >= 4 && b->y < 4 + w95::kTitleH) {
            close_filedlg();
            return;
        }
        if (b->y >= 24 && b->y < 46) {
            if (b->x >= w - 84 && b->x < w - 52) {
                load_file_dir(file_full(file_dir, ".."));
                draw_filedlg();
            } else if (b->x >= w - 48 && b->x < w - 12) {
                const char *home = getenv("HOME");
                load_file_dir(home && *home ? home : "/");
                draw_filedlg();
            }
            return;
        }
        if (b->button == 4) {
            file_scroll = std::max(0, file_scroll - 3);
            draw_filedlg();
            return;
        }
        if (b->button == 5) {
            file_scroll += 3;
            draw_filedlg();
            return;
        }
        int lx = 12, ly = 54, lw = w - 24, lh = 180;
        if (b->x >= lx && b->y >= ly && b->x < lx + lw && b->y < ly + lh) {
            int vis = (lh - 4) / w95::kListRow;
            int i = file_scroll + (b->y - ly - 2) / w95::kListRow;
            if (i >= 0 && i < (int)file_ents.size() && i < file_scroll + vis) {
                bool dbl = (file_sel == i && last_icon == i && b->time - last_icon_time < w95::kDblClickMs);
                file_sel = i;
                last_icon = i;
                last_icon_time = b->time;
                if (!file_ents[i].dir)
                    file_typed = file_ents[i].name;
                if (dbl) {
                    std::string full = file_full(file_dir, file_ents[i].name);
                    if (file_ents[i].dir)
                        load_file_dir(full);
                    else if (import_wallpaper(full)) {
                        close_filedlg();
                        return;
                    }
                }
                draw_filedlg();
            }
            return;
        }
        if (b->y >= by && b->y < by + bh) {
            if (b->x >= ok_x && b->x < ok_x + bw) {
                KeySym ks = XK_Return;
                file_key(ks, "", 0);
            } else if (b->x >= cancel_x && b->x < cancel_x + bw)
                close_filedlg();
        }
        return;
    }

    if (b->window == setdlg) {
        const int w = w95::kSetW, h = w95::kSetH;
        const int bw = w95::kDlgBtnW, bh = w95::kDlgBtnH;
        const int by = h - 12 - bh;
        const int apply_x = w - 12 - bw;
        const int cancel_x = apply_x - 8 - bw;
        const int ok_x = cancel_x - 8 - bw;
        auto listhit = [&](int lx, int ly, int lw, int lh, int scroll, int n) {
            if (b->x < lx || b->y < ly || b->x >= lx + lw || b->y >= ly + lh)
                return -1;
            int i = (b->y - ly - 2) / w95::kListRow;
            int vis = (lh - 4) / w95::kListRow;
            if (i < 0 || i >= vis)
                return -1;
            int idx = scroll + i;
            if (idx < 0 || idx >= n)
                return -1;
            return idx;
        };
        if (b->x >= w - 6 - w95::kBtn && b->y >= 4 && b->y < 4 + w95::kTitleH) {
            close_settings(true);
            return;
        }
        if (b->button == 4 || b->button == 5) {
            int *sc = &scheme_scroll;
            int n = (int)schemes.size();
            int vis = (w95::kSchemeListH - 4) / w95::kListRow;
            if (b->x >= w95::kWallListX && b->y >= w95::kWallListY && b->y < w95::kWallListY + w95::kWallListH) {
                sc = &wall_scroll;
                n = (int)walls.size();
                vis = (w95::kWallListH - 4) / w95::kListRow;
            } else if (b->x >= w95::kResListX && b->y >= w95::kResListY && b->y < w95::kResListY + w95::kResListH) {
                sc = &mode_scroll;
                n = (int)modes.size();
                vis = (w95::kResListH - 4) / w95::kListRow;
            }
            *sc += (b->button == 5) ? 1 : -1;
            ensure_list_scroll(*sc, -1, vis, n);
            draw_setdlg();
            return;
        }
        int wi = listhit(w95::kWallListX, w95::kWallListY, w95::kWallListW, w95::kWallListH, wall_scroll,
                         (int)walls.size());
        if (wi >= 0) {
            set_list = 0;
            apply_wall(wi);
            draw_setdlg();
            return;
        }
        int si = listhit(w95::kSchemeListX, w95::kSchemeListY, w95::kSchemeListW, w95::kSchemeListH, scheme_scroll,
                         (int)schemes.size());
        if (si >= 0) {
            set_list = 1;
            apply_scheme(si);
            draw_setdlg();
            return;
        }
        int mi = listhit(w95::kResListX, w95::kResListY, w95::kResListW, w95::kResListH, mode_scroll,
                         (int)modes.size());
        if (mi >= 0) {
            set_list = 2;
            mode_i = mi;
            mode_note.clear();
            draw_setdlg();
            return;
        }
        if (b->x >= w95::kBrowseX && b->x < w95::kBrowseX + w95::kBrowseW && b->y >= w95::kBrowseY &&
            b->y < w95::kBrowseY + w95::kDlgBtnH) {
            open_filedlg();
            return;
        }
        int bx = w95::kSchemeBtnX, byb = w95::kSchemeBtnY;
        if (b->x >= bx && b->x < bx + 96) {
            if (b->y >= byb && b->y < byb + w95::kDlgBtnH) {
                ColorScheme s = (scheme_i >= 0 && scheme_i < (int)schemes.size()) ? schemes[scheme_i]
                                                                                : scheme_from_builtin(w95::kSchemes[0]);
                s.builtin = false;
                s.name = unique_scheme_name("Custom");
                schemes.push_back(s);
                scheme_i = (int)schemes.size() - 1;
                apply_scheme(scheme_i);
                open_colordlg();
                return;
            }
            if (b->y >= byb + 28 && b->y < byb + 28 + w95::kDlgBtnH) {
                open_colordlg();
                return;
            }
            if (b->y >= byb + 56 && b->y < byb + 56 + w95::kDlgBtnH) {
                if (scheme_i >= 0 && scheme_i < (int)schemes.size() && !schemes[scheme_i].builtin &&
                    (int)schemes.size() > 1) {
                    schemes.erase(schemes.begin() + scheme_i);
                    if (scheme_i >= (int)schemes.size())
                        scheme_i = (int)schemes.size() - 1;
                    apply_scheme(scheme_i);
                    draw_setdlg();
                }
                return;
            }
        }
        if (b->y >= by && b->y < by + bh) {
            if (b->x >= ok_x && b->x < ok_x + bw) {
                save_display();
                apply_mode(mode_i);
                set_save_scheme = scheme_i;
                set_save_wall = wall_i;
                set_save_mode = mode_i;
                set_save_schemes = schemes;
                close_settings(false);
            } else if (b->x >= cancel_x && b->x < cancel_x + bw) {
                close_settings(true);
            } else if (b->x >= apply_x && b->x < apply_x + bw) {
                save_display();
                apply_mode(mode_i);
                set_save_scheme = scheme_i;
                set_save_wall = wall_i;
                set_save_mode = mode_i;
                set_save_schemes = schemes;
                draw_setdlg();
            }
        }
        return;
    }

    if (mon && b->window == mon->submenu && mon->sub_open) {
        int i = sub_item_at(b->y);
        mon->subhover = i;
        activate_sub_item(i);
        return;
    }

    if (mon && b->window == mon->startmenu && mon->start_open) {
        int idx = start_item_at(b->x, b->y);
        if (idx < 0 || kMenu[idx].sep || !kMenu[idx].enabled)
            return;
        mon->hover = idx;
        mon->subhover = -1;
        activate_start_item(*mon, idx);
        return;
    }

    if (mon && b->window == mon->taskbar) {
        if (b->x >= 2 && b->x < 2 + w95::kStartW && b->y >= 3 && b->y < w95::kTaskbarH - 3) {
            open_start(*mon);
            return;
        }
        close_menus();
        int trayw = tray_width(*mon);
        int trayx = mon->w - trayw - 4;
        if (b->x >= trayx)
            return; // Clock / tray well; volicon handles its own clicks.
        auto list = clients_on(mon_index(mon));
        int x0 = 2 + w95::kStartW + 6;
        int x1 = trayx - 6;
        int avail = x1 - x0;
        if (avail < 16 || list.empty())
            return;
        int n = (int)list.size();
        int bw = std::min(160, std::max(70, avail / n - 2));
        int x = x0;
        for (int i = 0; i < n && x + 20 < x1; i++) {
            int bw2 = std::min(bw, x1 - x);
            if (b->x >= x && b->x < x + bw2) {
                Client *c = list[i];
                if (c->iconic)
                    restore(c);
                else if (focused == c)
                    minimize(c);
                else {
                    raise_client(c);
                    focus(c);
                }
                return;
            }
            x += bw2 + 2;
        }
        return;
    }

    if (mon && b->window == mon->desktop) {
        close_menus();
        focus(nullptr);
        if (!mon->primary)
            return; // Icons exist only on the primary wallpaper.
        int hit = -1;
        for (int i = 0; i < 4; i++) {
            int ix, iy, iw, ih;
            desk_cell(i, ix, iy, iw, ih);
            if (b->x >= ix && b->y >= iy && b->x < ix + iw && b->y < iy + ih)
                hit = i;
        }
        bool ctrl = (b->state & ControlMask) != 0;
        bool shift = (b->state & ShiftMask) != 0;
        if (hit >= 0) {
            if ((desk_mask & (1u << hit)) && last_icon == hit && b->time - last_icon_time < w95::kDblClickMs) {
                activate_desk_sel();
                last_icon = -1;
            } else if (ctrl) {
                desk_mask ^= 1u << hit;
                selected_icon = hit;
            } else if (shift && selected_icon >= 0) {
                int a = std::min(selected_icon, hit), c = std::max(selected_icon, hit);
                desk_mask = 0;
                for (int i = a; i <= c; i++)
                    desk_mask |= 1u << i;
                selected_icon = hit;
            } else
                desk_select_only(hit);
            last_icon = hit;
            last_icon_time = b->time;
            draw_desktop(*mon);
        } else {
            if (!ctrl)
                desk_select_only(-1);
            sel_base = ctrl ? desk_mask : 0;
            sel_x0 = sel_x1 = b->x;
            sel_y0 = sel_y1 = b->y;
            sel_mon = mon_index(mon);
            drag = DragMode::Select;
            drag_c = nullptr;
            XGrabPointer(dpy, root, False, PointerMotionMask | ButtonReleaseMask, GrabModeAsync, GrabModeAsync, None,
                         cur_left, CurrentTime);
            draw_desktop(*mon);
        }
        return;
    }

    if (fr) {
        close_menus();
        raise_client(fr);
        focus(fr);
        Hit h = hit_frame(fr, b->x, b->y);
        if (h == Hit::Close) {
            close_client(fr);
            return;
        }
        if (h == Hit::Min) {
            minimize(fr);
            return;
        }
        if (h == Hit::Max) {
            maximize_toggle(fr);
            return;
        }
        if (h == Hit::Sys) {
            // Double-click the system-menu box closes, like Win95.
            if (last_title_win == fr->frame && b->time - last_title_time < w95::kDblClickMs)
                close_client(fr);
            last_title_win = fr->frame;
            last_title_time = b->time;
            return;
        }
        if (h == Hit::Title) {
            // Double-click caption toggles maximize; single click starts a move.
            if (last_title_win == fr->frame && b->time - last_title_time < w95::kDblClickMs) {
                maximize_toggle(fr);
                last_title_win = 0;
                return;
            }
            last_title_win = fr->frame;
            last_title_time = b->time;
            if (!fr->maxed && !fr->tiled) {
                fr->rx = fr->x;
                fr->ry = fr->y;
                fr->rw = fr->w;
                fr->rh = fr->h;
            }
            float_for_drag(fr, b->x_root, b->y_root);
            drag = DragMode::Move;
            drag_c = fr;
            drag_ox = b->x_root;
            drag_oy = b->y_root;
            drag_fx = fr->x;
            drag_fy = fr->y;
            drag_fw = fr->w;
            drag_fh = fr->h;
            XGrabPointer(dpy, root, False, PointerMotionMask | ButtonReleaseMask, GrabModeAsync, GrabModeAsync, None,
                         cur_move, CurrentTime);
            return;
        }
        int dir = dir_from_hit(h);
        if (dir && !fr->maxed) {
            if (fr->tiled) {
                fr->tiled = false;
                fr->snap = Snap::Off;
            }
            drag = DragMode::Resize;
            drag_c = fr;
            drag_dir = dir;
            drag_ox = b->x_root;
            drag_oy = b->y_root;
            drag_fx = fr->x;
            drag_fy = fr->y;
            drag_fw = fr->w;
            drag_fh = fr->h;
            XGrabPointer(dpy, root, False, PointerMotionMask | ButtonReleaseMask, GrabModeAsync, GrabModeAsync, None,
                         cursor_for(h), CurrentTime);
        }
    }
}

void WM::on_button_release(XEvent *e)
{
    if (drag == DragMode::Select) {
        drag = DragMode::Off;
        XUngrabPointer(dpy, CurrentTime);
        if (Monitor *p = primary_mon())
            draw_desktop(*p);
        return;
    }
    if (drag != DragMode::Off) {
        // Commit snap if the pointer is still in an edge zone; otherwise just
        // remember which monitor the window landed on.
        if (drag == DragMode::Move && drag_c) {
            int px = e->xbutton.x_root;
            int py = e->xbutton.y_root;
            Snap s = snap_at(px, py);
            if (s != Snap::Off)
                apply_snap(drag_c, s, px, py);
            else
                drag_c->last_mon = monitor_for(drag_c);
        } else if (drag_c) {
            drag_c->last_mon = monitor_for(drag_c);
        }
        hide_snap_preview();
        drag = DragMode::Off;
        drag_c = nullptr;
        XUngrabPointer(dpy, CurrentTime);
        for (auto &m : mons)
            draw_taskbar(m);
    }
}

void WM::on_motion(XEvent *e)
{
    // During a drag we update geometry every motion; otherwise we only refresh
    // resize cursors and Start-menu hover.
    XMotionEvent *m = &e->xmotion;
    if (drag == DragMode::Select) {
        Monitor *mon = (sel_mon >= 0 && sel_mon < (int)mons.size()) ? &mons[sel_mon] : primary_mon();
        if (mon) {
            sel_x1 = m->x_root - mon->x;
            sel_y1 = m->y_root - mon->y;
            select_desk_icons(sel_x0, sel_y0, sel_x1, sel_y1, sel_base);
            draw_desktop(*mon);
        }
        return;
    }
    if (drag != DragMode::Off && drag_c) {
        int dx = m->x_root - drag_ox;
        int dy = m->y_root - drag_oy;
        if (drag == DragMode::Move) {
            drag_c->x = drag_fx + dx;
            drag_c->y = drag_fy + dy;
            apply_geom(drag_c);
            show_snap_preview(snap_at(m->x_root, m->y_root), m->x_root, m->y_root);
        } else {
            // drag_dir bits: 1=N 2=S 4=E 8=W. Moving N/W also shifts origin.
            int x = drag_fx, y = drag_fy, w = drag_fw, h = drag_fh;
            if (drag_dir & 8) {
                x += dx;
                w -= dx;
            }
            if (drag_dir & 4)
                w += dx;
            if (drag_dir & 1) {
                y += dy;
                h -= dy;
            }
            if (drag_dir & 2)
                h += dy;
            if (w >= 80 && h >= 50) {
                drag_c->x = x;
                drag_c->y = y;
                drag_c->w = w;
                drag_c->h = h;
                apply_geom(drag_c);
            }
        }
        return;
    }
    if (Client *c = find_frame(m->window)) {
        Hit h = hit_frame(c, m->x, m->y);
        XDefineCursor(dpy, c->frame, cursor_for(h));
    }
    Monitor *mon = mon_by_window(m->window);
    if (mon && m->window == mon->startmenu && mon->start_open) {
        int idx = start_item_at(m->x, m->y);
        mon->subhover = -1;
        if (idx >= 0)
            set_start_hover(*mon, idx, idx == 0);
        ungrab_if_idle();
    }
    if (mon && m->window == mon->submenu && mon->sub_open) {
        int i = sub_item_at(m->y);
        if (i != mon->subhover) {
            mon->subhover = i;
            draw_submenu(*mon);
            ungrab_if_idle();
        }
    }
}

void WM::on_key(XEvent *e)
{
    // Super tap = Start. Super+R = Run. Super held with another key sets
    // super_chord so the eventual Super release does not open the menu.
    KeySym ks = XLookupKeysym(&e->xkey, 0);
    bool press = (e->type == KeyPress);

    if (!press) {
        if (ks == XK_Super_L || ks == XK_Super_R || ks == XK_Meta_L || ks == XK_Meta_R) {
            if (super_held && !super_chord && !run_open && !shut_open && !set_open && !mons.empty())
                open_start(mons[pointer_mon()]);
            super_held = false;
            super_chord = false;
        }
        return;
    }

    if (ks == XK_Super_L || ks == XK_Super_R || ks == XK_Meta_L || ks == XK_Meta_R) {
        super_held = true;
        super_chord = false;
        return;
    }

    if ((ks == XK_r || ks == XK_R) && (super_held || (super_mask && (e->xkey.state & super_mask)))) {
        super_chord = true;
        open_run(pointer_mon());
        return;
    }

    char buf[16];
    KeySym ks2;
    int n = XLookupString(&e->xkey, buf, sizeof(buf), &ks2, nullptr);

    if (file_open) {
        file_key(ks2, buf, n);
        return;
    }
    if (color_open) {
        color_key(ks2, buf, n);
        return;
    }
    if (set_open) {
        if (super_held || (super_mask && (e->xkey.state & super_mask))) {
            super_chord = true;
            return;
        }
        settings_key(ks2, buf, n);
        return;
    }

    if (open_start_mon()) {
        menu_key(ks2);
        return;
    }

    if (shut_open) {
        if (ks2 == XK_Escape || ks2 == XK_n || ks2 == XK_N) {
            shut_open = false;
            XUnmapWindow(dpy, shutdlg);
            ungrab_if_idle();
        } else if (ks2 == XK_Return || ks2 == XK_y || ks2 == XK_Y) {
            running = false;
            launch("poweroff -f 2>/dev/null || sudo poweroff 2>/dev/null || sudo halt 2>/dev/null || true");
        }
        return;
    }

    if (run_open) {
        if (super_held || (super_mask && (e->xkey.state & super_mask))) {
            super_chord = true;
            return;
        }
        caret_on = true;
        caret_ms = now_ms();
        if (ks2 == XK_Return) {
            if (!run_text.empty())
                launch(run_text.c_str());
            close_run();
        } else if (ks2 == XK_Escape) {
            close_run();
        } else if (ks2 == XK_Left) {
            if (run_cursor > 0)
                run_cursor--;
            draw_rundlg();
        } else if (ks2 == XK_Right) {
            if (run_cursor < (int)run_text.size())
                run_cursor++;
            draw_rundlg();
        } else if (ks2 == XK_Home) {
            run_cursor = 0;
            draw_rundlg();
        } else if (ks2 == XK_End) {
            run_cursor = (int)run_text.size();
            draw_rundlg();
        } else if (ks2 == XK_BackSpace) {
            if (run_cursor > 0 && !run_text.empty()) {
                run_text.erase((size_t)run_cursor - 1, 1);
                run_cursor--;
            }
            draw_rundlg();
        } else if (ks2 == XK_Delete) {
            if (run_cursor < (int)run_text.size())
                run_text.erase((size_t)run_cursor, 1);
            draw_rundlg();
        } else if (n == 1 && buf[0] >= 32 && buf[0] < 127) {
            run_text.insert((size_t)run_cursor, 1, buf[0]);
            run_cursor++;
            draw_rundlg();
        }
        return;
    }
    if (ks == XK_F4 && (e->xkey.state & Mod1Mask) && focused)
        close_client(focused);
    if (ks == XK_Tab && (e->xkey.state & Mod1Mask) && !clients.empty()) {
        // Cycle windows on the focused monitor (or the pointer's, if none).
        int mi = 0;
        if (focused)
            mi = monitor_for(focused);
        else {
            Window rr, cr;
            int rx, ry, wx, wy;
            unsigned mask;
            XQueryPointer(dpy, root, &rr, &cr, &rx, &ry, &wx, &wy, &mask);
            mi = monitor_at(rx, ry);
        }
        auto list = clients_on(mi);
        if (list.empty()) {
            for (auto &c : clients)
                list.push_back(c.get());
        }
        if (list.empty())
            return;
        int idx = 0;
        for (size_t i = 0; i < list.size(); i++)
            if (list[i] == focused)
                idx = (int)i;
        idx = (idx + ((e->xkey.state & ShiftMask) ? -1 : 1) + (int)list.size()) % (int)list.size();
        if (list[idx]->iconic)
            restore(list[idx]);
        else {
            raise_client(list[idx]);
            focus(list[idx]);
        }
    }
    if (ks == XK_Escape && (e->xkey.state & ControlMask) && !mons.empty())
        open_start(mons[pointer_mon()]); // Ctrl+Esc is the classic Start chord.
    else if (!(e->xkey.state & (Mod1Mask | super_mask)) && !focused)
        desktop_key(ks2, e->xkey.state);
}

void WM::on_expose(XEvent *e)
{
    // Compress: only paint when this is the last expose in the batch.
    if (e->xexpose.count != 0)
        return;
    if (e->xexpose.window == snapwin) {
        draw_snap_preview();
        return;
    }
    if (e->xexpose.window == rundlg) {
        draw_rundlg();
        return;
    }
    if (e->xexpose.window == shutdlg) {
        draw_shutdlg();
        return;
    }
    if (e->xexpose.window == setdlg) {
        draw_setdlg();
        return;
    }
    if (e->xexpose.window == colordlg) {
        draw_colordlg();
        return;
    }
    if (e->xexpose.window == filedlg) {
        draw_filedlg();
        return;
    }
    if (e->xexpose.window == traywin) {
        draw_tray();
        return;
    }
    if (Monitor *m = mon_by_window(e->xexpose.window)) {
        if (e->xexpose.window == m->desktop)
            draw_desktop(*m);
        else if (e->xexpose.window == m->taskbar)
            draw_taskbar(*m);
        else if (e->xexpose.window == m->startmenu)
            draw_startmenu(*m);
        else if (e->xexpose.window == m->submenu)
            draw_submenu(*m);
        return;
    }
    if (Client *c = find_frame(e->xexpose.window))
        draw_frame(c);
}

void WM::handle_event(XEvent *e)
{
    // RandR events are not in the core XEvent type enum; they are offset from
    // the first event code we stored in init().
    if (have_randr && e->type == rr_event + RRScreenChangeNotify) {
        XRRUpdateConfiguration(e);
        sync_monitors();
        return;
    }
    switch (e->type) {
    case MapRequest:
        // New top-level, or a minimized client asking to come back. Tray icons
        // are already children of traywin and just need XMapWindow.
        if (is_tray_icon(e->xmaprequest.window))
            XMapWindow(dpy, e->xmaprequest.window);
        else if (Client *c = find_client(e->xmaprequest.window))
            restore(c);
        else
            manage(e->xmaprequest.window);
        break;
    case ConfigureRequest: {
        // Clients must not configure themselves after we reparent; we translate
        // inner size into outer frame size. Unmanaged windows (override_redirect)
        // get the request applied as-is.
        XConfigureRequestEvent *cr = &e->xconfigurerequest;
        if (is_tray_icon(cr->window)) {
            tray_layout();
            break;
        }
        Client *c = find_client(cr->window);
        XWindowChanges wc;
        wc.x = cr->x;
        wc.y = cr->y;
        wc.width = cr->width;
        wc.height = cr->height;
        wc.border_width = 0;
        wc.sibling = cr->above;
        wc.stack_mode = cr->detail;
        if (c) {
            if (cr->value_mask & CWWidth)
                c->w = cr->width + 2 * w95::kFrameB;
            if (cr->value_mask & CWHeight)
                c->h = cr->height + 2 * w95::kFrameB + w95::kTitleH;
            if (cr->value_mask & CWX)
                c->x = cr->x;
            if (cr->value_mask & CWY)
                c->y = cr->y;
            apply_geom(c);
        } else {
            XConfigureWindow(dpy, cr->window, cr->value_mask, &wc);
        }
        break;
    }
    case UnmapNotify:
        if (Client *c = find_client(e->xunmap.window)) {
            if (c->ignore_unmap) {
                c->ignore_unmap--;
                break;
            }
            // ICCCM withdraw: a synthetic UnmapNotify means the client is
            // withdrawing. Events with event==root are from the root's
            // SubstructureNotify and would double-unmanage.
            if (e->xunmap.send_event)
                unmanage(c, false);
            else if (e->xunmap.event == root)
                break;
            else
                unmanage(c, false);
        }
        break;
    case DestroyNotify:
        if (is_tray_icon(e->xdestroywindow.window))
            tray_undock(e->xdestroywindow.window);
        else if (Client *c = find_client(e->xdestroywindow.window))
            unmanage(c, true);
        break;
    case EnterNotify:
        break;
    case LeaveNotify:
        if (Monitor *m = mon_by_window(e->xcrossing.window)) {
            if (e->xcrossing.window == m->startmenu && m->start_open && e->xcrossing.detail != NotifyInferior) {
                /* keep menu while moving to submenu */
            }
        }
        break;
    case ButtonPress:
        on_button_press(e);
        break;
    case ButtonRelease:
        on_button_release(e);
        break;
    case MotionNotify:
        on_motion(e);
        break;
    case KeyPress:
    case KeyRelease:
        on_key(e);
        break;
    case Expose:
        on_expose(e);
        break;
    case PropertyNotify:
        // Title changes refresh caption + task button. Tray XEMBED_INFO can
        // mean the icon wants to show/hide; we just relayout.
        if (is_tray_icon(e->xproperty.window) && e->xproperty.atom == xembed_info)
            tray_layout();
        else if (Client *c = find_client(e->xproperty.window)) {
            if (e->xproperty.atom == XA_WM_NAME || e->xproperty.atom == net_wm_name) {
                c->name = get_name(c->win);
                draw_frame(c);
                for (auto &m : mons)
                    draw_taskbar(m);
            }
        }
        break;
    case ClientMessage:
        if (e->xclient.message_type == net_system_tray_opcode) {
            if (e->xclient.data.l[1] == 0) // SYSTEM_TRAY_REQUEST_DOCK
                tray_dock((Window)e->xclient.data.l[2]);
        } else if (e->xclient.message_type == net_active) {
            if (Client *c = find_client(e->xclient.window)) {
                if (c->iconic)
                    restore(c);
                else {
                    raise_client(c);
                    focus(c);
                }
            }
        }
        break;
    case MappingNotify:
        XRefreshKeyboardMapping(&e->xmapping);
        break;
    default:
        break;
    }
}

unsigned WM::mod_mask_for(KeySym ks)
{
    // Walk the modifier map so Super works even when it is not Mod4.
    KeyCode kc = XKeysymToKeycode(dpy, ks);
    if (!kc)
        return 0;
    XModifierKeymap *mm = XGetModifierMapping(dpy);
    if (!mm)
        return 0;
    unsigned masks[8] = {ShiftMask, LockMask, ControlMask, Mod1Mask, Mod2Mask, Mod3Mask, Mod4Mask, Mod5Mask};
    unsigned found = 0;
    for (int m = 0; m < 8; m++) {
        for (int i = 0; i < mm->max_keypermod; i++) {
            if (mm->modifiermap[m * mm->max_keypermod + i] == kc)
                found |= masks[m];
        }
    }
    XFreeModifiermap(mm);
    return found;
}

void WM::grab_key(KeySym ks, unsigned mod)
{
    // Grab with CapsLock and NumLock (Mod2) ignored, otherwise Alt+Tab dies
    // the moment the user has NumLock on.
    KeyCode kc = XKeysymToKeycode(dpy, ks);
    if (!kc)
        return;
    unsigned ign[] = {0, LockMask, Mod2Mask, LockMask | Mod2Mask};
    for (unsigned extra : ign)
        XGrabKey(dpy, kc, mod | extra, root, True, GrabModeAsync, GrabModeAsync);
}

bool WM::init()
{
    dpy = XOpenDisplay(nullptr);
    if (!dpy) {
        std::fprintf(stderr, "chime: cannot open display (use make image && make run)\n");
        return false;
    }
    g_wm = this;
    signal(SIGCHLD, SIG_IGN);
    screen = DefaultScreen(dpy);
    root = RootWindow(dpy, screen);
    vis = DefaultVisual(dpy, screen);
    cmap = DefaultColormap(dpy, screen);
    depth = DefaultDepth(dpy, screen);
    sw = DisplayWidth(dpy, screen);
    sh = DisplayHeight(dpy, screen);
    intern_atoms();

    g_other_wm = false;
    XSetErrorHandler(xerr_start);
    // This SelectInput is how we become the window manager. XSync so the
    // error handler sees BadAccess before we continue.
    XSelectInput(dpy, root, SubstructureRedirectMask | SubstructureNotifyMask | ButtonPressMask | KeyPressMask |
                                KeyReleaseMask | PropertyChangeMask);
    XSync(dpy, False);
    if (g_other_wm) {
        std::fprintf(stderr, "chime: another window manager is running\n");
        return false;
    }
    XSetErrorHandler(xerr);

    desktop.pix = alloc_rgb(w95::rgb_desktop);
    face.pix = alloc_rgb(w95::rgb_face);
    hi.pix = alloc_rgb(w95::rgb_hi);
    lo.pix = alloc_rgb(w95::rgb_lo);
    dk.pix = alloc_rgb(w95::rgb_dk);
    title.pix = alloc_rgb(w95::rgb_title);
    title_in.pix = alloc_rgb(w95::rgb_title_in);
    fg.pix = alloc_rgb(w95::rgb_text);
    white.pix = alloc_rgb(w95::rgb_white);
    red.pix = alloc_rgb(w95::rgb_red);
    green.pix = alloc_rgb(w95::rgb_green);
    blue.pix = alloc_rgb(w95::rgb_blue);
    yellow.pix = alloc_rgb(w95::rgb_yellow);
    field.pix = alloc_rgb(w95::rgb_field);
    banner.pix = alloc_rgb(w95::rgb_banner);

    gc = XCreateGC(dpy, root, 0, nullptr);
    // Helvetica if the server has it (TinyCore Xfonts often don't), else "fixed".
    const char *fn[] = {"-*-helvetica-medium-r-*-*-12-*-*-*-*-*-*-*", "-*-helvetica-medium-r-*-*-11-*-*-*-*-*-*-*",
                        "fixed", nullptr};
    const char *fb[] = {"-*-helvetica-bold-r-*-*-12-*-*-*-*-*-*-*", "-*-helvetica-bold-r-*-*-11-*-*-*-*-*-*-*",
                        "fixed", nullptr};
    for (int i = 0; fn[i] && !font; i++)
        font = XLoadQueryFont(dpy, fn[i]);
    for (int i = 0; fb[i] && !font_b; i++)
        font_b = XLoadQueryFont(dpy, fb[i]);
    if (font)
        XSetFont(dpy, gc, font->fid);

    cur_left = XCreateFontCursor(dpy, XC_left_ptr);
    cur_move = XCreateFontCursor(dpy, XC_fleur);
    cur_n = XCreateFontCursor(dpy, XC_top_side);
    cur_s = XCreateFontCursor(dpy, XC_bottom_side);
    cur_e = XCreateFontCursor(dpy, XC_right_side);
    cur_w = XCreateFontCursor(dpy, XC_left_side);
    cur_ne = XCreateFontCursor(dpy, XC_top_right_corner);
    cur_nw = XCreateFontCursor(dpy, XC_top_left_corner);
    cur_se = XCreateFontCursor(dpy, XC_bottom_right_corner);
    cur_sw = XCreateFontCursor(dpy, XC_bottom_left_corner);
    XDefineCursor(dpy, root, cur_left);
    XSetWindowBackground(dpy, root, desktop.pix);
    XClearWindow(dpy, root);

    // Xfbdev has no RandR; have_randr stays false and query_monitors falls through.
    have_randr = XRRQueryExtension(dpy, &rr_event, &rr_error);
    if (have_randr)
        XRRSelectInput(dpy, root, RRScreenChangeNotifyMask | RROutputChangeNotifyMask | RRCrtcChangeNotifyMask);

    ewmh_init();
    // Dialogs and the snap overlay are created once, then moved onto the
    // relevant monitor when opened. override_redirect so we don't manage them.
    rundlg = mkwin(0, 0, w95::kRunW, w95::kRunH, face.pix,
                   ExposureMask | ButtonPressMask | KeyPressMask | KeyReleaseMask, true);
    shutdlg = mkwin(0, 0, 320, 120, face.pix, ExposureMask | ButtonPressMask | KeyPressMask, true);
    setdlg = mkwin(0, 0, w95::kSetW, w95::kSetH, face.pix,
                   ExposureMask | ButtonPressMask | KeyPressMask | KeyReleaseMask, true);
    snapwin = mkwin(0, 0, 40, 40, title.pix, ExposureMask, true);
    colordlg = mkwin(0, 0, w95::kColorW, w95::kColorH, face.pix,
                     ExposureMask | ButtonPressMask | KeyPressMask | KeyReleaseMask, true);
    filedlg = mkwin(0, 0, w95::kFileW, w95::kFileH, face.pix,
                    ExposureMask | ButtonPressMask | KeyPressMask | KeyReleaseMask, true);
    sync_monitors();
    load_display();

    super_mask = mod_mask_for(XK_Super_L) | mod_mask_for(XK_Super_R);
    if (!super_mask)
        super_mask = Mod4Mask;
    // Grabs on root: we see these even when a client has focus.
    grab_key(XK_Tab, Mod1Mask);
    grab_key(XK_Tab, Mod1Mask | ShiftMask);
    grab_key(XK_F4, Mod1Mask);
    grab_key(XK_Escape, ControlMask);
    grab_key(XK_Super_L, 0);
    grab_key(XK_Super_R, 0);
    grab_key(XK_r, super_mask);

    // Adopt windows that were already mapped (restart / existing xterms).
    Window dummy, *kids = nullptr;
    unsigned n = 0;
    if (XQueryTree(dpy, root, &dummy, &dummy, &kids, &n) && kids) {
        for (unsigned i = 0; i < n; i++) {
            XWindowAttributes wa{};
            if (!XGetWindowAttributes(dpy, kids[i], &wa))
                continue;
            if (wa.override_redirect || wa.map_state != IsViewable)
                continue;
            if (is_internal(kids[i]) || find_frame(kids[i]))
                continue;
            manage(kids[i]);
        }
        XFree(kids);
    }
    restack_shell();
    // Desktop owns the keyboard until a client maps, so arrow keys hit icons.
    if (!focused)
        focus(nullptr);
    return true;
}

void WM::run()
{
    // Drain the X queue, then select() with a 250ms timeout so the clock and
    // dialog carets can blink even when the user is idle.
    int fd = ConnectionNumber(dpy);
    int last_min = -1;
    caret_ms = now_ms();
    while (running) {
        while (XPending(dpy)) {
            XEvent e;
            XNextEvent(dpy, &e);
            handle_event(&e);
        }
        time_t now = time(nullptr);
        struct tm tm{};
        localtime_r(&now, &tm);
        if (tm.tm_min != last_min) {
            last_min = tm.tm_min;
            for (auto &m : mons)
                draw_taskbar(m);
        }
        long t = now_ms();
        if (t - caret_ms >= 530) {
            caret_ms = t;
            caret_on = !caret_on;
            if (run_open)
                draw_rundlg();
            if (color_open && color_name_edit)
                draw_colordlg();
            if (file_open)
                draw_filedlg();
        }
        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(fd, &fds);
        struct timeval tv{0, 250000};
        select(fd + 1, &fds, nullptr, nullptr, &tv);
    }
}

void WM::finish()
{
    // Drop the tray selection so another WM can claim it, then close Display.
    close_run();
    close_settings(false);
    close_menus();
    if (dpy && traywin)
        XSetSelectionOwner(dpy, net_system_tray, None, CurrentTime);
    if (dpy)
        XCloseDisplay(dpy);
    dpy = nullptr;
}
