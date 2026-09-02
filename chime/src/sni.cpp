// StatusNotifierItem host. Qt/Steam/AppIndicator talk to
// org.kde.StatusNotifierWatcher over the session bus; we own that name,
// paint each item as a 20x20 tray window, and forward clicks.

#include "sni.h"
#include "theme.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unistd.h>
#include <vector>

#ifdef CHIME_SNI
#include <dbus/dbus.h>
#endif

namespace {

#ifdef CHIME_SNI

constexpr int kSz = w95::kTrayIcon;
const char *kWatchKde = "org.kde.StatusNotifierWatcher";
const char *kWatchFdo = "org.freedesktop.StatusNotifierWatcher";
const char *kItemKde = "org.kde.StatusNotifierItem";
const char *kItemFdo = "org.freedesktop.StatusNotifierItem";
const char *kWatchPath = "/StatusNotifierWatcher";
const char *kPropIface = "org.freedesktop.DBus.Properties";

struct SniItem {
    std::string dest;
    std::string path;
    std::string id;
    std::string unique;
    Window win = 0;
};

struct SniHost {
    WM *wm = nullptr;
    DBusConnection *conn = nullptr;
    std::vector<SniItem> items;
    std::vector<std::string> pending;
};

SniHost *host(WM *wm)
{
    return wm ? (SniHost *)wm->sni : nullptr;
}

void argb_to_face(const std::uint8_t *data, int w, int h, int stride, bool bgra, std::vector<std::uint8_t> &out)
{
    out.assign((size_t)kSz * kSz * 3, 192);
    if (!data || w < 1 || h < 1 || stride < w * 4)
        return;
    for (int y = 0; y < kSz; y++) {
        int sy = y * h / kSz;
        for (int x = 0; x < kSz; x++) {
            int sx = x * w / kSz;
            const std::uint8_t *p = data + sy * stride + sx * 4;
            std::uint8_t a, r, g, b;
            if (bgra) {
                b = p[0];
                g = p[1];
                r = p[2];
                a = p[3];
            } else {
                a = p[0];
                r = p[1];
                g = p[2];
                b = p[3];
            }
            std::uint8_t *d = &out[((size_t)y * kSz + x) * 3];
            d[0] = (std::uint8_t)((r * a + 192 * (255 - a)) / 255);
            d[1] = (std::uint8_t)((g * a + 192 * (255 - a)) / 255);
            d[2] = (std::uint8_t)((b * a + 192 * (255 - a)) / 255);
        }
    }
}

bool pixmap_looks_bgra(const std::uint8_t *data, int n)
{
    int a0 = 0, a3 = 0, samples = 0;
    for (int i = 0; i + 4 <= n && samples < 96; i += 4, samples++) {
        a0 += data[i];
        a3 += data[i + 3];
    }
    return samples > 8 && a3 > a0 * 2 + 64;
}

DBusMessage *props_get(SniHost *h, const char *dest, const char *path, const char *iface, const char *prop)
{
    DBusError err;
    dbus_error_init(&err);
    DBusMessage *m = dbus_message_new_method_call(dest, path, kPropIface, "Get");
    if (!m)
        return nullptr;
    dbus_message_append_args(m, DBUS_TYPE_STRING, &iface, DBUS_TYPE_STRING, &prop, DBUS_TYPE_INVALID);
    DBusMessage *r = dbus_connection_send_with_reply_and_block(h->conn, m, 400, &err);
    dbus_message_unref(m);
    if (dbus_error_is_set(&err)) {
        dbus_error_free(&err);
        if (r)
            dbus_message_unref(r);
        return nullptr;
    }
    return r;
}

bool parse_variant_string(DBusMessage *r, std::string &out)
{
    DBusMessageIter iter, var;
    if (!r || !dbus_message_iter_init(r, &iter) || dbus_message_iter_get_arg_type(&iter) != DBUS_TYPE_VARIANT)
        return false;
    dbus_message_iter_recurse(&iter, &var);
    if (dbus_message_iter_get_arg_type(&var) != DBUS_TYPE_STRING)
        return false;
    const char *s = nullptr;
    dbus_message_iter_get_basic(&var, &s);
    if (!s)
        return false;
    out = s;
    return true;
}

bool parse_icon_pixmap(DBusMessage *r, std::vector<std::uint8_t> &rgb)
{
    DBusMessageIter iter, var, arr, st, bytes;
    if (!r || !dbus_message_iter_init(r, &iter) || dbus_message_iter_get_arg_type(&iter) != DBUS_TYPE_VARIANT)
        return false;
    dbus_message_iter_recurse(&iter, &var);
    if (dbus_message_iter_get_arg_type(&var) != DBUS_TYPE_ARRAY)
        return false;
    dbus_message_iter_recurse(&var, &arr);
    int best_w = 0, best_h = 0, best_score = 100000;
    const std::uint8_t *best = nullptr;
    int best_n = 0;
    while (dbus_message_iter_get_arg_type(&arr) == DBUS_TYPE_STRUCT) {
        dbus_message_iter_recurse(&arr, &st);
        dbus_int32_t w = 0, h = 0;
        if (dbus_message_iter_get_arg_type(&st) != DBUS_TYPE_INT32)
            break;
        dbus_message_iter_get_basic(&st, &w);
        dbus_message_iter_next(&st);
        if (dbus_message_iter_get_arg_type(&st) != DBUS_TYPE_INT32)
            break;
        dbus_message_iter_get_basic(&st, &h);
        dbus_message_iter_next(&st);
        if (w < 1 || h < 1 || w > 256 || h > 256) {
            dbus_message_iter_next(&arr);
            continue;
        }
        if (dbus_message_iter_get_arg_type(&st) != DBUS_TYPE_ARRAY) {
            dbus_message_iter_next(&arr);
            continue;
        }
        dbus_message_iter_recurse(&st, &bytes);
        if (dbus_message_iter_get_arg_type(&bytes) != DBUS_TYPE_BYTE) {
            dbus_message_iter_next(&arr);
            continue;
        }
        int n = 0;
        std::uint8_t *ptr = nullptr;
        dbus_message_iter_get_fixed_array(&bytes, &ptr, &n);
        int score = abs(w - kSz) + abs(h - kSz);
        if (ptr && n >= w * h * 4 && score < best_score) {
            best_score = score;
            best_w = w;
            best_h = h;
            best = ptr;
            best_n = n;
        }
        dbus_message_iter_next(&arr);
    }
    if (!best)
        return false;
    int stride = best_n / best_h;
    if (stride < best_w * 4)
        stride = best_w * 4;
    argb_to_face(best, best_w, best_h, stride, pixmap_looks_bgra(best, best_n), rgb);
    return true;
}

bool find_icon_file(const std::string &name, std::string &out)
{
    if (name.empty())
        return false;
    if (name[0] == '/') {
        if (access(name.c_str(), R_OK) == 0) {
            out = name;
            return true;
        }
        return false;
    }
    const char *home = getenv("HOME");
    std::string local = home && *home ? std::string(home) + "/.local/share/icons/" : "";
    const char *dirs[] = {local.c_str(),
                          "/usr/share/icons/hicolor/22x22/apps/",
                          "/usr/share/icons/hicolor/24x24/apps/",
                          "/usr/share/icons/hicolor/16x16/apps/",
                          "/usr/share/icons/hicolor/32x32/apps/",
                          "/usr/share/icons/hicolor/48x48/apps/",
                          "/usr/share/icons/hicolor/22x22/status/",
                          "/usr/share/icons/hicolor/24x24/status/",
                          "/usr/share/icons/hicolor/32x32/status/",
                          "/usr/share/pixmaps/",
                          "/usr/share/icons/Adwaita/22x22/apps/",
                          "/usr/share/icons/Adwaita/24x24/status/",
                          "/usr/share/icons/Adwaita/22x22/status/",
                          nullptr};
    const char *ext[] = {".png", ".ppm", ".bmp", "", nullptr};
    for (int d = 0; dirs[d]; d++) {
        if (!dirs[d][0])
            continue;
        for (int e = 0; ext[e]; e++) {
            std::string p = std::string(dirs[d]) + name + ext[e];
            if (access(p.c_str(), R_OK) == 0) {
                out = p;
                return true;
            }
        }
    }
    return false;
}

SniItem *find_item(SniHost *h, const std::string &id)
{
    for (auto &it : h->items)
        if (it.id == id)
            return &it;
    return nullptr;
}

SniItem *find_item_win(SniHost *h, Window w)
{
    for (auto &it : h->items)
        if (it.win == w)
            return &it;
    return nullptr;
}

SniItem *find_item_dest(SniHost *h, const char *dest)
{
    if (!dest)
        return nullptr;
    for (auto &it : h->items)
        if (it.dest == dest)
            return &it;
    return nullptr;
}

void sni_refresh(SniHost *h, SniItem *it)
{
    if (!h || !it || !it->win)
        return;
    std::vector<std::uint8_t> rgb;
    const char *ifaces[] = {kItemKde, kItemFdo, nullptr};
    bool got = false;
    for (int i = 0; ifaces[i] && !got; i++) {
        DBusMessage *r = props_get(h, it->dest.c_str(), it->path.c_str(), ifaces[i], "IconPixmap");
        if (r) {
            got = parse_icon_pixmap(r, rgb);
            dbus_message_unref(r);
        }
    }
    if (!got) {
        for (int i = 0; ifaces[i] && !got; i++) {
            DBusMessage *r = props_get(h, it->dest.c_str(), it->path.c_str(), ifaces[i], "AttentionIconPixmap");
            if (r) {
                got = parse_icon_pixmap(r, rgb);
                dbus_message_unref(r);
            }
        }
    }
    if (!got) {
        std::string name;
        for (int i = 0; ifaces[i] && name.empty(); i++) {
            DBusMessage *r = props_get(h, it->dest.c_str(), it->path.c_str(), ifaces[i], "IconName");
            if (r) {
                parse_variant_string(r, name);
                dbus_message_unref(r);
            }
        }
        std::string path;
        if (!name.empty() && find_icon_file(name, path))
            got = h->wm->tray_icon_from_path(path, rgb);
    }
    if (!got)
        rgb.assign((size_t)kSz * kSz * 3, 192);
    h->wm->tray_set_rgb(it->win, rgb);
}

void emit_str(SniHost *h, const char *member, const std::string &s)
{
    DBusMessage *m = dbus_message_new_signal(kWatchPath, kWatchKde, member);
    if (!m)
        return;
    const char *p = s.c_str();
    dbus_message_append_args(m, DBUS_TYPE_STRING, &p, DBUS_TYPE_INVALID);
    dbus_connection_send(h->conn, m, nullptr);
    dbus_message_unref(m);
    DBusMessage *m2 = dbus_message_new_signal(kWatchPath, kWatchFdo, member);
    if (m2) {
        dbus_message_append_args(m2, DBUS_TYPE_STRING, &p, DBUS_TYPE_INVALID);
        dbus_connection_send(h->conn, m2, nullptr);
        dbus_message_unref(m2);
    }
    dbus_connection_flush(h->conn);
}

void append_variant_bool(DBusMessageIter *iter, dbus_bool_t v)
{
    DBusMessageIter var;
    dbus_message_iter_open_container(iter, DBUS_TYPE_VARIANT, "b", &var);
    dbus_message_iter_append_basic(&var, DBUS_TYPE_BOOLEAN, &v);
    dbus_message_iter_close_container(iter, &var);
}

void append_variant_int(DBusMessageIter *iter, dbus_int32_t v)
{
    DBusMessageIter var;
    dbus_message_iter_open_container(iter, DBUS_TYPE_VARIANT, "i", &var);
    dbus_message_iter_append_basic(&var, DBUS_TYPE_INT32, &v);
    dbus_message_iter_close_container(iter, &var);
}

void append_variant_as(DBusMessageIter *iter, SniHost *h)
{
    DBusMessageIter var, arr;
    dbus_message_iter_open_container(iter, DBUS_TYPE_VARIANT, "as", &var);
    dbus_message_iter_open_container(&var, DBUS_TYPE_ARRAY, "s", &arr);
    for (auto &it : h->items) {
        const char *id = it.id.c_str();
        dbus_message_iter_append_basic(&arr, DBUS_TYPE_STRING, &id);
    }
    dbus_message_iter_close_container(&var, &arr);
    dbus_message_iter_close_container(iter, &var);
}

void reply_empty(DBusConnection *c, DBusMessage *m)
{
    DBusMessage *r = dbus_message_new_method_return(m);
    if (!r)
        return;
    dbus_connection_send(c, r, nullptr);
    dbus_message_unref(r);
}

bool handle_get(SniHost *h, DBusMessage *m)
{
    DBusError err;
    dbus_error_init(&err);
    const char *iface = nullptr, *prop = nullptr;
    if (!dbus_message_get_args(m, &err, DBUS_TYPE_STRING, &iface, DBUS_TYPE_STRING, &prop, DBUS_TYPE_INVALID)) {
        dbus_error_free(&err);
        return false;
    }
    DBusMessage *r = dbus_message_new_method_return(m);
    if (!r)
        return false;
    DBusMessageIter iter;
    dbus_message_iter_init_append(r, &iter);
    if (!strcmp(prop, "IsStatusNotifierHostRegistered"))
        append_variant_bool(&iter, TRUE);
    else if (!strcmp(prop, "ProtocolVersion"))
        append_variant_int(&iter, 0);
    else if (!strcmp(prop, "RegisteredStatusNotifierItems"))
        append_variant_as(&iter, h);
    else {
        dbus_message_unref(r);
        return false;
    }
    dbus_connection_send(h->conn, r, nullptr);
    dbus_message_unref(r);
    return true;
}

bool handle_get_all(SniHost *h, DBusMessage *m)
{
    DBusMessage *r = dbus_message_new_method_return(m);
    if (!r)
        return false;
    DBusMessageIter iter, arr, dict;
    dbus_message_iter_init_append(r, &iter);
    dbus_message_iter_open_container(&iter, DBUS_TYPE_ARRAY, "{sv}", &arr);
    auto put = [&](const char *key, auto fn) {
        dbus_message_iter_open_container(&arr, DBUS_TYPE_DICT_ENTRY, nullptr, &dict);
        dbus_message_iter_append_basic(&dict, DBUS_TYPE_STRING, &key);
        fn(&dict);
        dbus_message_iter_close_container(&arr, &dict);
    };
    put("IsStatusNotifierHostRegistered", [&](DBusMessageIter *d) { append_variant_bool(d, TRUE); });
    put("ProtocolVersion", [&](DBusMessageIter *d) { append_variant_int(d, 0); });
    put("RegisteredStatusNotifierItems", [&](DBusMessageIter *d) { append_variant_as(d, h); });
    dbus_message_iter_close_container(&iter, &arr);
    dbus_connection_send(h->conn, r, nullptr);
    dbus_message_unref(r);
    return true;
}

const char *kIntrospect =
    "<!DOCTYPE node PUBLIC \"-//freedesktop//DTD D-BUS Object Introspection 1.0//EN\" "
    "\"http://www.freedesktop.org/standards/dbus/1.0/introspect.dtd\">\n"
    "<node>\n"
    "  <interface name=\"org.freedesktop.DBus.Introspectable\">\n"
    "    <method name=\"Introspect\"><arg name=\"xml\" type=\"s\" direction=\"out\"/></method>\n"
    "  </interface>\n"
    "  <interface name=\"org.freedesktop.DBus.Properties\">\n"
    "    <method name=\"Get\">\n"
    "      <arg name=\"interface\" type=\"s\" direction=\"in\"/>\n"
    "      <arg name=\"prop\" type=\"s\" direction=\"in\"/>\n"
    "      <arg name=\"value\" type=\"v\" direction=\"out\"/>\n"
    "    </method>\n"
    "    <method name=\"GetAll\">\n"
    "      <arg name=\"interface\" type=\"s\" direction=\"in\"/>\n"
    "      <arg name=\"props\" type=\"a{sv}\" direction=\"out\"/>\n"
    "    </method>\n"
    "  </interface>\n"
    "  <interface name=\"org.kde.StatusNotifierWatcher\">\n"
    "    <method name=\"RegisterStatusNotifierItem\"><arg name=\"service\" type=\"s\" direction=\"in\"/></method>\n"
    "    <method name=\"RegisterStatusNotifierHost\"><arg name=\"service\" type=\"s\" direction=\"in\"/></method>\n"
    "    <property name=\"RegisteredStatusNotifierItems\" type=\"as\" access=\"read\"/>\n"
    "    <property name=\"IsStatusNotifierHostRegistered\" type=\"b\" access=\"read\"/>\n"
    "    <property name=\"ProtocolVersion\" type=\"i\" access=\"read\"/>\n"
    "    <signal name=\"StatusNotifierItemRegistered\"><arg name=\"service\" type=\"s\"/></signal>\n"
    "    <signal name=\"StatusNotifierItemUnregistered\"><arg name=\"service\" type=\"s\"/></signal>\n"
    "    <signal name=\"StatusNotifierHostRegistered\"/>\n"
    "  </interface>\n"
    "</node>\n";

void handle_introspect(SniHost *h, DBusMessage *m)
{
    DBusMessage *r = dbus_message_new_method_return(m);
    if (!r)
        return;
    dbus_message_append_args(r, DBUS_TYPE_STRING, &kIntrospect, DBUS_TYPE_INVALID);
    dbus_connection_send(h->conn, r, nullptr);
    dbus_message_unref(r);
}

void register_item(SniHost *h, const char *sender, const char *svc)
{
    if (!sender || !svc || !svc[0])
        return;
    SniItem it;
    if (svc[0] == '/') {
        it.dest = sender;
        it.path = svc;
    } else {
        it.dest = svc;
        it.path = "/StatusNotifierItem";
    }
    it.id = it.dest + it.path;
    it.unique = sender ? sender : "";
    if (find_item(h, it.id))
        return;
    it.win = h->wm->tray_add_owned();
    if (!it.win)
        return;
    h->items.push_back(it);
    h->pending.push_back(it.id);
    emit_str(h, "StatusNotifierItemRegistered", it.id);
}

void unregister_item(SniHost *h, const std::string &id)
{
    SniItem *it = find_item(h, id);
    if (!it)
        return;
    Window w = it->win;
    for (auto i = h->items.begin(); i != h->items.end(); ++i) {
        if (i->id == id) {
            h->items.erase(i);
            break;
        }
    }
    if (w)
        h->wm->tray_undock(w);
    emit_str(h, "StatusNotifierItemUnregistered", id);
}

void handle_register_item(SniHost *h, DBusMessage *m)
{
    DBusError err;
    dbus_error_init(&err);
    const char *svc = nullptr;
    if (!dbus_message_get_args(m, &err, DBUS_TYPE_STRING, &svc, DBUS_TYPE_INVALID)) {
        dbus_error_free(&err);
        return;
    }
    register_item(h, dbus_message_get_sender(m), svc);
    reply_empty(h->conn, m);
}

DBusHandlerResult filter(DBusConnection *, DBusMessage *m, void *data)
{
    auto *h = (SniHost *)data;
    if (!h || !m)
        return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;

    const char *path = dbus_message_get_path(m);
    if (dbus_message_is_method_call(m, "org.freedesktop.DBus.Introspectable", "Introspect") && path &&
        (!strcmp(path, kWatchPath) || !strcmp(path, "/"))) {
        handle_introspect(h, m);
        return DBUS_HANDLER_RESULT_HANDLED;
    }
    if (dbus_message_is_method_call(m, "org.freedesktop.DBus.Peer", "Ping") && path && !strcmp(path, kWatchPath)) {
        reply_empty(h->conn, m);
        return DBUS_HANDLER_RESULT_HANDLED;
    }
    if (dbus_message_is_method_call(m, kPropIface, "Get") && path && !strcmp(path, kWatchPath)) {
        handle_get(h, m);
        return DBUS_HANDLER_RESULT_HANDLED;
    }
    if (dbus_message_is_method_call(m, kPropIface, "GetAll") && path && !strcmp(path, kWatchPath)) {
        handle_get_all(h, m);
        return DBUS_HANDLER_RESULT_HANDLED;
    }
    if (path && !strcmp(path, kWatchPath) && dbus_message_get_member(m) &&
        !strcmp(dbus_message_get_member(m), "RegisterStatusNotifierItem") &&
        dbus_message_get_type(m) == DBUS_MESSAGE_TYPE_METHOD_CALL) {
        handle_register_item(h, m);
        return DBUS_HANDLER_RESULT_HANDLED;
    }
    if (path && !strcmp(path, kWatchPath) && dbus_message_get_member(m) &&
        !strcmp(dbus_message_get_member(m), "RegisterStatusNotifierHost") &&
        dbus_message_get_type(m) == DBUS_MESSAGE_TYPE_METHOD_CALL) {
        reply_empty(h->conn, m);
        return DBUS_HANDLER_RESULT_HANDLED;
    }

    if (dbus_message_is_signal(m, "org.freedesktop.DBus", "NameOwnerChanged")) {
        const char *name = nullptr, *old_o = nullptr, *new_o = nullptr;
        if (dbus_message_get_args(m, nullptr, DBUS_TYPE_STRING, &name, DBUS_TYPE_STRING, &old_o, DBUS_TYPE_STRING,
                                  &new_o, DBUS_TYPE_INVALID) &&
            name && new_o && new_o[0] == 0) {
            std::vector<std::string> gone;
            for (auto &it : h->items)
                if (it.dest == name || it.unique == name)
                    gone.push_back(it.id);
            for (auto &id : gone)
                unregister_item(h, id);
        }
        return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
    }

    const char *iface = dbus_message_get_interface(m);
    const char *member = dbus_message_get_member(m);
    if (dbus_message_get_type(m) == DBUS_MESSAGE_TYPE_SIGNAL && member && iface &&
        (!strcmp(iface, kItemKde) || !strcmp(iface, kItemFdo))) {
        const char *sender = dbus_message_get_sender(m);
        const char *ipath = dbus_message_get_path(m);
        SniItem *it = nullptr;
        if (sender && ipath) {
            std::string id = std::string(sender) + ipath;
            it = find_item(h, id);
            if (!it)
                it = find_item_dest(h, sender);
        }
        if (it && (!strcmp(member, "NewIcon") || !strcmp(member, "NewAttentionIcon") || !strcmp(member, "NewStatus") ||
                   !strcmp(member, "NewOverlayIcon")))
            h->pending.push_back(it->id);
        return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
    }
    return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
}

void call_item(SniHost *h, SniItem *it, const char *method, int x, int y)
{
    const char *ifaces[] = {kItemKde, kItemFdo, nullptr};
    for (int i = 0; ifaces[i]; i++) {
        DBusMessage *m = dbus_message_new_method_call(it->dest.c_str(), it->path.c_str(), ifaces[i], method);
        if (!m)
            continue;
        dbus_int32_t xi = x, yi = y;
        dbus_message_append_args(m, DBUS_TYPE_INT32, &xi, DBUS_TYPE_INT32, &yi, DBUS_TYPE_INVALID);
        dbus_connection_send(h->conn, m, nullptr);
        dbus_message_unref(m);
    }
    dbus_connection_flush(h->conn);
}

void call_scroll(SniHost *h, SniItem *it, int delta)
{
    const char *ifaces[] = {kItemKde, kItemFdo, nullptr};
    const char *ori = "vertical";
    for (int i = 0; ifaces[i]; i++) {
        DBusMessage *m = dbus_message_new_method_call(it->dest.c_str(), it->path.c_str(), ifaces[i], "Scroll");
        if (!m)
            continue;
        dbus_int32_t d = delta;
        dbus_message_append_args(m, DBUS_TYPE_INT32, &d, DBUS_TYPE_STRING, &ori, DBUS_TYPE_INVALID);
        dbus_connection_send(h->conn, m, nullptr);
        dbus_message_unref(m);
    }
    dbus_connection_flush(h->conn);
}

#endif // CHIME_SNI

} // namespace

