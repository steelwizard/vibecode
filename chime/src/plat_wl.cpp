#ifdef CHIME_WAYLAND

#ifndef WLR_USE_UNSTABLE
#define WLR_USE_UNSTABLE
#endif

#include "plat_wl.h"

#include <X11/cursorfont.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <memory>
#include <string>
#include <vector>

#include <drm/drm_fourcc.h>
#include <linux/input-event-codes.h>
#include <unistd.h>
#include <xkbcommon/xkbcommon.h>

extern "C" {
#include <wayland-server-core.h>
#include <wlr/backend.h>
#include <wlr/interfaces/wlr_buffer.h>
#include <wlr/render/allocator.h>
#include <wlr/render/wlr_renderer.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_cursor.h>
#include <wlr/types/wlr_data_device.h>
#include <wlr/types/wlr_keyboard.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_pointer.h>
#include <wlr/types/wlr_output_layout.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/types/wlr_subcompositor.h>
#include <wlr/types/wlr_xcursor_manager.h>
#include <wlr/types/wlr_xdg_decoration_v1.h>
#include <wlr/types/wlr_xdg_shell.h>
#include <wlr/util/log.h>
#include <wlr/xwayland.h>
}

// wlroots compositor: scene graph holds Chime chrome (software ARGB
// buffers) and client surfaces (xdg-shell / Xwayland). Input is translated
// into the same XEvent-shaped messages the X11 path already handles.

#define from_l(type, ptr, member) ((type *)((char *)(ptr) - offsetof(type, member)))

namespace {

struct PixelBuffer {
    wlr_buffer base;
    std::vector<uint32_t> pixels;
    int w = 0, h = 0;
};

struct WlSurf {
    Window id = 0;
    int x = 0, y = 0, w = 1, h = 1;
    int layer = 1;
    bool mapped = false;
    bool pixmap = false;
    bool client = false;
    unsigned long bg = 0;
    std::vector<uint32_t> argb;
    PixelBuffer *pb = nullptr;
    wlr_scene_tree *tree = nullptr;
    wlr_scene_buffer *sbuf = nullptr;
    wlr_scene_tree *view_tree = nullptr;
    wlr_xdg_toplevel *xdg = nullptr;
    wlr_xwayland_surface *xwl = nullptr;
};

struct WlOutput {
    WM *wm = nullptr;
    wlr_output *output = nullptr;
    wl_listener frame{};
    wl_listener destroy{};
    wl_listener request_state{};
};

struct WlKbd {
    WM *wm = nullptr;
    wlr_keyboard *kbd = nullptr;
    wl_listener modifiers{};
    wl_listener key{};
    wl_listener destroy{};
};

struct WlView {
    WM *wm = nullptr;
    Window win = 0;
    wlr_xdg_toplevel *xdg = nullptr;
    wlr_xwayland_surface *xwl = nullptr;
    wl_listener map{};
    wl_listener unmap{};
    wl_listener destroy{};
    wl_listener commit{};
    wl_listener set_title{};
    wl_listener request_move{};
    wl_listener request_resize{};
    wl_listener request_maximize{};
    wl_listener request_minimize{};
    wl_listener request_fullscreen{};
    wl_listener associate{};
    wl_listener dissociate{};
    wl_listener request_configure{};
    wl_listener request_close{};
};

struct WlDeco {
    WM *wm = nullptr;
    wlr_xdg_toplevel_decoration_v1 *deco = nullptr;
    wl_listener request_mode{};
    wl_listener destroy{};
};

struct WlServer {
    WM *wm = nullptr;
    wl_display *display = nullptr;
    wlr_backend *backend = nullptr;
    wlr_session *session = nullptr;
    wlr_renderer *renderer = nullptr;
    wlr_allocator *allocator = nullptr;
    wlr_compositor *compositor = nullptr;
    wlr_scene *scene = nullptr;
    wlr_scene_tree *layer[4]{};
    wlr_output_layout *layout = nullptr;
    wlr_scene_output_layout *scene_layout = nullptr;
    wlr_xdg_shell *xdg_shell = nullptr;
    wlr_seat *seat = nullptr;
    wlr_cursor *cursor = nullptr;
    wlr_xcursor_manager *cursor_mgr = nullptr;
    wlr_xwayland *xwayland = nullptr;
    wlr_xdg_decoration_manager_v1 *deco_mgr = nullptr;

    wl_listener new_output{};
    wl_listener new_input{};
    wl_listener new_xdg_toplevel{};
    wl_listener new_xdg_popup{};
    wl_listener new_xwayland{};
    wl_listener xwayland_ready{};
    wl_listener request_cursor{};
    wl_listener request_set_selection{};
    wl_listener new_decoration{};
    wl_listener cursor_motion{};
    wl_listener cursor_motion_abs{};
    wl_listener cursor_button{};
    wl_listener cursor_axis{};
    wl_listener cursor_frame{};

    std::vector<std::unique_ptr<WlSurf>> surfs;
    std::vector<std::unique_ptr<WlOutput>> outputs;
    std::vector<std::unique_ptr<WlKbd>> kbds;
    std::vector<std::unique_ptr<WlView>> views;
    std::vector<std::unique_ptr<WlDeco>> decos;

    Window next_id = 1;
    Window grab_w = 0;
    uint32_t seat_caps = 0;
    Cursor last_cursor = 0;

