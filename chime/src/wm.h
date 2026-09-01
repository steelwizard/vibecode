#pragma once

#include "theme.h"

#include <X11/Xlib.h>
#include <X11/Xatom.h>
#include <X11/Xutil.h>
#include <X11/cursorfont.h>
#include <X11/keysym.h>
#include <X11/extensions/Xrandr.h>
#include <X11/extensions/Xinerama.h>

#include <string>
#include <vector>
#include <memory>

struct Col {
    unsigned long pix;
};

enum class Snap { Off, Left, Right, Top, TL, TR, BL, BR };

struct Client {
    Window frame = 0;
    Window win = 0;
    int x = 0, y = 0, w = 0, h = 0;
    int rx = 0, ry = 0, rw = 0, rh = 0;
    int last_mon = 0;
    bool maxed = false;
    bool tiled = false;
    Snap snap = Snap::Off;
    bool iconic = false;
    int ignore_unmap = 0;
    std::string name;
};

struct Monitor {
    int x = 0, y = 0, w = 0, h = 0;
    bool primary = false;
    Window desktop = 0;
    Window taskbar = 0;
    Window startmenu = 0;
    Window submenu = 0;
    int hover = -1;
    int subhover = -1;
    bool start_open = false;
    bool sub_open = false;
};

enum class Hit {
    Miss,
    Title,
    Sys,
    Min,
    Max,
    Close,
    EdgeN,
    EdgeS,
    EdgeE,
    EdgeW,
    EdgeNE,
    EdgeNW,
    EdgeSE,
    EdgeSW
};

enum class DragMode { Off, Move, Resize };

struct MenuItem {
    const char *label;
    bool sep;
    bool sub;
    bool enabled;
};

extern MenuItem kMenu[];
extern const int kMenuN;
int menu_height();

struct WallChoice {
    std::string name;
    int pattern = 0;
    std::string file;
};

struct WM {
    Display *dpy = nullptr;
    int screen = 0;
    Window root = 0;
    Visual *vis = nullptr;
    Colormap cmap = 0;
    int depth = 0;
    int sw = 0, sh = 0;
    GC gc = 0;
    XFontStruct *font = nullptr;
    XFontStruct *font_b = nullptr;

    Col desktop, face, hi, lo, dk, title, title_in, fg, white, red, green, blue, yellow, field, banner;

    Cursor cur_left = 0, cur_move = 0, cur_n = 0, cur_s = 0, cur_e = 0, cur_w = 0;
    Cursor cur_ne = 0, cur_nw = 0, cur_se = 0, cur_sw = 0;

    Atom utf8 = 0, wm_protocols = 0, wm_delete = 0, wm_state = 0, wm_take_focus = 0;
    Atom net_supported = 0, net_wm_name = 0, net_wm_state = 0, net_active = 0;
    Atom net_client_list = 0, net_supporting = 0, net_wm_window_type = 0;
    Atom net_type_desktop = 0, net_type_dock = 0, net_workarea = 0;
    Atom net_number_desktops = 0, net_current_desktop = 0, net_desktop_geometry = 0;
    Atom net_wm_state_hidden = 0, net_wm_state_maxv = 0, net_wm_state_maxh = 0;
    Atom net_active_window = 0, motif_hints = 0;
    Atom net_system_tray = 0, net_system_tray_opcode = 0, net_system_tray_orientation = 0;
    Atom net_system_tray_visual = 0, xembed = 0, xembed_info = 0, manager = 0;

    Window checkwin = 0;
    Window rundlg = 0, shutdlg = 0, setdlg = 0, snapwin = 0, traywin = 0;
    bool run_open = false, shut_open = false, set_open = false;
    int dialog_mon = 0;
    std::string run_text;
    int run_cursor = 0;
    std::vector<Window> tray_icons;
    unsigned super_mask = 0;
    bool super_held = false;
    bool super_chord = false;
    bool volicon_launched = false;

    int scheme_i = 0;
    int wall_i = 0;
    int set_save_scheme = 0;
    int set_save_wall = 0;
    std::vector<WallChoice> walls;
    Pixmap wall_tile = 0;
    int wall_tw = 0, wall_th = 0;

    std::vector<Monitor> mons;
    std::vector<std::unique_ptr<Client>> clients;
    Client *focused = nullptr;

    DragMode drag = DragMode::Off;
    Client *drag_c = nullptr;
    int drag_dir = 0;
    int drag_ox = 0, drag_oy = 0;
    int drag_fx = 0, drag_fy = 0, drag_fw = 0, drag_fh = 0;