bool sni_start(WM *wm)
{
#ifdef CHIME_SNI
    if (!wm || wm->sni)
        return true;
    DBusError err;
    dbus_error_init(&err);
    DBusConnection *conn = dbus_bus_get(DBUS_BUS_SESSION, &err);
    if (!conn || dbus_error_is_set(&err)) {
        dbus_error_free(&err);
        return false;
    }
    dbus_connection_set_exit_on_disconnect(conn, FALSE);
    int flags = DBUS_NAME_FLAG_REPLACE_EXISTING | DBUS_NAME_FLAG_DO_NOT_QUEUE;
    if (dbus_bus_request_name(conn, kWatchKde, flags, &err) == -1) {
        dbus_error_free(&err);
        dbus_error_init(&err);
    }
    dbus_error_free(&err);
    dbus_error_init(&err);
    dbus_bus_request_name(conn, kWatchFdo, flags, &err);
    dbus_error_free(&err);

    auto *h = new SniHost;
    h->wm = wm;
    h->conn = conn;
    wm->sni = h;
    dbus_connection_add_filter(conn, filter, h, nullptr);
    dbus_bus_add_match(conn, "type='signal',interface='org.freedesktop.DBus',member='NameOwnerChanged'", nullptr);
    dbus_bus_add_match(conn, "type='signal',interface='org.kde.StatusNotifierItem'", nullptr);
    dbus_bus_add_match(conn, "type='signal',interface='org.freedesktop.StatusNotifierItem'", nullptr);
    dbus_connection_flush(conn);

    DBusMessage *sig = dbus_message_new_signal(kWatchPath, kWatchKde, "StatusNotifierHostRegistered");
    if (sig) {
        dbus_connection_send(conn, sig, nullptr);
        dbus_message_unref(sig);
    }
    sig = dbus_message_new_signal(kWatchPath, kWatchFdo, "StatusNotifierHostRegistered");
    if (sig) {
        dbus_connection_send(conn, sig, nullptr);
        dbus_message_unref(sig);
    }
    dbus_connection_flush(conn);
    return true;
#else
    (void)wm;
    return false;
#endif
}