    WlSurf *find(Window id)
    {
        for (auto &s : surfs)
            if (s->id == id)
                return s.get();
        return nullptr;
    }
    Window alloc_id() { return next_id++; }
};

static WlServer *srv(WM *wm)
{
    return wm ? (WlServer *)wm->wl : nullptr;
}

static void pb_destroy(wlr_buffer *buffer)
{
    auto *p = (PixelBuffer *)buffer;
    wlr_buffer_finish(&p->base);
    delete p;
}

static bool pb_begin(wlr_buffer *buffer, uint32_t, void **data, uint32_t *format, size_t *stride)
{
    auto *p = (PixelBuffer *)buffer;
    *data = p->pixels.data();
    *format = DRM_FORMAT_ARGB8888;
    *stride = (size_t)p->w * 4;
    return true;
}

static void pb_end(wlr_buffer *) {}

static const wlr_buffer_impl kPbImpl = {
    .destroy = pb_destroy,
    .get_dmabuf = nullptr,
    .get_shm = nullptr,
    .begin_data_ptr_access = pb_begin,
    .end_data_ptr_access = pb_end,
};

static PixelBuffer *pb_create(int w, int h, const uint32_t *src)
{
    auto *p = new PixelBuffer();
    p->w = w;
    p->h = h;
    p->pixels.assign((size_t)w * (size_t)h, 0xFF000000u);
    if (src)
        memcpy(p->pixels.data(), src, (size_t)w * (size_t)h * 4);
    wlr_buffer_init(&p->base, &kPbImpl, w, h);
    return p;
}

static void surf_upload(WlServer *s, WlSurf *sf)
{
    (void)s;
    if (!sf || sf->pixmap || sf->client || !sf->sbuf)
        return;
    if (sf->w < 1 || sf->h < 1)
        return;
    if ((int)sf->argb.size() != sf->w * sf->h)
        sf->argb.assign((size_t)sf->w * (size_t)sf->h, 0xFF000000u);
    if (sf->pb) {
        wlr_buffer_drop(&sf->pb->base);
        sf->pb = nullptr;
    }
    sf->pb = pb_create(sf->w, sf->h, sf->argb.data());
    wlr_scene_buffer_set_buffer(sf->sbuf, &sf->pb->base);
}

static void surf_resize_buf(WlServer *s, WlSurf *sf, int w, int h)
{
    if (!sf || w < 1 || h < 1)
        return;
    if (sf->w == w && sf->h == h && !sf->argb.empty())
        return;
    std::vector<uint32_t> old = std::move(sf->argb);
    int ow = sf->w, oh = sf->h;
    sf->w = w;
    sf->h = h;
    uint32_t fill = 0xFF000000u | (uint32_t)(sf->bg & 0xFFFFFFu);
    sf->argb.assign((size_t)w * (size_t)h, fill);
    int cw = std::min(ow, w), ch = std::min(oh, h);
    for (int y = 0; y < ch; y++)
        memcpy(&sf->argb[(size_t)y * w], &old[(size_t)y * ow], (size_t)cw * 4);
    if (!sf->pixmap && !sf->client)
        surf_upload(s, sf);
}

static const char *cursor_name(Cursor c)
{
    switch (c) {
    case XC_fleur:
        return "fleur";
    case XC_top_side:
        return "top_side";
    case XC_bottom_side:
        return "bottom_side";
    case XC_right_side:
        return "right_side";
    case XC_left_side:
        return "left_side";
    case XC_top_right_corner:
        return "top_right_corner";
    case XC_top_left_corner:
        return "top_left_corner";
    case XC_bottom_right_corner:
        return "bottom_right_corner";
    case XC_bottom_left_corner:
        return "bottom_left_corner";
    default:
        return "left_ptr";
    }
}

static unsigned seat_mods(wlr_keyboard *kbd)
{
    if (!kbd)
        return 0;
    uint32_t m = wlr_keyboard_get_modifiers(kbd);
    unsigned st = 0;
    if (m & WLR_MODIFIER_SHIFT)
        st |= ShiftMask;
    if (m & WLR_MODIFIER_CAPS)
        st |= LockMask;
    if (m & WLR_MODIFIER_CTRL)
        st |= ControlMask;
    if (m & WLR_MODIFIER_ALT)
        st |= Mod1Mask;
    if (m & WLR_MODIFIER_MOD2)
        st |= Mod2Mask;
    if (m & WLR_MODIFIER_LOGO)
        st |= Mod4Mask;
    return st;
}

static unsigned btn_mask(uint32_t held)
{
    unsigned st = 0;
    if (held & (1u << 1))
        st |= Button1Mask;
    if (held & (1u << 2))
        st |= Button2Mask;
    if (held & (1u << 3))
        st |= Button3Mask;
    return st;
}

static int linux_to_xbtn(uint32_t b)
{
    if (b == BTN_LEFT)
        return 1;
    if (b == BTN_MIDDLE)
        return 2;
    if (b == BTN_RIGHT)
        return 3;
    if (b == BTN_SIDE || b == 0x113)
        return 8;
    if (b == BTN_EXTRA)
        return 9;
    return 0;
}

static wlr_keyboard *first_kbd(WlServer *s)
{
    if (s->kbds.empty())
        return nullptr;
    return s->kbds[0]->kbd;
}

static WlSurf *surf_at(WlServer *s, double lx, double ly, double *sx, double *sy, wlr_surface **out_surf)
{
    if (out_surf)
        *out_surf = nullptr;
    double nx = 0, ny = 0;
    wlr_scene_node *node = wlr_scene_node_at(&s->scene->tree.node, lx, ly, &nx, &ny);
    if (!node)
        return nullptr;
    if (sx)
        *sx = nx;
    if (sy)
        *sy = ny;
    if (node->type == WLR_SCENE_NODE_BUFFER) {
        wlr_scene_buffer *sb = wlr_scene_buffer_from_node(node);
        wlr_scene_surface *ss = wlr_scene_surface_try_from_buffer(sb);
        if (ss && out_surf)
            *out_surf = ss->surface;
    }
    for (wlr_scene_node *n = node; n; n = n->parent ? &n->parent->node : nullptr) {
        if (n->data)
            return s->find((Window)(uintptr_t)n->data);
    }
    return nullptr;
}

static void send_button(WM *wm, Window w, int x, int y, int rx, int ry, unsigned btn, bool press, unsigned state)
{
    XEvent e{};
    e.type = press ? ButtonPress : ButtonRelease;
    e.xbutton.window = w;
    e.xbutton.root = wm->root;
    e.xbutton.subwindow = 0;
    e.xbutton.time = CurrentTime;
    e.xbutton.x = x;
    e.xbutton.y = y;
    e.xbutton.x_root = rx;
    e.xbutton.y_root = ry;
    e.xbutton.state = state;
    e.xbutton.button = btn;
    e.xbutton.same_screen = True;
    wm->handle_event(&e);
}

static void send_motion(WM *wm, Window w, int x, int y, int rx, int ry, unsigned state)
{
    XEvent e{};
    e.type = MotionNotify;
    e.xmotion.window = w;
    e.xmotion.root = wm->root;
    e.xmotion.time = CurrentTime;
    e.xmotion.x = x;
    e.xmotion.y = y;
    e.xmotion.x_root = rx;
    e.xmotion.y_root = ry;
    e.xmotion.state = state;
    e.xmotion.same_screen = True;
    wm->handle_event(&e);
}

static void send_key(WM *wm, KeySym ks, unsigned state, bool press, const char *text, int n)
{
    wm->key_text_n = 0;
    if (text && n > 0) {
        n = std::min(n, 31);
        memcpy(wm->key_text, text, (size_t)n);
        wm->key_text[n] = 0;
        wm->key_text_n = n;
    }
    XEvent e{};
    e.type = press ? KeyPress : KeyRelease;
    e.xkey.window = wm->kbd_grab_w ? wm->kbd_grab_w : wm->root;
    e.xkey.root = wm->root;
    e.xkey.time = CurrentTime;
    e.xkey.state = state;
    e.xkey.keycode = (unsigned)ks;
    e.xkey.same_screen = True;
    wm->handle_event(&e);
}

static WlView *view_by_win(WlServer *s, Window w)
{
    for (auto &v : s->views)
        if (v->win == w)
            return v.get();
    return nullptr;
}

static void focus_surface(WlServer *s, wlr_surface *surf)
{
    if (!s->seat)
        return;
    wlr_keyboard *kbd = first_kbd(s);
    if (surf) {
        wlr_seat_keyboard_notify_enter(s->seat, surf, kbd ? kbd->keycodes : nullptr, kbd ? kbd->num_keycodes : 0,
                                       kbd ? &kbd->modifiers : nullptr);
    } else
        wlr_seat_keyboard_notify_clear_focus(s->seat);
}

static void output_frame(wl_listener *listener, void *)
{
    WlOutput *o = from_l(WlOutput, listener, frame);
    WlServer *s = srv(o->wm);
    if (!s)
        return;
    wlr_scene_output *so = wlr_scene_get_scene_output(s->scene, o->output);
    if (!so)
        return;
    wlr_scene_output_commit(so, nullptr);
    timespec now{};
    clock_gettime(CLOCK_MONOTONIC, &now);
    wlr_scene_output_send_frame_done(so, &now);
}

static void output_request_state(wl_listener *listener, void *data)
{
    WlOutput *o = from_l(WlOutput, listener, request_state);
    auto *event = (wlr_output_event_request_state *)data;
    wlr_output_commit_state(o->output, event->state);
}

static void output_destroy(wl_listener *listener, void *)
{
    WlOutput *o = from_l(WlOutput, listener, destroy);
    WlServer *s = srv(o->wm);
    if (!s)
        return;
    wl_list_remove(&o->frame.link);
    wl_list_remove(&o->destroy.link);
    wl_list_remove(&o->request_state.link);
    WM *wm = o->wm;
    bool running = wm && wm->running;
    s->outputs.erase(std::remove_if(s->outputs.begin(), s->outputs.end(), [&](auto &p) { return p.get() == o; }),
                     s->outputs.end());
    if (running)
        wm->sync_monitors();
}

static void new_output(wl_listener *listener, void *data)
{
    WlServer *s = from_l(WlServer, listener, new_output);
    auto *output = (wlr_output *)data;
    wlr_output_init_render(output, s->allocator, s->renderer);

    wlr_output_state st;
    wlr_output_state_init(&st);
    wlr_output_state_set_enabled(&st, true);
    if (wlr_output_mode *mode = wlr_output_preferred_mode(output))
        wlr_output_state_set_mode(&st, mode);
    wlr_output_commit_state(output, &st);
    wlr_output_state_finish(&st);

    auto o = std::make_unique<WlOutput>();
    o->wm = s->wm;
    o->output = output;
    o->frame.notify = output_frame;
    wl_signal_add(&output->events.frame, &o->frame);
    o->request_state.notify = output_request_state;
    wl_signal_add(&output->events.request_state, &o->request_state);
    o->destroy.notify = output_destroy;
    wl_signal_add(&output->events.destroy, &o->destroy);

    wlr_output_layout_output *l_output = wlr_output_layout_add_auto(s->layout, output);
    wlr_scene_output *scene_output = wlr_scene_output_create(s->scene, output);
    wlr_scene_output_layout_add_output(s->scene_layout, l_output, scene_output);
    wlr_xcursor_manager_load(s->cursor_mgr, (float)output->scale);
    s->outputs.push_back(std::move(o));
    if (s->wm->running || !s->wm->mons.empty() || s->outputs.size() == 1) {
        wlr_box box{};
        wlr_output_layout_get_box(s->layout, nullptr, &box);
        s->wm->sw = box.width;
        s->wm->sh = box.height;
        if (!s->wm->mons.empty() || s->wm->running)
            s->wm->sync_monitors();
    }
}

static void kbd_modifiers(wl_listener *listener, void *)
{
    WlKbd *k = from_l(WlKbd, listener, modifiers);
    WlServer *s = srv(k->wm);
    wlr_seat_set_keyboard(s->seat, k->kbd);
    wlr_seat_keyboard_notify_modifiers(s->seat, &k->kbd->modifiers);
}

static bool compositor_key(WM *wm, xkb_keysym_t sym, unsigned st, bool press)
{
    (void)wm;
    if (!press)
        return false;
    if ((st & ControlMask) && (sym == XKB_KEY_Escape || sym == XKB_KEY_Escape))
        return false;
    if ((st & Mod1Mask) && (sym == XKB_KEY_F4 || sym == XKB_KEY_Tab || sym == XKB_KEY_ISO_Left_Tab))
        return false;
    if (sym == XKB_KEY_Super_L || sym == XKB_KEY_Super_R)
        return false;
    if ((st & Mod4Mask) && (sym == XKB_KEY_r || sym == XKB_KEY_R))
        return false;
    return false;
}

static void kbd_key(wl_listener *listener, void *data)
{
    WlKbd *k = from_l(WlKbd, listener, key);
    WlServer *s = srv(k->wm);
    auto *event = (wlr_keyboard_key_event *)data;
    wlr_seat_set_keyboard(s->seat, k->kbd);
    uint32_t keycode = event->keycode + 8;
    const xkb_keysym_t *syms = nullptr;
    int nsyms = xkb_state_key_get_syms(k->kbd->xkb_state, keycode, &syms);
    unsigned st = seat_mods(k->kbd);
    bool press = event->state == WL_KEYBOARD_KEY_STATE_PRESSED;
    char text[32]{};
    int tn = xkb_state_key_get_utf8(k->kbd->xkb_state, keycode, text, sizeof text);
    if (tn < 0)
        tn = 0;

    bool handled = false;
    if (nsyms > 0) {
        xkb_keysym_t sym = syms[0];
        if (k->wm->kbd_grab_w || compositor_key(k->wm, sym, st, press) ||
            (st & (Mod1Mask | ControlMask | Mod4Mask)) || sym == XKB_KEY_Super_L || sym == XKB_KEY_Super_R) {
            send_key(k->wm, (KeySym)sym, st, press, text, tn);
            handled = true;
        }
    }
    if (!handled && s->seat->keyboard_state.focused_surface)
        wlr_seat_keyboard_notify_key(s->seat, event->time_msec, event->keycode, event->state);
    else if (!handled && nsyms > 0)
        send_key(k->wm, (KeySym)syms[0], st, press, text, tn);
}

static void kbd_destroy(wl_listener *listener, void *)
{
    WlKbd *k = from_l(WlKbd, listener, destroy);
    WlServer *s = srv(k->wm);
    wl_list_remove(&k->modifiers.link);
    wl_list_remove(&k->key.link);
    wl_list_remove(&k->destroy.link);
    s->kbds.erase(std::remove_if(s->kbds.begin(), s->kbds.end(), [&](auto &p) { return p.get() == k; }), s->kbds.end());
}

static void new_input(wl_listener *listener, void *data)
{
    WlServer *s = from_l(WlServer, listener, new_input);
    auto *dev = (wlr_input_device *)data;
    if (dev->type == WLR_INPUT_DEVICE_KEYBOARD) {
        auto k = std::make_unique<WlKbd>();
        k->wm = s->wm;
        k->kbd = wlr_keyboard_from_input_device(dev);
        xkb_context *ctx = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
        xkb_keymap *keymap = xkb_keymap_new_from_names(ctx, nullptr, XKB_KEYMAP_COMPILE_NO_FLAGS);
        wlr_keyboard_set_keymap(k->kbd, keymap);
        xkb_keymap_unref(keymap);
        xkb_context_unref(ctx);
        wlr_keyboard_set_repeat_info(k->kbd, 25, 600);
        k->modifiers.notify = kbd_modifiers;
        wl_signal_add(&k->kbd->events.modifiers, &k->modifiers);
        k->key.notify = kbd_key;
        wl_signal_add(&k->kbd->events.key, &k->key);
        k->destroy.notify = kbd_destroy;
        wl_signal_add(&dev->events.destroy, &k->destroy);
        wlr_seat_set_keyboard(s->seat, k->kbd);
        s->kbds.push_back(std::move(k));
        s->seat_caps |= WL_SEAT_CAPABILITY_KEYBOARD;
    } else if (dev->type == WLR_INPUT_DEVICE_POINTER) {
        wlr_cursor_attach_input_device(s->cursor, dev);
        s->seat_caps |= WL_SEAT_CAPABILITY_POINTER;
    }
    wlr_seat_set_capabilities(s->seat, s->seat_caps);
}

static void process_cursor_move(WlServer *s, uint32_t time)
{
    (void)time;
    double lx = s->cursor->x, ly = s->cursor->y;
    unsigned st = seat_mods(first_kbd(s)) | btn_mask(s->wm->drag != DragMode::Off ? Button1Mask : 0);
    if (s->grab_w) {
        WlSurf *g = s->find(s->grab_w);
        int gx = g ? g->x : 0, gy = g ? g->y : 0;
        send_motion(s->wm, s->grab_w, (int)lx - gx, (int)ly - gy, (int)lx, (int)ly, st | Button1Mask);
        return;
    }
    double sx = 0, sy = 0;
    wlr_surface *surf = nullptr;
    WlSurf *hit = surf_at(s, lx, ly, &sx, &sy, &surf);
    if (surf) {
        wlr_seat_pointer_notify_enter(s->seat, surf, sx, sy);
        wlr_seat_pointer_notify_motion(s->seat, time, sx, sy);
    } else {
        wlr_seat_pointer_clear_focus(s->seat);
        if (hit)
            send_motion(s->wm, hit->id, (int)sx, (int)sy, (int)lx, (int)ly, st);
    }
}

static void cursor_motion(wl_listener *listener, void *data)
{
    WlServer *s = from_l(WlServer, listener, cursor_motion);
    auto *ev = (wlr_pointer_motion_event *)data;
    wlr_cursor_move(s->cursor, &ev->pointer->base, ev->delta_x, ev->delta_y);
    process_cursor_move(s, ev->time_msec);
}

static void cursor_motion_abs(wl_listener *listener, void *data)
{
    WlServer *s = from_l(WlServer, listener, cursor_motion_abs);
    auto *ev = (wlr_pointer_motion_absolute_event *)data;
    wlr_cursor_warp_absolute(s->cursor, &ev->pointer->base, ev->x, ev->y);
    process_cursor_move(s, ev->time_msec);
}

static void cursor_button(wl_listener *listener, void *data)
{
    WlServer *s = from_l(WlServer, listener, cursor_button);
    auto *ev = (wlr_pointer_button_event *)data;
    bool press = ev->state == WL_POINTER_BUTTON_STATE_PRESSED;
    int btn = linux_to_xbtn(ev->button);
    if (ev->button == BTN_LEFT)
        btn = 1;
    if (!btn)
        btn = 1;
    unsigned st = seat_mods(first_kbd(s));
    double lx = s->cursor->x, ly = s->cursor->y;
    if (s->grab_w && !press) {
        WlSurf *g = s->find(s->grab_w);
        int gx = g ? g->x : 0, gy = g ? g->y : 0;
        send_button(s->wm, s->grab_w, (int)lx - gx, (int)ly - gy, (int)lx, (int)ly, (unsigned)btn, false, st | Button1Mask);
        wlr_seat_pointer_notify_button(s->seat, ev->time_msec, ev->button, ev->state);
        return;
    }
    if (s->grab_w && press) {
        WlSurf *g = s->find(s->grab_w);
        int gx = g ? g->x : 0, gy = g ? g->y : 0;
        send_button(s->wm, s->grab_w, (int)lx - gx, (int)ly - gy, (int)lx, (int)ly, (unsigned)btn, true, st);
        return;
    }
    double sx = 0, sy = 0;
    wlr_surface *surf = nullptr;
    WlSurf *hit = surf_at(s, lx, ly, &sx, &sy, &surf);
    if (surf && press) {
        Window cw = 0;
        if (hit && hit->client)
            cw = hit->id;
        else {
            for (auto &v : s->views) {
                wlr_surface *vs = nullptr;
                if (v->xdg)
                    vs = v->xdg->base->surface;
                else if (v->xwl)
                    vs = v->xwl->surface;
                if (vs && vs == surf) {
                    cw = v->win;
                    break;
                }
            }
        }
        if (cw) {
            if (Client *c = s->wm->find_client(cw)) {
                s->wm->close_menus();
                s->wm->raise_client(c);
                s->wm->focus(c);
            }
        }
        wlr_seat_pointer_notify_enter(s->seat, surf, sx, sy);
        wlr_seat_pointer_notify_button(s->seat, ev->time_msec, ev->button, ev->state);
        return;
    }
    if (surf) {
        wlr_seat_pointer_notify_button(s->seat, ev->time_msec, ev->button, ev->state);
        return;
    }
    if (hit)
        send_button(s->wm, hit->id, (int)sx, (int)sy, (int)lx, (int)ly, (unsigned)btn, press, st);
}

static void cursor_axis(wl_listener *listener, void *data)
{
    WlServer *s = from_l(WlServer, listener, cursor_axis);
    auto *ev = (wlr_pointer_axis_event *)data;
    wlr_seat_pointer_notify_axis(s->seat, ev->time_msec, ev->orientation, ev->delta, ev->delta_discrete, ev->source,
                                 ev->relative_direction);
}

static void cursor_frame(wl_listener *listener, void *)
{
    WlServer *s = from_l(WlServer, listener, cursor_frame);
    wlr_seat_pointer_notify_frame(s->seat);
}

static void request_cursor(wl_listener *listener, void *data)
{
    WlServer *s = from_l(WlServer, listener, request_cursor);
    auto *ev = (wlr_seat_pointer_request_set_cursor_event *)data;
    wlr_seat_client *focused = s->seat->pointer_state.focused_client;
    if (focused == ev->seat_client)
        wlr_cursor_set_surface(s->cursor, ev->surface, ev->hotspot_x, ev->hotspot_y);
}

static void request_set_selection(wl_listener *listener, void *data)
{
    WlServer *s = from_l(WlServer, listener, request_set_selection);
    auto *ev = (wlr_seat_request_set_selection_event *)data;
    wlr_seat_set_selection(s->seat, ev->source, ev->serial);
}

static void view_map(wl_listener *listener, void *)
{
    WlView *v = from_l(WlView, listener, map);
    WlServer *s = srv(v->wm);
    if (!v->win)
        return;
    WlSurf *sf = s->find(v->win);
    if (!sf)
        return;
    sf->mapped = true;
    if (v->xwl && v->xwl->surface && !sf->view_tree) {
        int layer = v->xwl->override_redirect ? 3 : 1;
        sf->view_tree = wlr_scene_subsurface_tree_create(s->layer[layer], v->xwl->surface);
        sf->tree = sf->view_tree;
        sf->tree->node.data = (void *)(uintptr_t)sf->id;
    }
    if (sf->tree)
        wlr_scene_node_set_enabled(&sf->tree->node, true);
    if (v->xwl && v->xwl->override_redirect) {
        if (sf->tree)
            wlr_scene_node_set_position(&sf->tree->node, v->xwl->x, v->xwl->y);
        return;
    }
    if (!v->wm->find_client(v->win))
        v->wm->manage(v->win);
}

static void view_unmap(wl_listener *listener, void *)
{
    WlView *v = from_l(WlView, listener, unmap);
    if (Client *c = v->wm->find_client(v->win)) {
        if (c->ignore_unmap) {
            c->ignore_unmap--;
            return;
        }
        v->wm->unmanage(c, false);
    }
    if (WlServer *s = srv(v->wm)) {
        if (WlSurf *sf = s->find(v->win)) {
            sf->mapped = false;
            if (sf->tree)
                wlr_scene_node_set_enabled(&sf->tree->node, false);
        }
    }
}

static void drop_listener(wl_listener *l)
{
    if (l->link.next) {
        wl_list_remove(&l->link);
        l->link.next = l->link.prev = nullptr;
    }
}

static void view_destroy(wl_listener *listener, void *)
{
    WlView *v = from_l(WlView, listener, destroy);
    WlServer *s = srv(v->wm);
    if (Client *c = v->wm->find_client(v->win))
        v->wm->unmanage(c, true);
    if (s) {
        if (WlSurf *sf = s->find(v->win)) {
            sf->xdg = nullptr;
            sf->xwl = nullptr;
            sf->view_tree = nullptr;
        }
        drop_listener(&v->map);
        drop_listener(&v->unmap);
        drop_listener(&v->destroy);
        drop_listener(&v->commit);
        drop_listener(&v->set_title);
        drop_listener(&v->request_move);
        drop_listener(&v->request_resize);
        drop_listener(&v->request_maximize);
        drop_listener(&v->request_minimize);
        drop_listener(&v->request_fullscreen);
        drop_listener(&v->associate);
        drop_listener(&v->dissociate);
        drop_listener(&v->request_configure);
        drop_listener(&v->request_close);
        s->views.erase(std::remove_if(s->views.begin(), s->views.end(), [&](auto &p) { return p.get() == v; }),
                       s->views.end());
    }
}

static void view_commit(wl_listener *listener, void *)
{
    WlView *v = from_l(WlView, listener, commit);
    if (v->xdg && v->xdg->base->initial_commit) {
        wlr_xdg_toplevel_set_wm_capabilities(v->xdg, WLR_XDG_TOPLEVEL_WM_CAPABILITIES_MAXIMIZE |
                                                         WLR_XDG_TOPLEVEL_WM_CAPABILITIES_MINIMIZE |
                                                         WLR_XDG_TOPLEVEL_WM_CAPABILITIES_FULLSCREEN);
        wlr_xdg_toplevel_set_size(v->xdg, 0, 0);
    }
}

static void view_title(wl_listener *listener, void *)
{
    WlView *v = from_l(WlView, listener, set_title);
    if (Client *c = v->wm->find_client(v->win)) {
        c->name = wl_client_name(v->wm, v->win);
        v->wm->draw_frame(c);
        for (auto &m : v->wm->mons)
            v->wm->draw_taskbar(m);
    }
}

static void view_req_move(wl_listener *listener, void *)
{
    WlView *v = from_l(WlView, listener, request_move);
    if (Client *c = v->wm->find_client(v->win)) {
        v->wm->raise_client(c);
        v->wm->focus(c);
    }
}

static void view_req_max(wl_listener *listener, void *)
{
    WlView *v = from_l(WlView, listener, request_maximize);
    if (Client *c = v->wm->find_client(v->win))
        v->wm->maximize_toggle(c);
    if (v->xdg)
        wlr_xdg_surface_schedule_configure(v->xdg->base);
}

static void view_req_min(wl_listener *listener, void *)
{
    WlView *v = from_l(WlView, listener, request_minimize);
    if (Client *c = v->wm->find_client(v->win))
        v->wm->minimize(c);
}

static void view_req_fs(wl_listener *listener, void *)
{
    WlView *v = from_l(WlView, listener, request_fullscreen);
    if (Client *c = v->wm->find_client(v->win))
        v->wm->maximize_toggle(c);
    if (v->xdg)
        wlr_xdg_surface_schedule_configure(v->xdg->base);
}

static void xwl_configure(wl_listener *listener, void *data)
{
    WlView *v = from_l(WlView, listener, request_configure);
    auto *ev = (wlr_xwayland_surface_configure_event *)data;
    if (Client *c = v->wm->find_client(v->win)) {
        if (ev->width)
            c->w = ev->width + 2 * w95::kFrameB;
        if (ev->height)
            c->h = ev->height + 2 * w95::kFrameB + w95::kTitleH;
        v->wm->apply_geom(c);
    } else if (v->xwl)
        wlr_xwayland_surface_configure(v->xwl, ev->x, ev->y, ev->width, ev->height);
}

static void xwl_associate(wl_listener *listener, void *)
{
    WlView *v = from_l(WlView, listener, associate);
    if (!v->xwl || !v->xwl->surface)
        return;
    v->map.notify = view_map;
    wl_signal_add(&v->xwl->surface->events.map, &v->map);
    v->unmap.notify = view_unmap;
    wl_signal_add(&v->xwl->surface->events.unmap, &v->unmap);
}

static void xwl_dissociate(wl_listener *listener, void *)
{
    WlView *v = from_l(WlView, listener, dissociate);
    if (v->map.link.next)
        wl_list_remove(&v->map.link);
    if (v->unmap.link.next)
        wl_list_remove(&v->unmap.link);
    v->map.link = {};
    v->unmap.link = {};
}

static void new_xdg_toplevel(wl_listener *listener, void *data)
{
    WlServer *s = from_l(WlServer, listener, new_xdg_toplevel);
    auto *xdg = (wlr_xdg_toplevel *)data;
    auto v = std::make_unique<WlView>();
    v->wm = s->wm;
    v->xdg = xdg;
    WlSurf sf{};
    sf.id = s->alloc_id();
    sf.client = true;
    sf.xdg = xdg;
    sf.w = 200;
    sf.h = 150;
    sf.view_tree = wlr_scene_xdg_surface_create(s->layer[1], xdg->base);
    sf.tree = sf.view_tree;
    wlr_scene_node_set_enabled(&sf.tree->node, false);
    sf.tree->node.data = (void *)(uintptr_t)sf.id;
    v->win = sf.id;
    xdg->base->data = (void *)(uintptr_t)sf.id;
    s->surfs.push_back(std::make_unique<WlSurf>(sf));

    v->map.notify = view_map;
    wl_signal_add(&xdg->base->surface->events.map, &v->map);
    v->unmap.notify = view_unmap;
    wl_signal_add(&xdg->base->surface->events.unmap, &v->unmap);
    v->destroy.notify = view_destroy;
    wl_signal_add(&xdg->events.destroy, &v->destroy);
    v->commit.notify = view_commit;
    wl_signal_add(&xdg->base->surface->events.commit, &v->commit);
    v->set_title.notify = view_title;
    wl_signal_add(&xdg->events.set_title, &v->set_title);
    v->request_move.notify = view_req_move;
    wl_signal_add(&xdg->events.request_move, &v->request_move);
    v->request_resize.notify = view_req_move;
    wl_signal_add(&xdg->events.request_resize, &v->request_resize);
    v->request_maximize.notify = view_req_max;
    wl_signal_add(&xdg->events.request_maximize, &v->request_maximize);
    v->request_minimize.notify = view_req_min;
    wl_signal_add(&xdg->events.request_minimize, &v->request_minimize);
    v->request_fullscreen.notify = view_req_fs;
    wl_signal_add(&xdg->events.request_fullscreen, &v->request_fullscreen);
    s->views.push_back(std::move(v));
}

static void new_xdg_popup(wl_listener *listener, void *data)
{
    WlServer *s = from_l(WlServer, listener, new_xdg_popup);
    auto *popup = (wlr_xdg_popup *)data;
    wlr_xdg_surface *parent = wlr_xdg_surface_try_from_wlr_surface(popup->parent);
    if (!parent)
        return;
    Window pid = (Window)(uintptr_t)parent->data;
    WlSurf *ps = s->find(pid);
    wlr_scene_tree *ptree = ps && ps->view_tree ? ps->view_tree : s->layer[3];
    wlr_scene_xdg_surface_create(ptree, popup->base);
}

static void new_xwayland(wl_listener *listener, void *data)
{
    WlServer *s = from_l(WlServer, listener, new_xwayland);
    auto *xwl = (wlr_xwayland_surface *)data;
    auto v = std::make_unique<WlView>();
    v->wm = s->wm;
    v->xwl = xwl;
    WlSurf sf{};
    sf.id = s->alloc_id();
    sf.client = true;
    sf.xwl = xwl;
    sf.w = xwl->width ? xwl->width : 200;
    sf.h = xwl->height ? xwl->height : 150;
    sf.layer = xwl->override_redirect ? 3 : 1;
    v->win = sf.id;
    xwl->data = (void *)(uintptr_t)sf.id;
    s->surfs.push_back(std::make_unique<WlSurf>(sf));

    v->destroy.notify = view_destroy;
    wl_signal_add(&xwl->events.destroy, &v->destroy);
    v->associate.notify = xwl_associate;
    wl_signal_add(&xwl->events.associate, &v->associate);
    v->dissociate.notify = xwl_dissociate;
    wl_signal_add(&xwl->events.dissociate, &v->dissociate);
    v->request_configure.notify = xwl_configure;
    wl_signal_add(&xwl->events.request_configure, &v->request_configure);
    v->set_title.notify = view_title;
    wl_signal_add(&xwl->events.set_title, &v->set_title);
    v->request_move.notify = view_req_move;
    wl_signal_add(&xwl->events.request_move, &v->request_move);
    v->request_maximize.notify = view_req_max;
    wl_signal_add(&xwl->events.request_maximize, &v->request_maximize);
    v->request_minimize.notify = view_req_min;
    wl_signal_add(&xwl->events.request_minimize, &v->request_minimize);
    v->request_fullscreen.notify = view_req_fs;
    wl_signal_add(&xwl->events.request_fullscreen, &v->request_fullscreen);
    v->request_close.notify = view_req_min;
    // map/unmap attached on associate
    s->views.push_back(std::move(v));
}

static void xwayland_ready(wl_listener *listener, void *)
{
    WlServer *s = from_l(WlServer, listener, xwayland_ready);
    if (s->xwayland && s->xwayland->display_name)
        setenv("DISPLAY", s->xwayland->display_name, 1);
}

static void deco_request(wl_listener *listener, void *)
{
    WlDeco *d = from_l(WlDeco, listener, request_mode);
    wlr_xdg_toplevel_decoration_v1_set_mode(d->deco, WLR_XDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE);
}

static void deco_destroy(wl_listener *listener, void *)
{
    WlDeco *d = from_l(WlDeco, listener, destroy);
    WlServer *s = srv(d->wm);
    wl_list_remove(&d->request_mode.link);
    wl_list_remove(&d->destroy.link);
    s->decos.erase(std::remove_if(s->decos.begin(), s->decos.end(), [&](auto &p) { return p.get() == d; }),
                   s->decos.end());
}

static void new_decoration(wl_listener *listener, void *data)
{
    WlServer *s = from_l(WlServer, listener, new_decoration);
    auto *deco = (wlr_xdg_toplevel_decoration_v1 *)data;
    auto d = std::make_unique<WlDeco>();
    d->wm = s->wm;
    d->deco = deco;
    d->request_mode.notify = deco_request;
    wl_signal_add(&deco->events.request_mode, &d->request_mode);
    d->destroy.notify = deco_destroy;
    wl_signal_add(&deco->events.destroy, &d->destroy);
    wlr_xdg_toplevel_decoration_v1_set_mode(deco, WLR_XDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE);
    s->decos.push_back(std::move(d));
}

} // namespace

