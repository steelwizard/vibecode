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
#include <ctime>

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
// wall_pix / desk_pix cache the wallpaper (and primary-only icons) so marquee
// select can blit instead of retile the pattern on every motion event.
struct Monitor {
    int x = 0, y = 0, w = 0, h = 0;
    bool primary = false;
    Window desktop = 0;
    Window taskbar = 0;
    Window startmenu = 0;
    Window submenu = 0;
    Pixmap wall_pix = 0;
    Pixmap desk_pix = 0;
    int pix_w = 0, pix_h = 0;
    bool wall_dirty = true;
    bool desk_dirty = true;
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

enum class DragMode { Off, Move, Resize, Select, Icons };

// Dialog / chrome push-buttons. Armed on press, fired on release if still inside.
enum {
    PB_NONE = 0,
    PB_RUN_OK,
    PB_RUN_CANCEL,
    PB_RUN_X,
    PB_SHUT_YES,
    PB_SHUT_NO,
    PB_SHUT_X,
    PB_COLOR_OK,
    PB_COLOR_CANCEL,
    PB_COLOR_X,
    PB_FILE_OK,
    PB_FILE_CANCEL,
    PB_FILE_X,
    PB_FILE_UP,
    PB_FILE_HOME,
    PB_SET_OK,
    PB_SET_CANCEL,
    PB_SET_APPLY,
    PB_SET_X,
    PB_SET_BROWSE,
    PB_SET_NEW,
    PB_SET_EDIT,
    PB_SET_DEL,
    PB_SET_IDENT
};

// One Start-menu row. sep draws a groove instead of a label; sub draws a
// triangle and opens the Programs flyout. Find is present but disabled.
struct MenuItem {
    const char *label;
    bool sep;
    bool sub;
    bool enabled;
};

// A desktop icon or Start → Programs row, loaded from XDG dirs (.desktop
// files, plus ordinary files/folders on the Desktop).
struct LaunchItem {
    std::string name;
    std::string exec; // Already expanded; passed to launch() / sh -c
    std::string path; // Source file, directory, or .desktop path
    int kind = 3;     // draw_icon: 0 computer, 1 folder, 2 terminal, 3 document
    bool dir = false;
    int x = -1, y = -1; // Desktop position; -1 until auto-placed or restored
};

// One docked system-tray client (XEmbed) or a StatusNotifier window we own.
struct TrayItem {
    Window win = 0;   // Client icon, or the 20x20 window we paint for SNI
    Window sock = 0;  // Root-level wrapper matching the client's visual/depth
    Colormap cmap = 0;
    bool own_cmap = false;
    bool own_win = false; // We created win (SNI); destroy it on undock
    bool mapped = true;
    std::vector<std::uint8_t> rgb; // own_win: kTrayIcon^2 RGB triples
};

extern MenuItem kMenu[];
extern const int kMenuN;
int menu_height();

// Wallpaper: a 32x32 pattern (pattern 0 = solid desktop color) or a photo
// scanned from /opt/chime/wallpapers and ~/.chime/wallpapers.
struct WallChoice {
    std::string name;
    int pattern = 0;
    std::string file;
};

enum class PicPos { Tile, Center, Stretch };

struct HeadMode {
    int w = 0, h = 0;
    RRMode id = 0;
};

// One connected RandR output, used by the Settings tab to arrange monitors.
struct Head {
    std::string name;
    RROutput output = 0;
    RRCrtc crtc = 0;
    RRMode mode = 0;
    int x = 0, y = 0, w = 0, h = 0;
    bool primary = false;
    std::vector<HeadMode> modes;
    int mode_i = 0;
};

struct PixCache {
    int w = 0, h = 0;
    Pixmap pix = 0;
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
    int shut_choice = 0; // 0 shut down, 1 restart, 2 close Chime (log off)
    bool color_open = false, file_open = false;
    int dialog_mon = 0;
    std::string run_text;
    int run_cursor = 0;
    bool caret_on = true;
    long caret_ms = 0;
    std::vector<TrayItem> tray_items;
    void *sni = nullptr; // SniHost in sni.cpp; null if D-Bus is unavailable
    unsigned super_mask = 0; // Actual modifier bit for Super (often Mod4)
    bool super_held = false;
    bool super_chord = false; // Super was used with another key; don't open Start on release
    bool volicon_launched = false;

