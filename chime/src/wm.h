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
#include <cstdint>

// Allocated X colormap pixel. Named Col so drawing code can write face.pix
// without colliding with X's Pixel typedef.
struct Col {
    unsigned long pix;
};

// Edge/corner snap while dragging a window. Top is "maximize to the work
// area"; the rest tile to half or quarter of the monitor (taskbar excluded).
enum class Snap { Off, Left, Right, Top, TL, TR, BL, BR };

// One top-level client wrapped in a Chime frame. Geometry (x,y,w,h) is the
// outer frame in root coordinates. The real application window lives as a
// child, inset by kFrameB / kTitleH.
struct Client {
    Window frame = 0;   // Our decorated parent
    Window win = 0;     // The application's top-level window
    int x = 0, y = 0, w = 0, h = 0;
    int rx = 0, ry = 0, rw = 0, rh = 0; // Last floating size, for un-maximize / un-snap
    int last_mon = 0;   // Monitor index; kept while iconic so the task button stays put
    bool maxed = false; // Snap::Top (fills work area)
    bool tiled = false; // Any snap other than Top
    Snap snap = Snap::Off;
    bool iconic = false;
    int ignore_unmap = 0; // Unmaps we issued ourselves (minimize) must not unmanage
    std::string name;     // WM_NAME / _NET_WM_NAME for caption and task button
};

// One physical output. Each monitor owns a wallpaper window, a taskbar, and
// the Start menu / Programs submenu (created even when closed; we map/unmap).
struct Monitor {
    int x = 0, y = 0, w = 0, h = 0;
    bool primary = false;
    Window desktop = 0;
    Window taskbar = 0;
    Window startmenu = 0;
    Window submenu = 0;
    int hover = -1;     // Start-menu row under the pointer, or -1
    int subhover = -1;  // Programs submenu row
    bool start_open = false;
    bool sub_open = false;
};

// Hit-test result for a click/motion on a frame. Edges include a 12px corner
// grab so NW/NE/SW/SE win over the straight edges.
enum class Hit {
    Miss,
    Title,
    Sys,    // Fake system-menu box on the left of the caption
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

enum class DragMode { Off, Move, Resize, Select };

// One Start-menu row. sep draws a groove instead of a label; sub draws a
// triangle and opens the Programs flyout. Find is present but disabled.
struct MenuItem {
    const char *label;
    bool sep;
    bool sub;
    bool enabled;
};

extern MenuItem kMenu[];
extern const int kMenuN;
int menu_height();

// Wallpaper entry: either a built-in pattern (pattern 0 = solid desktop color)
// or a P6 PPM/BMP path scanned from /opt/chime/wallpapers and ~/.chime/wallpapers.
struct WallChoice {
    std::string name;
    int pattern = 0;
    std::string file;
};

// Runtime color scheme. Built-ins are copied from w95::kSchemes; user schemes
// live in ~/.chime/schemes and can be edited from Display Properties.
struct ColorScheme {
    std::string name;
    w95::Rgb desktop{}, face{}, hi{}, lo{}, dk{}, title{}, title_in{}, text{}, field{}, banner{};
    bool builtin = false;
};

struct FileEnt {
    std::string name;
    bool dir = false;
};

struct Raster {
    int w = 0, h = 0;
    std::vector<std::uint8_t> rgb;
};

struct VideoMode {
    int w = 0, h = 0;
};

// The window manager. One instance lives for the whole session (see main.cpp).
// Drawing lives in draw.cpp; event handling, clients, tray, and monitors here.
struct WM {
    Display *dpy = nullptr;
    int screen = 0;
    Window root = 0;
    Visual *vis = nullptr;
    Colormap cmap = 0;
    int depth = 0;
    int sw = 0, sh = 0; // Root size; refreshed on RandR
    GC gc = 0;
    XFontStruct *font = nullptr;
    XFontStruct *font_b = nullptr;

    // Scheme pixels. white/red/green/blue/yellow are for desktop icons,
    // not the window text color (that is fg).
    Col desktop, face, hi, lo, dk, title, title_in, fg, white, red, green, blue, yellow, field, banner;

    Cursor cur_left = 0, cur_move = 0, cur_n = 0, cur_s = 0, cur_e = 0, cur_w = 0;
    Cursor cur_ne = 0, cur_nw = 0, cur_se = 0, cur_sw = 0;

    // ICCCM + EWMH + system tray atoms interned once in intern_atoms().
    Atom utf8 = 0, wm_protocols = 0, wm_delete = 0, wm_state = 0, wm_take_focus = 0;
    Atom net_supported = 0, net_wm_name = 0, net_wm_state = 0, net_active = 0;
    Atom net_client_list = 0, net_supporting = 0, net_wm_window_type = 0;
    Atom net_type_desktop = 0, net_type_dock = 0, net_workarea = 0;
    Atom net_number_desktops = 0, net_current_desktop = 0, net_desktop_geometry = 0;
    Atom net_wm_state_hidden = 0, net_wm_state_maxv = 0, net_wm_state_maxh = 0;
    Atom net_active_window = 0, motif_hints = 0;
    Atom net_system_tray = 0, net_system_tray_opcode = 0, net_system_tray_orientation = 0;
    Atom net_system_tray_visual = 0, xembed = 0, xembed_info = 0, manager = 0;