bool wl_backend_init(WM *wm)
{
    wlr_log_init(WLR_ERROR, nullptr);
    auto s = std::make_unique<WlServer>();
    s->wm = wm;
    s->display = wl_display_create();
    if (!s->display)
        return false;
    wl_event_loop *loop = wl_display_get_event_loop(s->display);
    s->backend = wlr_backend_autocreate(loop, &s->session);
    if (!s->backend) {
        std::fprintf(stderr, "chime: no Wayland backend (need a TTY, or nest under another compositor)\n");
        return false;
    }
    s->renderer = wlr_renderer_autocreate(s->backend);
    if (!s->renderer || !wlr_renderer_init_wl_display(s->renderer, s->display))
        return false;
    s->allocator = wlr_allocator_autocreate(s->backend, s->renderer);
    if (!s->allocator)
        return false;
    s->compositor = wlr_compositor_create(s->display, 5, s->renderer);
    if (!s->compositor)
        return false;
    wlr_subcompositor_create(s->display);
    wlr_data_device_manager_create(s->display);

    s->scene = wlr_scene_create();
    for (int i = 0; i < 4; i++)
        s->layer[i] = wlr_scene_tree_create(&s->scene->tree);
    s->layout = wlr_output_layout_create(s->display);
    s->scene_layout = wlr_scene_attach_output_layout(s->scene, s->layout);

    s->xdg_shell = wlr_xdg_shell_create(s->display, 3);
    s->new_xdg_toplevel.notify = new_xdg_toplevel;
    wl_signal_add(&s->xdg_shell->events.new_toplevel, &s->new_xdg_toplevel);
    s->new_xdg_popup.notify = new_xdg_popup;
    wl_signal_add(&s->xdg_shell->events.new_popup, &s->new_xdg_popup);

    s->deco_mgr = wlr_xdg_decoration_manager_v1_create(s->display);
    s->new_decoration.notify = new_decoration;
    wl_signal_add(&s->deco_mgr->events.new_toplevel_decoration, &s->new_decoration);

    s->cursor = wlr_cursor_create();
    wlr_cursor_attach_output_layout(s->cursor, s->layout);
    s->cursor_mgr = wlr_xcursor_manager_create(nullptr, 24);
    wlr_xcursor_manager_load(s->cursor_mgr, 1);
    s->cursor_motion.notify = cursor_motion;
    wl_signal_add(&s->cursor->events.motion, &s->cursor_motion);
    s->cursor_motion_abs.notify = cursor_motion_abs;
    wl_signal_add(&s->cursor->events.motion_absolute, &s->cursor_motion_abs);
    s->cursor_button.notify = cursor_button;
    wl_signal_add(&s->cursor->events.button, &s->cursor_button);
    s->cursor_axis.notify = cursor_axis;
    wl_signal_add(&s->cursor->events.axis, &s->cursor_axis);
    s->cursor_frame.notify = cursor_frame;
    wl_signal_add(&s->cursor->events.frame, &s->cursor_frame);

    s->seat = wlr_seat_create(s->display, "seat0");
    s->request_cursor.notify = request_cursor;
    wl_signal_add(&s->seat->events.request_set_cursor, &s->request_cursor);
    s->request_set_selection.notify = request_set_selection;
    wl_signal_add(&s->seat->events.request_set_selection, &s->request_set_selection);

    s->new_output.notify = new_output;
    wl_signal_add(&s->backend->events.new_output, &s->new_output);
    s->new_input.notify = new_input;
    wl_signal_add(&s->backend->events.new_input, &s->new_input);

    const char *sock = wl_display_add_socket_auto(s->display);
    if (!sock) {
        std::fprintf(stderr, "chime: could not create Wayland socket\n");
        return false;
    }
    setenv("WAYLAND_DISPLAY", sock, 1);
    unsetenv("DISPLAY");

    s->xwayland = wlr_xwayland_create(s->display, s->compositor, false);
    if (s->xwayland) {
        s->new_xwayland.notify = new_xwayland;
        wl_signal_add(&s->xwayland->events.new_surface, &s->new_xwayland);
        s->xwayland_ready.notify = xwayland_ready;
        wl_signal_add(&s->xwayland->events.ready, &s->xwayland_ready);
        wlr_xwayland_set_seat(s->xwayland, s->seat);
    }

    if (!wlr_backend_start(s->backend)) {
        std::fprintf(stderr, "chime: Wayland backend failed to start\n");
        return false;
    }
    wlr_cursor_set_xcursor(s->cursor, s->cursor_mgr, "left_ptr");
    std::fprintf(stderr, "chime: Wayland compositor on WAYLAND_DISPLAY=%s\n", sock);
    wm->wl = s.release();
    return true;
}

