#pragma once

// Wayland/wlroots backend. Built only with -DCHIME_WAYLAND.
// Window IDs are allocated here; chrome is software-rendered into scene
// buffers. Application windows are xdg-shell or Xwayland surfaces.

#include "wm.h"

#ifdef CHIME_WAYLAND

bool wl_backend_init(WM *wm);
void wl_backend_finish(WM *wm);
int wl_backend_fd(WM *wm);
void wl_backend_drain(WM *wm);
void wl_backend_flush(WM *wm);

Window wl_mkwin(WM *wm, int x, int y, int w, int h, unsigned long bg, bool override, int layer);
void wl_destroy(WM *wm, Window w);
void wl_map(WM *wm, Window w);
void wl_unmap(WM *wm, Window w);
void wl_raise(WM *wm, Window w);
void wl_lower(WM *wm, Window w);
void wl_move_resize(WM *wm, Window w, int x, int y, int wd, int ht);
void wl_reparent(WM *wm, Window child, Window parent, int x, int y);
void wl_set_bg(WM *wm, Window w, unsigned long pix);
void wl_set_cursor(WM *wm, Cursor c);

uint32_t *wl_pixels(WM *wm, Drawable d, int *w, int *h);
void wl_damage(WM *wm, Drawable d);
void wl_ensure_size(WM *wm, Drawable d, int w, int h);

Pixmap wl_pix_create(WM *wm, int w, int h);
void wl_pix_free(WM *wm, Pixmap p);

bool wl_ptr_xy(WM *wm, int *x, int *y);
void wl_ptr_grab(WM *wm, Window w);
void wl_ptr_ungrab(WM *wm);
void wl_kbd_grab(WM *wm, Window w);
void wl_focus_client(WM *wm, Window w);
void wl_close_client(WM *wm, Window w);
void wl_kill_client(WM *wm, Window w);
void wl_configure_client(WM *wm, Client *c);
std::string wl_client_name(WM *wm, Window w);

std::vector<Monitor> wl_query_mons(WM *wm);
void wl_refresh_heads(WM *wm);
bool wl_apply_heads(WM *wm);
void wl_refresh_modes(WM *wm);
bool wl_apply_output_mode(WM *wm, int w, int h);
int wl_layout_w(WM *wm);
int wl_layout_h(WM *wm);

int wl_win_w(WM *wm, Window w);
int wl_win_h(WM *wm, Window w);
bool wl_win_mapped(WM *wm, Window w);
bool wl_win_xy(WM *wm, Window w, int *x, int *y);

#endif
