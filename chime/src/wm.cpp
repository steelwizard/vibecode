#include "wm.h"

#include <X11/Xproto.h>

#include <algorithm>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <sys/select.h>
#include <unistd.h>

static WM *g_wm;
static bool g_other_wm;

static int xerr_start(Display *, XErrorEvent *e)
{
    if (e->error_code == BadAccess)
        g_other_wm = true;
    return 0;
}

static int xerr(Display *, XErrorEvent *e)
{
    if (e->error_code == BadWindow || e->error_code == BadDrawable || e->error_code == BadMatch)
        return 0;
    return 0;
}

unsigned long WM::alloc_rgb(w95::Rgb c)
{
    XColor xc{};
    xc.red = (unsigned short)(c.r * 257);
    xc.green = (unsigned short)(c.g * 257);
    xc.blue = (unsigned short)(c.b * 257);
    xc.flags = DoRed | DoGreen | DoBlue;
    XAllocColor(dpy, cmap, &xc);
    return xc.pixel;
}

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
}

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
    long dmask = ExposureMask | ButtonPressMask | ButtonReleaseMask | ButtonMotionMask;
    long tmask = ExposureMask | ButtonPressMask | ButtonReleaseMask | PointerMotionMask | LeaveWindowMask;
    long mmask = ExposureMask | ButtonPressMask | PointerMotionMask | LeaveWindowMask | EnterWindowMask;
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
    for (auto &m : mons)
        destroy_shell(m);
    mons = std::move(geoms);
    for (auto &m : mons)
        create_shell(m);
    for (auto &c : clients) {
        if (c->maxed) {
            int mi = monitor_at(c->x + c->w / 2, c->y + c->h / 2);
            Monitor &mon = mons[mi];
            c->x = mon.x;
            c->y = mon.y;
            c->w = mon.w;
            c->h = mon.h - w95::kTaskbarH;
            apply_geom(c.get());
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
    for (auto &m : mons) {
        draw_desktop(m);
        draw_taskbar(m);
    }
    ewmh_update();
}

void WM::restack_shell()
{
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
}

int WM::monitor_at(int x, int y)
{
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
    if (!c)
        return 0;
    if (c->iconic)
        return std::max(0, std::min(c->last_mon, (int)mons.size() - 1));
    return monitor_at(c->x + c->w / 2, c->y + 8);
}

Monitor *WM::mon_by_window(Window w)
{
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

bool WM::is_internal(Window w)
{
    if (w == rundlg || w == shutdlg || w == checkwin || w == root)
        return true;
    return mon_by_window(w) != nullptr;
}

Client *WM::find_client(Window w)
{
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
    std::vector<Client *> out;
    for (auto &c : clients)
        if (monitor_for(c.get()) == mi)
            out.push_back(c.get());
    return out;
}

void WM::set_wm_state(Client *c, long state)
{
    long data[2] = {state, None};
    XChangeProperty(dpy, c->win, wm_state, wm_state, 32, PropModeReplace, (unsigned char *)data, 2);
}

void WM::send_configure(Client *c)
{
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
    if (is_internal(w) || find_client(w) || find_frame(w))
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
    if (!c)
        return;
    if (focused == c)
        focused = nullptr;
    if (drag_c == c) {
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
    for (auto &m : mons)
        draw_taskbar(m);
    ewmh_update();
}

void WM::focus(Client *c)
{
    Client *old = focused;
    focused = c;
    if (old && old != c)
        draw_frame(old);
    if (c && !c->iconic) {
        XSetInputFocus(dpy, c->win, RevertToPointerRoot, CurrentTime);
        if (has_proto(c->win, wm_take_focus))
            send_client_message(c->win, wm_protocols, (long)wm_take_focus, CurrentTime);
        draw_frame(c);
        c->last_mon = monitor_for(c);
    } else if (!c) {
        XSetInputFocus(dpy, root, RevertToPointerRoot, CurrentTime);
    }
    for (auto &m : mons)
        draw_taskbar(m);
    ewmh_update();
}

void WM::raise_client(Client *c)
{
    if (!c)
        return;
    XRaiseWindow(dpy, c->frame);
    restack_shell();
}

void WM::close_client(Client *c)
{
    if (!c)
        return;
    if (has_proto(c->win, wm_delete))
        send_client_message(c->win, wm_protocols, (long)wm_delete, CurrentTime);
    else
        XKillClient(dpy, c->win);
}

void WM::minimize(Client *c)
{
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
    if (!c)
        return;
    c->iconic = false;
    XMapWindow(dpy, c->frame);
    set_wm_state(c, NormalState);
    raise_client(c);
    focus(c);
}

void WM::maximize_toggle(Client *c)
{
    if (!c)
        return;
    int mi = monitor_for(c);
    Monitor &m = mons[mi];
    if (!c->maxed) {
        c->rx = c->x;
        c->ry = c->y;
        c->rw = c->w;
        c->rh = c->h;
        c->x = m.x;
        c->y = m.y;
        c->w = m.w;
        c->h = m.h - w95::kTaskbarH;
        c->maxed = true;
    } else {
        c->x = c->rx;
        c->y = c->ry;
        c->w = c->rw;
        c->h = c->rh;
        c->maxed = false;
    }
    apply_geom(c);
    c->last_mon = mi;
    for (auto &mon : mons)
        draw_taskbar(mon);
}

Hit WM::hit_frame(Client *c, int x, int y)
{
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
    for (auto &m : mons) {
        if (m.start_open)
            XUnmapWindow(dpy, m.startmenu);
        if (m.sub_open)
            XUnmapWindow(dpy, m.submenu);
        m.start_open = m.sub_open = false;
        m.hover = m.subhover = -1;
        draw_taskbar(m);
    }
}

void WM::open_start(Monitor &m)
{
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
    m.hover = -1;
    XMapRaised(dpy, m.startmenu);
    draw_startmenu(m);
    draw_taskbar(m);
}

void WM::open_run(int mi)
{
    close_menus();
    dialog_mon = mi;
    Monitor &m = mons[mi];
    int w = 380, h = 148;
    int x = m.x + (m.w - w) / 2;
    int y = m.y + (m.h - w95::kTaskbarH - h) / 3;
    XMoveResizeWindow(dpy, rundlg, x, y, (unsigned)w, (unsigned)h);
    run_open = true;
    run_text.clear();
    XMapRaised(dpy, rundlg);
    XSetInputFocus(dpy, rundlg, RevertToPointerRoot, CurrentTime);
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
    draw_shutdlg();
}

static int dir_from_hit(Hit h)
{
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
        close_menus();
        raise_client(cl);
        focus(cl);
        XAllowEvents(dpy, ReplayPointer, CurrentTime);
        return;
    }

    if (b->window == rundlg) {
        int w = 380, h = 148, bw = 74, bh = 24;
        if (b->x >= w - 8 - w95::kBtn && b->y < w95::kTitleH + 6) {
            run_open = false;
            XUnmapWindow(dpy, rundlg);
            return;
        }
        if (b->y >= h - 34 && b->y < h - 34 + bh) {
            if (b->x >= w - 2 * bw - 20 && b->x < w - bw - 20) {
                if (!run_text.empty())
                    launch(run_text.c_str());
                run_open = false;
                XUnmapWindow(dpy, rundlg);
            } else if (b->x >= w - bw - 12 && b->x < w - 12) {
                run_open = false;
                XUnmapWindow(dpy, rundlg);
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
            }
        }
        return;
    }

    if (mon && b->window == mon->submenu && mon->sub_open) {
        int i = (b->y - 4) / w95::kMenuItemH;
        close_menus();
        if (i == 0)
            launch("aterm || xterm || x-terminal-emulator || true");
        else if (i == 1)
            launch("editor || aterm -e vi || xterm -e vi || true");
        else if (i == 2)
            launch("cabinet /");
        return;
    }

    if (mon && b->window == mon->startmenu && mon->start_open) {
        int y = 4;
        int idx = -1;
        for (int i = 0; i < kMenuN; i++) {
            int ih = kMenu[i].sep ? 8 : w95::kMenuItemH;
            if (b->x >= w95::kBannerW && b->y >= y && b->y < y + ih) {
                idx = i;
                break;
            }
            y += ih;
        }
        if (idx < 0 || kMenu[idx].sep || !kMenu[idx].enabled)
            return;
        if (idx == 0) {
            int mw = w95::kBannerW + w95::kMenuBodyW;
            int mh = menu_height();
            int sx = mon->x + 2 + mw - 4;
            int sy = mon->y + mon->h - w95::kTaskbarH - mh + 4;
            if (sx + 150 > mon->x + mon->w)
                sx = mon->x + 2 - 150 + 4;
            XMoveWindow(dpy, mon->submenu, sx, sy);
            mon->sub_open = true;
            mon->subhover = -1;
            XMapRaised(dpy, mon->submenu);
            draw_submenu(*mon);
            return;
        }
        int mi = mon_index(mon);
        close_menus();
        if (idx == 1)
            launch("cabinet \"$HOME\"");
        else if (idx == 4)
            launch("aterm -e sh -c 'echo Chime desktop; read x' || true");
        else if (idx == 5)
            open_run(mi);
        else if (idx == 7)
            open_shut(mi);
        return;
    }

    if (mon && b->window == mon->taskbar) {
        if (b->x >= 2 && b->x < 2 + w95::kStartW && b->y >= 3 && b->y < w95::kTaskbarH - 3) {
            open_start(*mon);
            return;
        }
        close_menus();
        int trayw = w95::kClockW;
        int trayx = mon->w - trayw - 4;
        if (b->x >= trayx)
            return;
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
            return;
        int pad = 12;
        int hit = -1;
        for (int i = 0; i < 4; i++) {
            int ix = pad;
            int iy = pad + i * w95::kCellH;
            if (b->x >= ix && b->x < ix + w95::kCellW && b->y >= iy && b->y < iy + w95::kCellH)
                hit = i;
        }
        if (hit >= 0) {
            if (selected_icon == hit && last_icon == hit && b->time - last_icon_time < w95::kDblClickMs) {
                if (hit == 0)
                    launch("cabinet /");
                else if (hit == 1)
                    launch("cabinet \"$HOME\"");
                else if (hit == 2)
                    launch("aterm -e sh -c 'echo Network Neighborhood; read x' || true");
                else
                    launch("aterm -e sh -c 'echo Recycle Bin is empty; read x' || true");
                selected_icon = -1;
            } else {
                selected_icon = hit;
            }
            last_icon = hit;
            last_icon_time = b->time;
            draw_desktop(*mon);
        } else {
            selected_icon = -1;
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
            if (last_title_win == fr->frame && b->time - last_title_time < w95::kDblClickMs)
                close_client(fr);
            last_title_win = fr->frame;
            last_title_time = b->time;
            return;
        }
        if (h == Hit::Title) {
            if (last_title_win == fr->frame && b->time - last_title_time < w95::kDblClickMs) {
                maximize_toggle(fr);
                last_title_win = 0;
                return;
            }
            last_title_win = fr->frame;
            last_title_time = b->time;
            if (fr->maxed)
                return;
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

void WM::on_button_release(XEvent *)
{
    if (drag != DragMode::Off) {
        if (drag_c)
            drag_c->last_mon = monitor_for(drag_c);
        drag = DragMode::Off;
        drag_c = nullptr;
        XUngrabPointer(dpy, CurrentTime);
        for (auto &m : mons)
            draw_taskbar(m);
    }
}

void WM::on_motion(XEvent *e)
{
    XMotionEvent *m = &e->xmotion;
    if (drag != DragMode::Off && drag_c) {
        int dx = m->x_root - drag_ox;
        int dy = m->y_root - drag_oy;
        if (drag == DragMode::Move) {
            drag_c->x = drag_fx + dx;
            drag_c->y = drag_fy + dy;
            apply_geom(drag_c);
        } else {
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
        int y = 4;
        int idx = -1;
        for (int i = 0; i < kMenuN; i++) {
            int ih = kMenu[i].sep ? 8 : w95::kMenuItemH;
            if (m->x >= w95::kBannerW && m->y >= y && m->y < y + ih)
                idx = i;
            y += ih;
        }
        if (idx != mon->hover) {
            mon->hover = idx;
            draw_startmenu(*mon);
            if (idx == 0 && kMenu[0].enabled) {
                int mw = w95::kBannerW + w95::kMenuBodyW;
                int mh = menu_height();
                int sx = mon->x + 2 + mw - 4;
                int sy = mon->y + mon->h - w95::kTaskbarH - mh + 4;
                if (sx + 150 > mon->x + mon->w)
                    sx = mon->x + 2 - 150 + 4;
                XMoveWindow(dpy, mon->submenu, sx, sy);
                mon->sub_open = true;
                XMapRaised(dpy, mon->submenu);
                draw_submenu(*mon);
            } else if (idx != 0 && mon->sub_open) {
                XUnmapWindow(dpy, mon->submenu);
                mon->sub_open = false;
            }
        }
    }
    if (mon && m->window == mon->submenu && mon->sub_open) {
        int i = (m->y - 4) / w95::kMenuItemH;
        if (i < 0 || i > 2)
            i = -1;
        if (i != mon->subhover) {
            mon->subhover = i;
            draw_submenu(*mon);
        }
    }
}

void WM::on_key(XEvent *e)
{
    KeySym ks = XLookupKeysym(&e->xkey, 0);
    if (run_open && e->xkey.window == rundlg) {
        char buf[16];
        KeySym ks2;
        int n = XLookupString(&e->xkey, buf, sizeof(buf), &ks2, nullptr);
        if (ks2 == XK_Return) {
            if (!run_text.empty())
                launch(run_text.c_str());
            run_open = false;
            XUnmapWindow(dpy, rundlg);
        } else if (ks2 == XK_Escape) {
            run_open = false;
            XUnmapWindow(dpy, rundlg);
        } else if (ks2 == XK_BackSpace) {
            if (!run_text.empty())
                run_text.pop_back();
            draw_rundlg();
        } else if (n == 1 && buf[0] >= 32 && buf[0] < 127) {
            run_text.push_back(buf[0]);
            draw_rundlg();
        }
        return;
    }
    if (ks == XK_F4 && (e->xkey.state & Mod1Mask) && focused)
        close_client(focused);
    if (ks == XK_Tab && (e->xkey.state & Mod1Mask) && !clients.empty()) {
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
    if ((ks == XK_Escape && (e->xkey.state & ControlMask)) || ks == XK_Super_L || ks == XK_Super_R) {
        Window rr, cr;
        int rx, ry, wx, wy;
        unsigned mask;
        XQueryPointer(dpy, root, &rr, &cr, &rx, &ry, &wx, &wy, &mask);
        open_start(mons[monitor_at(rx, ry)]);
    }
}

void WM::on_expose(XEvent *e)
{
    if (e->xexpose.count != 0)
        return;
    if (e->xexpose.window == rundlg) {
        draw_rundlg();
        return;
    }
    if (e->xexpose.window == shutdlg) {
        draw_shutdlg();
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
    if (have_randr && e->type == rr_event + RRScreenChangeNotify) {
        XRRUpdateConfiguration(e);
        sync_monitors();
        return;
    }
    switch (e->type) {
    case MapRequest:
        if (Client *c = find_client(e->xmaprequest.window))
            restore(c);
        else
            manage(e->xmaprequest.window);
        break;
    case ConfigureRequest: {
        XConfigureRequestEvent *cr = &e->xconfigurerequest;
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
            if (e->xunmap.send_event)
                unmanage(c, false);
            else if (e->xunmap.event == root)
                break;
            else
                unmanage(c, false);
        }
        break;
    case DestroyNotify:
        if (Client *c = find_client(e->xdestroywindow.window))
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
        on_key(e);
        break;
    case Expose:
        on_expose(e);
        break;
    case PropertyNotify:
        if (Client *c = find_client(e->xproperty.window)) {
            if (e->xproperty.atom == XA_WM_NAME || e->xproperty.atom == net_wm_name) {
                c->name = get_name(c->win);
                draw_frame(c);
                for (auto &m : mons)
                    draw_taskbar(m);
            }
        }
        break;
    case ClientMessage:
        if (e->xclient.message_type == net_active) {
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
    XSelectInput(dpy, root, SubstructureRedirectMask | SubstructureNotifyMask | ButtonPressMask | KeyPressMask | PropertyChangeMask);
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

    have_randr = XRRQueryExtension(dpy, &rr_event, &rr_error);
    if (have_randr)
        XRRSelectInput(dpy, root, RRScreenChangeNotifyMask | RROutputChangeNotifyMask | RRCrtcChangeNotifyMask);

    ewmh_init();
    rundlg = mkwin(0, 0, 380, 148, face.pix, ExposureMask | ButtonPressMask | KeyPressMask, true);
    shutdlg = mkwin(0, 0, 320, 120, face.pix, ExposureMask | ButtonPressMask | KeyPressMask, true);
    sync_monitors();

    auto grab = [&](KeySym ks, unsigned mod) {
        KeyCode kc = XKeysymToKeycode(dpy, ks);
        if (kc)
            XGrabKey(dpy, kc, mod, root, True, GrabModeAsync, GrabModeAsync);
    };
    grab(XK_Tab, Mod1Mask);
    grab(XK_Tab, Mod1Mask | ShiftMask);
    grab(XK_F4, Mod1Mask);
    grab(XK_Escape, ControlMask);
    grab(XK_Super_L, 0);
    grab(XK_Super_R, 0);

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
    return true;
}

void WM::run()
{
    int fd = ConnectionNumber(dpy);
    int last_min = -1;
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
        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(fd, &fds);
        struct timeval tv{0, 250000};
        select(fd + 1, &fds, nullptr, nullptr, &tv);
    }
}

void WM::finish()
{
    close_menus();
    if (dpy)
        XCloseDisplay(dpy);
    dpy = nullptr;
}