void wl_backend_finish(WM *wm)
{
    WlServer *s = srv(wm);
    if (!s)
        return;
    if (s->xwayland)
        wlr_xwayland_destroy(s->xwayland);
    if (s->display)
        wl_display_destroy_clients(s->display);
    if (s->display)
        wl_display_destroy(s->display);
    delete s;
    wm->wl = nullptr;
}

int wl_backend_fd(WM *wm)
{
    WlServer *s = srv(wm);
    return s && s->display ? wl_event_loop_get_fd(wl_display_get_event_loop(s->display)) : -1;
}

void wl_backend_drain(WM *wm)
{
    WlServer *s = srv(wm);
    if (!s || !s->display)
        return;
    wl_display_flush_clients(s->display);
    wl_event_loop_dispatch(wl_display_get_event_loop(s->display), 0);
}

void wl_backend_flush(WM *wm)
{
    WlServer *s = srv(wm);
    if (s && s->display)
        wl_display_flush_clients(s->display);
}

Window wl_mkwin(WM *wm, int x, int y, int w, int h, unsigned long bg, bool override, int layer)
{
    (void)override;
    WlServer *s = srv(wm);
    if (!s)
        return 0;
    auto sf = std::make_unique<WlSurf>();
    sf->id = s->alloc_id();
    sf->x = x;
    sf->y = y;
    sf->w = std::max(1, w);
    sf->h = std::max(1, h);
    sf->layer = layer < 0 ? 3 : std::clamp(layer, 0, 3);
    sf->bg = bg;
    sf->argb.assign((size_t)sf->w * (size_t)sf->h, 0xFF000000u | (uint32_t)(bg & 0xFFFFFFu));
    sf->tree = wlr_scene_tree_create(s->layer[sf->layer]);
    sf->sbuf = wlr_scene_buffer_create(sf->tree, nullptr);
    sf->tree->node.data = (void *)(uintptr_t)sf->id;
    wlr_scene_node_set_position(&sf->tree->node, x, y);
    wlr_scene_node_set_enabled(&sf->tree->node, false);
    surf_upload(s, sf.get());
    Window id = sf->id;
    s->surfs.push_back(std::move(sf));
    return id;
}