    int scheme_i = 0;
    int pat_i = 0;
    int pic_i = 0;
    PicPos pic_pos = PicPos::Stretch;
    int set_save_scheme = 0; // Snapshot when Display Properties opens (Cancel reverts)
    int set_save_pat = 0;
    int set_save_pic = 0;
    PicPos set_save_pic_pos = PicPos::Stretch;
    int pat_scroll = 0;
    int pic_scroll = 0;
    int scheme_scroll = 0;
    int head_mode_scroll = 0;
    int set_tab = 0;  // 0 Background, 1 Appearance, 2 Settings
    int set_list = 1; // Which list on the current tab has keyboard focus
    int mode_i = 0;
    int set_save_mode = 0;
    std::vector<VideoMode> modes;
    std::string mode_note;
    std::vector<ColorScheme> schemes;
    std::vector<ColorScheme> set_save_schemes;
    std::vector<WallChoice> patterns;
    std::vector<WallChoice> pictures;
    Pixmap wall_tile = 0; // 32x32 pattern
    int wall_tw = 0, wall_th = 0;
    Raster pic_img;
    Pixmap pic_pix = 0;
    int pic_pw = 0, pic_ph = 0;
    std::vector<PixCache> pic_scaled;

    std::vector<Head> heads;
    std::vector<Head> set_save_heads;
    int head_sel = 0;
    int head_drag = -1;
    int head_grab_dx = 0, head_grab_dy = 0;
    bool heads_dirty = false;
    bool head_view_lock = false;
    double head_sc = 1;
    int head_ox = 0, head_oy = 0, head_minx = 0, head_miny = 0;
    std::vector<Window> ident_wins;
    long identify_until = 0;

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
    int selected_icon = -1; // Keyboard focus among desktop icons, or -1
    std::vector<LaunchItem> desk_items;
    std::vector<char> desk_sel; // Parallel to desk_items; 1 = selected
    std::string desk_dir;
    time_t desk_mtime = 0;
    std::vector<LaunchItem> programs;
    int prog_scroll = 0;
    int prog_vis = 1;
    int sub_w = 150;
    int sub_h = 74;
    int sel_x0 = 0, sel_y0 = 0, sel_x1 = 0, sel_y1 = 0;
    int sel_mon = 0;
    std::vector<char> sel_base; // desk_sel at marquee start (Ctrl adds to this)
    int desk_press_i = -1; // Icon under the pointer at button-down, or -1
    int desk_press_x = 0, desk_press_y = 0;
    std::vector<int> desk_orig_x, desk_orig_y; // Positions at the start of an icon drag