void sni_stop(WM *wm)
{
#ifdef CHIME_SNI
    auto *h = host(wm);
    if (!h)
        return;
    std::vector<Window> wins;
    for (auto &it : h->items)
        if (it.win)
            wins.push_back(it.win);
    h->items.clear();
    for (Window w : wins)
        wm->tray_undock(w);
    if (h->conn) {
        dbus_connection_remove_filter(h->conn, filter, h);
        DBusError err;
        dbus_error_init(&err);
        dbus_bus_release_name(h->conn, kWatchKde, &err);
        dbus_error_free(&err);
        dbus_error_init(&err);
        dbus_bus_release_name(h->conn, kWatchFdo, &err);
        dbus_error_free(&err);
        dbus_connection_unref(h->conn);
    }
    delete h;
    wm->sni = nullptr;
#else
    (void)wm;
#endif
}

void sni_dispatch(WM *wm)
{
#ifdef CHIME_SNI
    auto *h = host(wm);
    if (!h || !h->conn)
        return;
    while (dbus_connection_get_dispatch_status(h->conn) == DBUS_DISPATCH_DATA_REMAINS)
        dbus_connection_dispatch(h->conn);
    dbus_connection_read_write_dispatch(h->conn, 0);
    auto pending = h->pending;
    h->pending.clear();
    std::sort(pending.begin(), pending.end());
    pending.erase(std::unique(pending.begin(), pending.end()), pending.end());
    for (auto &id : pending)
        if (SniItem *it = find_item(h, id))
            sni_refresh(h, it);
#else
    (void)wm;
#endif
}

int sni_fd(WM *wm)
{
#ifdef CHIME_SNI
    auto *h = host(wm);
    int fd = -1;
    if (h && h->conn && dbus_connection_get_unix_fd(h->conn, &fd) && fd >= 0)
        return fd;
    return -1;
#else
    (void)wm;
    return -1;
#endif
}

bool sni_handle_click(WM *wm, const XButtonEvent *b)
{
#ifdef CHIME_SNI
    auto *h = host(wm);
    if (!h || !b)
        return false;
    SniItem *it = find_item_win(h, b->window);
    if (!it)
        return false;
    if (b->button == 1)
        call_item(h, it, "Activate", b->x_root, b->y_root);
    else if (b->button == 2)
        call_item(h, it, "SecondaryActivate", b->x_root, b->y_root);
    else if (b->button == 3)
        call_item(h, it, "ContextMenu", b->x_root, b->y_root);
    else if (b->button == 4)
        call_scroll(h, it, 120);
    else if (b->button == 5)
        call_scroll(h, it, -120);
    return true;
#else
    (void)wm;
    (void)b;
    return false;
#endif
}