void wl_destroy(WM *wm, Window w)
{
    WlServer *s = srv(wm);
    if (!s)
        return;
    WlSurf *sf = s->find(w);
    if (!sf)
        return;
    if (sf->pb) {
        wlr_buffer_drop(&sf->pb->base);
        sf->pb = nullptr;
    }
    if (sf->tree)
        wlr_scene_node_destroy(&sf->tree->node);
    s->surfs.erase(std::remove_if(s->surfs.begin(), s->surfs.end(), [&](auto &p) { return p.get() == sf; }),
                   s->surfs.end());
}

void wl_map(WM *wm, Window w)
{
    WlServer *s = srv(wm);
    WlSurf *sf = s ? s->find(w) : nullptr;
    if (!sf)
        return;
    sf->mapped = true;
    if (sf->tree)
        wlr_scene_node_set_enabled(&sf->tree->node, true);
}

void wl_unmap(WM *wm, Window w)
{
    WlServer *s = srv(wm);
    WlSurf *sf = s ? s->find(w) : nullptr;
    if (!sf)
        return;
    sf->mapped = false;
    if (sf->tree)
        wlr_scene_node_set_enabled(&sf->tree->node, false);
}

void wl_raise(WM *wm, Window w)
{
    WlServer *s = srv(wm);
    WlSurf *sf = s ? s->find(w) : nullptr;
    if (sf && sf->tree)
        wlr_scene_node_raise_to_top(&sf->tree->node);
}