    int btn_id = 0;
    Window btn_win = 0;
    int btn_x = 0, btn_y = 0, btn_w = 0, btn_h = 0;
    bool btn_in = false;
    Hit cap_hit = Hit::Miss;
    Client *cap_c = nullptr;
    bool cap_in = false;

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
    void draw_caption_btn(Drawable d, int x, int y, int kind, bool maxed, bool pressed = false);
    void draw_desktop(Monitor &m);
    void paint_desk_icons(Drawable d);
    void compose_desktop(Monitor &m);
    void free_desk_pix(Monitor &m);
    void invalidate_walls();
    void invalidate_icons();
    bool desk_shows_icons(const Monitor *m);
    void draw_taskbar(Monitor &m);
    void draw_clock(Drawable d, int x, int y, int w, int h);
    void draw_tray();
    void draw_startmenu(Monitor &m);
    void draw_submenu(Monitor &m);
    void draw_rundlg();
    void draw_shutdlg();
    void draw_radio(Drawable d, int x, int y, bool on);
    void draw_setdlg();
    void draw_colordlg();
    void draw_filedlg();
    void draw_dlg_btn(Drawable d, int x, int y, int w, int h, const char *lab, bool pressed = false);
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
    TrayItem *tray_by_win(Window w);
    void tray_send_xembed(Window w, long msg, long detail, long d1, long d2);
    void tray_prepare_socket(TrayItem &t, const XWindowAttributes &wa);
    bool read_xembed_info(Window w, long *ver, long *flags);
    void tray_apply_xembed(TrayItem &t);
    Window tray_add_owned();
    void tray_set_rgb(Window w, const std::vector<std::uint8_t> &rgb);
    void draw_tray_owned(const TrayItem &t);
    bool tray_icon_from_path(const std::string &path, std::vector<std::uint8_t> &rgb);
    void save_icon_layout();
    void apply_icon_layout();
    void auto_place_desk_icon(int i);
    void clamp_desk_icons();
    void snap_desk_sel();
    int desk_neighbor(int from, int dirx, int diry);

    unsigned mod_mask_for(KeySym ks);
    void grab_key(KeySym ks, unsigned mod);
    void close_run();
    void close_shut();
    void confirm_shut();
    void close_settings(bool revert);
    void open_settings(int mi);
    void rebuild_walls();
    void make_wall_tile();
    void apply_scheme(int i);
    void apply_pattern(int i);
    void apply_picture(int i);
    void apply_picpos(PicPos p);
    void apply_background();
    void free_pic_cache();
    Pixmap scaled_pic(int w, int h);
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
    void blit_rgb(Drawable dst, const std::uint8_t *rgb, int w, int h);
    void grab_dialog(Window w);
    void ungrab_if_idle();
    void refresh_heads();
    void save_heads();
    void load_heads();
    bool apply_heads();
    void snap_head(int i);
    void compute_head_view();
    bool head_view(int i, int &x, int &y, int &w, int &h);
    void view_to_layout(int px, int py, int &lx, int &ly);
    int head_at(int px, int py);
    void identify_heads();
    void hide_identify();
    void settings_apply(bool close);
    void draw_set_tabs();
    void draw_set_background();
    void draw_set_appearance();
    void draw_set_settings();
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
    void activate_launch(const LaunchItem &it);
    void ensure_list_scroll(int &scroll, int sel, int vis, int n);
    int pointer_mon();
    void refresh_modes();
    bool apply_mode(int i);
    void select_desk_icons(int x0, int y0, int x1, int y1);
    void desk_select_only(int i);
    void activate_desk_sel();
    void load_desktop();
    void maybe_reload_desktop();
    void load_programs();
    void seed_desktop();
    std::string xdg_documents_dir();
    void desk_cell(int i, int &x, int &y, int &w, int &h);
    int desk_rows();
    Monitor *mon_from_index(int i);
    void redraw_desktops();
    int desk_n() const;
    bool desk_is_sel(int i) const;
    int sub_item_at(int y);
    void submenu_geom(Monitor &m, int &w, int &h, int &vis);

    Hit hit_frame(Client *c, int x, int y);
    Cursor cursor_for(Hit h);
    void close_menus();
    void open_start(Monitor &m);
    void open_run(int mi);
    void open_shut(int mi);
    void handle_event(XEvent *e);
    bool btn_down(int id) const;
    bool cap_down(const Client *c, Hit h) const;
    void arm_btn(Window w, int id, int x, int y, int bw, int bh);
    void arm_cap(Client *c, Hit h);
    void paint_press();
    void update_press(int x, int y);
    void finish_press(bool activate);
    void fire_btn(int id);

    void on_button_press(XEvent *e);
    void on_button_release(XEvent *e);
    void on_motion(XEvent *e);
    void on_key(XEvent *e);
    void on_expose(XEvent *e);
};