    Time last_title_time = 0;
    Window last_title_win = 0;
    Time last_icon_time = 0;
    int last_icon = -1;
    int selected_icon = -1;

    int rr_event = 0, rr_error = 0;
    bool have_randr = false;
    bool running = true;
    int cascade[16] = {};

    bool init();
    void run();
    void finish();

    unsigned long alloc_rgb(w95::Rgb c);
    Window mkwin(int x, int y, int w, int h, unsigned long bg, long mask, bool override);
    void intern_atoms();
    void ewmh_init();
    void ewmh_update();
    void launch(const char *cmd);
    std::string get_name(Window w);
    bool has_proto(Window w, Atom a);
    void send_client_message(Window w, Atom type, long a, long b = 0, long c = 0);

    std::vector<Monitor> query_monitors();
    void sync_monitors();
    void create_shell(Monitor &m);
    void destroy_shell(Monitor &m);
    void restack_shell();
    int monitor_at(int x, int y);
    int monitor_for(Client *c);
    Monitor *mon_by_window(Window w);
    int mon_index(const Monitor *m);
    Monitor *primary_mon();

    bool is_internal(Window w);
    Client *find_client(Window w);
    Client *find_frame(Window w);
    std::vector<Client *> clients_on(int mi);

    void manage(Window w);
    void unmanage(Client *c, bool destroyed);
    void focus(Client *c);
    void raise_client(Client *c);
    void close_client(Client *c);
    void minimize(Client *c);
    void restore(Client *c);
    void maximize_toggle(Client *c);
    void remember_float(Client *c);
    void float_for_drag(Client *c, int px, int py);
    Snap snap_at(int px, int py);
    void snap_rect(Snap s, int px, int py, int &x, int &y, int &w, int &h);
    void apply_snap(Client *c, Snap s, int px, int py);
    void show_snap_preview(Snap s, int px, int py);
    void hide_snap_preview();
    void draw_snap_preview();
    void set_wm_state(Client *c, long state);
    void apply_geom(Client *c);
    void send_configure(Client *c);

    void fill(Drawable d, int x, int y, int w, int h, unsigned long p);
    void bevel(Drawable d, int x, int y, int w, int h, bool raised);
    void sunken(Drawable d, int x, int y, int w, int h);
    int text_w(const char *s, bool bold = false);
    void draw_str(Drawable d, int x, int y, int box_h, const char *s, unsigned long pix, bool bold = false);
    void draw_str_clip(Drawable d, int x, int y, int w, int h, const char *s, unsigned long pix, bool bold = false);
    void draw_flag(Drawable d, int x, int y);
    void draw_icon(Drawable d, int x, int y, int kind, bool selected);
    void draw_frame(Client *c);
    void draw_caption_btn(Drawable d, int x, int y, int kind, bool maxed);
    void draw_desktop(Monitor &m);
    void draw_taskbar(Monitor &m);
    void draw_clock(Drawable d, int x, int y, int w, int h);
    void draw_tray();
    void draw_startmenu(Monitor &m);
    void draw_submenu(Monitor &m);
    void draw_rundlg();
    void draw_shutdlg();
    void draw_setdlg();
    void draw_dlg_btn(Drawable d, int x, int y, int w, int h, const char *lab);
    void draw_listbox(Drawable d, int x, int y, int w, int h, int sel, int n, const char *(*label)(int));
    void tile_wall(Drawable d, int x, int y, int w, int h);

    int tray_width(const Monitor &m);
    void tray_create();
    void tray_stash();
    void tray_claim();
    void tray_layout();
    void tray_dock(Window w);
    void tray_undock(Window w);
    bool is_tray_icon(Window w);
    void tray_send_xembed(Window w, long msg, long detail, long d1, long d2);

    unsigned mod_mask_for(KeySym ks);
    void grab_key(KeySym ks, unsigned mod);
    void close_run();
    void close_settings(bool revert);
    void open_settings(int mi);
    void rebuild_walls();
    void make_wall_tile();
    void apply_scheme(int i);
    void apply_wall(int i);
    void refresh_chrome();
    void save_display();
    void load_display();
    int pointer_mon();

    Hit hit_frame(Client *c, int x, int y);
    Cursor cursor_for(Hit h);
    void close_menus();
    void open_start(Monitor &m);
    void open_run(int mi);
    void open_shut(int mi);
    void handle_event(XEvent *e);
    void on_button_press(XEvent *e);
    void on_button_release(XEvent *e);
    void on_motion(XEvent *e);
    void on_key(XEvent *e);
    void on_expose(XEvent *e);
};