void wl_lower(WM *wm, Window w)
{
    WlServer *s = srv(wm);
    WlSurf *sf = s ? s->find(w) : nullptr;
    if (sf && sf->tree)
        wlr_scene_node_lower_to_bottom(&sf->tree->node);
}

void wl_move_resize(WM *wm, Window w, int x, int y, int wd, int ht)
{
    WlServer *s = srv(wm);
    WlSurf *sf = s ? s->find(w) : nullptr;
    if (!sf)
        return;
    sf->x = x;
    sf->y = y;
    if (sf->tree)
        wlr_scene_node_set_position(&sf->tree->node, x, y);
    if (!sf->client)
        surf_resize_buf(s, sf, std::max(1, wd), std::max(1, ht));
    else {
        sf->w = std::max(1, wd);
        sf->h = std::max(1, ht);
    }
}

void wl_reparent(WM *wm, Window child, Window parent, int x, int y)
{
    WlServer *s = srv(wm);
    if (!s)
        return;
    WlSurf *ch = s->find(child);
    WlSurf *pa = s->find(parent);
    if (!ch)
        return;
    wlr_scene_tree *dest = pa && pa->tree ? pa->tree : s->layer[ch->layer];
    wlr_scene_tree *src = ch->view_tree ? ch->view_tree : ch->tree;
    if (src && dest)
        wlr_scene_node_reparent(&src->node, dest);
    if (src)
        wlr_scene_node_set_position(&src->node, x, y);
    ch->x = x;
    ch->y = y;
    if (ch->xwl && ch->xwl->surface && !ch->view_tree) {
        ch->view_tree = wlr_scene_subsurface_tree_create(dest, ch->xwl->surface);
        ch->view_tree->node.data = (void *)(uintptr_t)ch->id;
        wlr_scene_node_set_position(&ch->view_tree->node, x, y);
    }
}