    Window checkwin = 0; // 1x1 _NET_SUPPORTING_WM_CHECK window named "chime"
    Window rundlg = 0, shutdlg = 0, setdlg = 0, snapwin = 0, traywin = 0;
    Window colordlg = 0, filedlg = 0;
    bool run_open = false, shut_open = false, set_open = false;
    bool color_open = false, file_open = false;
    int dialog_mon = 0;
    std::string run_text;
    int run_cursor = 0;
    bool caret_on = true;
    long caret_ms = 0;
    std::vector<Window> tray_icons;
    unsigned super_mask = 0; // Actual modifier bit for Super (often Mod4)
    bool super_held = false;
    bool super_chord = false; // Super was used with another key; don't open Start on release
    bool volicon_launched = false;

    int scheme_i = 0;
    int wall_i = 0;
    int set_save_scheme = 0; // Snapshot when Display Properties opens (Cancel reverts)
    int set_save_wall = 0;
    int wall_scroll = 0;
    int scheme_scroll = 0;
    int mode_scroll = 0;
    int set_list = 1; // 0 wallpaper, 1 scheme, 2 resolution (arrow keys)
    int mode_i = 0;
    int set_save_mode = 0;
    std::vector<VideoMode> modes;
    std::string mode_note;
    std::vector<ColorScheme> schemes;
    std::vector<ColorScheme> set_save_schemes;
    std::vector<WallChoice> walls;
    Pixmap wall_tile = 0;
    int wall_tw = 0, wall_th = 0;

    int color_role = 0;     // Which scheme slot the color editor is changing
    int color_chan = 0;     // 0=R 1=G 2=B for Left/Right
    bool color_name_edit = false;
    std::string color_name_buf;

    std::string file_dir;
    std::string file_typed;
    int file_sel = 0;
    int file_scroll = 0;
    std::vector<FileEnt> file_ents;

    std::vector<Monitor> mons;
    std::vector<std::unique_ptr<Client>> clients;
    Client *focused = nullptr;

    DragMode drag = DragMode::Off;
    Client *drag_c = nullptr;
    int drag_dir = 0; // Bitmask: 1=N 2=S 4=E 8=W
    int drag_ox = 0, drag_oy = 0;
    int drag_fx = 0, drag_fy = 0, drag_fw = 0, drag_fh = 0;

    Time last_title_time = 0;
    Window last_title_win = 0;
    Time last_icon_time = 0;
    int last_icon = -1;
    int selected_icon = -1; // Keyboard focus among the four desktop shortcuts
    unsigned desk_mask = 0; // Bit i set → shortcut i is in the selection
    int sel_x0 = 0, sel_y0 = 0, sel_x1 = 0, sel_y1 = 0;
    int sel_mon = 0;
    unsigned sel_base = 0; // desk_mask at marquee start (Ctrl adds to this)

    int rr_event = 0, rr_error = 0;
    bool have_randr = false;
    bool running = true;
    int cascade[16] = {}; // Per-monitor stagger for newly mapped windows

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
    void draw_colordlg();
    void draw_filedlg();
    void draw_dlg_btn(Drawable d, int x, int y, int w, int h, const char *lab);
    void draw_listbox(Drawable d, int x, int y, int w, int h, int sel, int n, const char *(*label)(int));
    void tile_wall(Drawable d, int x, int y, int w, int h);
    void draw_dotted_rect(Drawable d, int x0, int y0, int x1, int y1);

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
    void load_schemes();
    void save_schemes();
    void init_schemes();
    std::string unique_scheme_name(const std::string &base);
    w95::Rgb &scheme_role(ColorScheme &s, int role);
    void open_colordlg();
    void close_colordlg(bool apply);
    void open_filedlg();
    void close_filedlg();
    void load_file_dir(const std::string &path);
    bool import_wallpaper(const std::string &path);
    unsigned long pixel_from_rgb(std::uint8_t r, std::uint8_t g, std::uint8_t b);
    void blit_rgb(Pixmap dst, const std::uint8_t *rgb, int w, int h);
    void grab_dialog(Window w);
    void ungrab_if_idle();
    Monitor *open_start_mon();
    void position_submenu(Monitor &m);
    void set_start_hover(Monitor &m, int idx, bool open_sub);
    void sync_start_pointer(Monitor &m);
    int menu_step(int from, int dir);
    void menu_key(KeySym ks);
    void desktop_key(KeySym ks, unsigned state);
    void settings_key(KeySym ks, const char *buf, int n);
    void color_key(KeySym ks, const char *buf, int n);
    void file_key(KeySym ks, const char *buf, int n);
    void activate_start_item(Monitor &m, int idx);
    void activate_sub_item(int i);
    void activate_desk_icon(int i);
    void ensure_list_scroll(int &scroll, int sel, int vis, int n);
    int pointer_mon();
    void refresh_modes();
    bool apply_mode(int i);
    void select_desk_icons(int x0, int y0, int x1, int y1, unsigned base);
    void desk_select_only(int i);
    void activate_desk_sel();

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