void wl_set_bg(WM *wm, Window w, unsigned long pix)
{
    WlServer *s = srv(wm);
    WlSurf *sf = s ? s->find(w) : nullptr;
    if (sf)
        sf->bg = pix;
}

void wl_set_cursor(WM *wm, Cursor c)
{
    WlServer *s = srv(wm);
    if (!s || !s->cursor)
        return;
    s->last_cursor = c;
    wlr_cursor_set_xcursor(s->cursor, s->cursor_mgr, cursor_name(c));
}

uint32_t *wl_pixels(WM *wm, Drawable d, int *w, int *h)
{
    WlServer *s = srv(wm);
    WlSurf *sf = s ? s->find(d) : nullptr;
    if (!sf || sf->client)
        return nullptr;
    if (sf->argb.empty())
        sf->argb.assign((size_t)std::max(1, sf->w) * (size_t)std::max(1, sf->h), 0xFF000000u);
    if (w)
        *w = sf->w;
    if (h)
        *h = sf->h;
    return sf->argb.data();
}

void wl_damage(WM *wm, Drawable d)
{
    WlServer *s = srv(wm);
    WlSurf *sf = s ? s->find(d) : nullptr;
    if (sf)
        surf_upload(s, sf);
}

void wl_ensure_size(WM *wm, Drawable d, int w, int h)
{
    WlServer *s = srv(wm);
    WlSurf *sf = s ? s->find(d) : nullptr;
    if (sf)
        surf_resize_buf(s, sf, std::max(1, w), std::max(1, h));
}

Pixmap wl_pix_create(WM *wm, int w, int h)
{
    WlServer *s = srv(wm);
    if (!s)
        return 0;
    auto sf = std::make_unique<WlSurf>();
    sf->id = s->alloc_id();
    sf->pixmap = true;
    sf->w = std::max(1, w);
    sf->h = std::max(1, h);
    sf->argb.assign((size_t)sf->w * (size_t)sf->h, 0xFF000000u);
    Window id = sf->id;
    s->surfs.push_back(std::move(sf));
    return id;
}

void wl_pix_free(WM *wm, Pixmap p)
{
    wl_destroy(wm, p);
}

bool wl_ptr_xy(WM *wm, int *x, int *y)
{
    WlServer *s = srv(wm);
    if (!s || !s->cursor)
        return false;
    if (x)
        *x = (int)s->cursor->x;
    if (y)
        *y = (int)s->cursor->y;
    return true;
}

void wl_ptr_grab(WM *wm, Window w)
{
    WlServer *s = srv(wm);
    if (s)
        s->grab_w = w;
}

void wl_ptr_ungrab(WM *wm)
{
    WlServer *s = srv(wm);
    if (s)
        s->grab_w = 0;
}

void wl_kbd_grab(WM *wm, Window w)
{
    wm->kbd_grab_w = w;
    WlServer *s = srv(wm);
    if (s && w == 0)
        focus_surface(s, nullptr);
}

void wl_focus_client(WM *wm, Window w)
{
    WlServer *s = srv(wm);
    WlView *v = s ? view_by_win(s, w) : nullptr;
    if (!v)
        return;
    wlr_surface *surf = nullptr;
    if (v->xdg)
        surf = v->xdg->base->surface;
    else if (v->xwl)
        surf = v->xwl->surface;
    if (v->xdg)
        wlr_xdg_toplevel_set_activated(v->xdg, true);
    if (v->xwl)
        wlr_xwayland_surface_activate(v->xwl, true);
    focus_surface(s, surf);
}

void wl_close_client(WM *wm, Window w)
{
    WlView *v = srv(wm) ? view_by_win(srv(wm), w) : nullptr;
    if (!v)
        return;
    if (v->xdg)
        wlr_xdg_toplevel_send_close(v->xdg);
    else if (v->xwl)
        wlr_xwayland_surface_close(v->xwl);
}

void wl_kill_client(WM *wm, Window w)
{
    wl_close_client(wm, w);
}

void wl_configure_client(WM *wm, Client *c)
{
    if (!c)
        return;
    WlView *v = srv(wm) ? view_by_win(srv(wm), c->win) : nullptr;
    int cw = std::max(1, c->w - 2 * w95::kFrameB);
    int ch = std::max(1, c->h - 2 * w95::kFrameB - w95::kTitleH);
    if (v && v->xdg)
        wlr_xdg_toplevel_set_size(v->xdg, cw, ch);
    if (v && v->xwl)
        wlr_xwayland_surface_configure(v->xwl, (int16_t)(c->x + w95::kFrameB),
                                       (int16_t)(c->y + w95::kFrameB + w95::kTitleH), (uint16_t)cw, (uint16_t)ch);
    if (WlServer *s = srv(wm)) {
        if (WlSurf *sf = s->find(c->win)) {
            if (sf->view_tree)
                wlr_scene_node_set_position(&sf->view_tree->node, w95::kFrameB, w95::kFrameB + w95::kTitleH);
        }
    }
}

std::string wl_client_name(WM *wm, Window w)
{
    WlView *v = srv(wm) ? view_by_win(srv(wm), w) : nullptr;
    if (v && v->xdg && v->xdg->title && v->xdg->title[0])
        return v->xdg->title;
    if (v && v->xwl && v->xwl->title && v->xwl->title[0])
        return v->xwl->title;
    return "Untitled";
}

std::vector<Monitor> wl_query_mons(WM *wm)
{
    std::vector<Monitor> out;
    WlServer *s = srv(wm);
    if (!s)
        return out;
    bool first = true;
    for (auto &o : s->outputs) {
        wlr_box box{};
        wlr_output_layout_get_box(s->layout, o->output, &box);
        if (box.width < 1 || box.height < 1)
            continue;
        Monitor m;
        m.x = box.x;
        m.y = box.y;
        m.w = box.width;
        m.h = box.height;
        m.primary = first;
        first = false;
        out.push_back(m);
    }
    if (out.empty()) {
        Monitor m;
        m.w = s->wm->sw > 0 ? s->wm->sw : 1280;
        m.h = s->wm->sh > 0 ? s->wm->sh : 800;
        m.primary = true;
        out.push_back(m);
    }
    return out;
}

void wl_refresh_heads(WM *wm)
{
    wm->heads.clear();
    WlServer *s = srv(wm);
    if (!s)
        return;
    int i = 0;
    for (auto &o : s->outputs) {
        wlr_box box{};
        wlr_output_layout_get_box(s->layout, o->output, &box);
        Head h;
        h.name = o->output->name ? o->output->name : "WL";
        h.output = (RROutput)(i + 1);
        h.x = box.x;
        h.y = box.y;
        h.w = box.width;
        h.h = box.height;
        h.primary = (i == 0);
        wlr_output_mode *m;
        wl_list_for_each(m, &o->output->modes, link)
        {
            bool seen = false;
            for (auto &hm : h.modes)
                if (hm.w == m->width && hm.h == m->height)
                    seen = true;
            if (!seen)
                h.modes.push_back({m->width, m->height, (RRMode)h.modes.size() + 1});
        }
        if (h.modes.empty())
            h.modes.push_back({h.w, h.h, 1});
        for (int k = 0; k < (int)h.modes.size(); k++)
            if (h.modes[k].w == h.w && h.modes[k].h == h.h)
                h.mode_i = k;
        wm->heads.push_back(std::move(h));
        i++;
    }
}

bool wl_apply_heads(WM *wm)
{
    WlServer *s = srv(wm);
    if (!s || wm->heads.empty())
        return false;
    for (auto &h : wm->heads) {
        int idx = (int)h.output - 1;
        if (idx < 0 || idx >= (int)s->outputs.size())
            continue;
        wlr_output *out = s->outputs[idx]->output;
        if (h.mode_i >= 0 && h.mode_i < (int)h.modes.size()) {
            h.w = h.modes[h.mode_i].w;
            h.h = h.modes[h.mode_i].h;
        }
        wlr_output_state st;
        wlr_output_state_init(&st);
        wlr_output_state_set_enabled(&st, true);
        bool set = false;
        wlr_output_mode *m;
        wl_list_for_each(m, &out->modes, link)
        {
            if (m->width == h.w && m->height == h.h) {
                wlr_output_state_set_mode(&st, m);
                set = true;
                break;
            }
        }
        if (!set)
            wlr_output_state_set_custom_mode(&st, h.w, h.h, 0);
        wlr_output_commit_state(out, &st);
        wlr_output_state_finish(&st);
        wlr_output_layout_add(s->layout, out, h.x, h.y);
    }
    wlr_box box{};
    wlr_output_layout_get_box(s->layout, nullptr, &box);
    wm->sw = box.width;
    wm->sh = box.height;
    wm->sync_monitors();
    return true;
}

void wl_refresh_modes(WM *wm)
{
    wm->modes.clear();
    auto add = [&](int w, int h) {
        if (w < 320 || h < 200)
            return;
        for (auto &m : wm->modes)
            if (m.w == w && m.h == h)
                return;
        wm->modes.push_back({w, h});
    };
    WlServer *s = srv(wm);
    if (s) {
        for (auto &o : s->outputs) {
            wlr_output_mode *m;
            wl_list_for_each(m, &o->output->modes, link)
                add(m->width, m->height);
        }
    }
    add(wm->sw, wm->sh);
    wm->mode_i = 0;
    for (int i = 0; i < (int)wm->modes.size(); i++)
        if (wm->modes[i].w == wm->sw && wm->modes[i].h == wm->sh)
            wm->mode_i = i;
}

bool wl_apply_output_mode(WM *wm, int w, int h)
{
    if (wm->heads.empty())
        wl_refresh_heads(wm);
    if (wm->heads.empty())
        return false;
    for (auto &hm : wm->heads[0].modes)
        if (hm.w == w && hm.h == h) {
            wm->heads[0].w = w;
            wm->heads[0].h = h;
        }
    wm->heads[0].w = w;
    wm->heads[0].h = h;
    return wl_apply_heads(wm);
}

int wl_layout_w(WM *wm)
{
    WlServer *s = srv(wm);
    if (!s)
        return 0;
    wlr_box box{};
    wlr_output_layout_get_box(s->layout, nullptr, &box);
    return box.width;
}

int wl_layout_h(WM *wm)
{
    WlServer *s = srv(wm);
    if (!s)
        return 0;
    wlr_box box{};
    wlr_output_layout_get_box(s->layout, nullptr, &box);
    return box.height;
}

int wl_win_w(WM *wm, Window w)
{
    WlSurf *sf = srv(wm) ? srv(wm)->find(w) : nullptr;
    return sf ? sf->w : 0;
}

int wl_win_h(WM *wm, Window w)
{
    WlSurf *sf = srv(wm) ? srv(wm)->find(w) : nullptr;
    return sf ? sf->h : 0;
}

bool wl_win_mapped(WM *wm, Window w)
{
    WlSurf *sf = srv(wm) ? srv(wm)->find(w) : nullptr;
    return sf && sf->mapped;
}

bool wl_win_xy(WM *wm, Window w, int *x, int *y)
{
    WlSurf *sf = srv(wm) ? srv(wm)->find(w) : nullptr;
    if (!sf)
        return false;
    if (x)
        *x = sf->x;
    if (y)
        *y = sf->y;
    return true;
}

#endif // CHIME_WAYLAND
