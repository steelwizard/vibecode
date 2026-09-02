// XDG Desktop and applications dirs for Chime's wallpaper icons and the
// Start → Programs flyout. Desktop files follow the freedesktop Desktop
// Entry spec enough to pick Name/Exec and honor Hidden/NoDisplay/OnlyShowIn.

#include "wm.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <map>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

namespace {

const char *kTermCmd = "aterm || xterm || x-terminal-emulator || true";
const char *kEditorCmd = "editor || aterm -e vi || xterm -e vi || true";

std::string home_dir()
{
    const char *h = getenv("HOME");
    return (h && *h) ? h : "/";
}

std::string trim(std::string s)
{
    while (!s.empty() && (unsigned char)s.back() <= ' ')
        s.pop_back();
    size_t i = 0;
    while (i < s.size() && (unsigned char)s[i] <= ' ')
        i++;
    return s.substr(i);
}

std::string unquote(std::string s)
{
    s = trim(s);
    if (s.size() >= 2 && ((s.front() == '"' && s.back() == '"') || (s.front() == '\'' && s.back() == '\'')))
        s = s.substr(1, s.size() - 2);
    return s;
}

std::string expand_home(std::string v, const std::string &home)
{
    v = unquote(v);
    const char *pat[] = {"$HOME", "${HOME}"};
    for (const char *p : pat) {
        size_t at = v.find(p);
        if (at != std::string::npos)
            v.replace(at, strlen(p), home);
    }
    return v;
}

bool ends_with(const std::string &s, const char *suf)
{
    size_t n = strlen(suf);
    return s.size() >= n && strcasecmp(s.c_str() + s.size() - n, suf) == 0;
}

bool mkdir_p(const std::string &path)
{
    if (path.empty())
        return false;
    std::string cur;
    size_t i = 0;
    if (path[0] == '/') {
        cur = "/";
        i = 1;
    }
    while (i < path.size()) {
        size_t j = path.find('/', i);
        if (j == std::string::npos)
            j = path.size();
        if (j > i) {
            if (cur.size() > 1)
                cur += '/';
            cur.append(path, i, j - i);
            mkdir(cur.c_str(), 0755);
        }
        i = j + 1;
    }
    return true;
}

std::string quote(const std::string &s)
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

std::string user_dir(const char *env_name, const char *fallback_rel)
{
    if (const char *e = getenv(env_name)) {
        if (*e && strcmp(e, home_dir().c_str()) != 0)
            return e;
    }
    std::string home = home_dir();
    std::string cfg = home + "/.config/user-dirs.dirs";
    FILE *f = fopen(cfg.c_str(), "r");
    std::string found;
    if (f) {
        char line[1024];
        std::string key = std::string(env_name) + "=";
        while (fgets(line, sizeof line, f)) {
            std::string s = trim(line);
            if (s.empty() || s[0] == '#')
                continue;
            if (s.compare(0, key.size(), key) != 0)
                continue;
            found = expand_home(s.substr(key.size()), home);
            break;
        }
        fclose(f);
    }
    if (!found.empty() && found != home)
        return found;
    return home + "/" + fallback_rel;
}

void split_colon(const char *s, std::vector<std::string> &out)
{
    if (!s || !*s)
        return;
    size_t i = 0;
    std::string str = s;
    while (i < str.size()) {
        size_t j = str.find(':', i);
        if (j == std::string::npos)
            j = str.size();
        if (j > i)
            out.emplace_back(str.substr(i, j - i));
        i = j + 1;
    }
}

void split_semi(const std::string &s, std::vector<std::string> &out)
{
    size_t i = 0;
    while (i < s.size()) {
        size_t j = s.find(';', i);
        if (j == std::string::npos)
            j = s.size();
        if (j > i)
            out.emplace_back(s.substr(i, j - i));
        i = j + 1;
    }
}

bool list_has(const std::string &list, const std::string &item)
{
    std::vector<std::string> parts;
    split_semi(list, parts);
    for (const auto &p : parts)
        if (p == item)
            return true;
    return false;
}

std::string current_desktop()
{
    const char *e = getenv("XDG_CURRENT_DESKTOP");
    return (e && *e) ? e : "Chime";
}

bool shown_here(const std::string &only, const std::string &notshow)
{
    std::vector<std::string> desks;
    split_colon(current_desktop().c_str(), desks);
    if (desks.empty())
        desks.emplace_back("Chime");
    if (!notshow.empty()) {
        for (const auto &d : desks)
            if (list_has(notshow, d))
                return false;
    }
    if (!only.empty()) {
        for (const auto &d : desks)
            if (list_has(only, d))
                return true;
        return false;
    }
    return true;
}

bool parse_bool(const std::string &v)
{
    return strcasecmp(v.c_str(), "true") == 0 || v == "1";
}

std::string strip_codes(const std::string &exec)
{
    std::string o;
    for (size_t i = 0; i < exec.size(); i++) {
        if (exec[i] == '%' && i + 1 < exec.size()) {
            i++;
            if (exec[i] == '%')
                o += '%';
            continue;
        }
        o += exec[i];
    }
    while (!o.empty() && o.back() == ' ')
        o.pop_back();
    return o;
}

struct DeskFile {
    std::string type, name, exec, tryexec, icon, categories, only, notshow, url;
    bool hidden = false, nodisplay = false, terminal = false;
};

bool parse_desktop(const std::string &path, DeskFile &out)
{
    FILE *f = fopen(path.c_str(), "r");
    if (!f)
        return false;
    char buf[4096];
    bool in = false;
    while (fgets(buf, sizeof buf, f)) {
        std::string s = trim(buf);
        if (s.empty() || s[0] == '#')
            continue;
        if (s[0] == '[') {
            in = (s == "[Desktop Entry]");
            continue;
        }
        if (!in)
            continue;
        size_t eq = s.find('=');
        if (eq == std::string::npos || eq == 0)
            continue;
        std::string k = s.substr(0, eq);
        std::string v = s.substr(eq + 1);
        if (k == "Type")
            out.type = v;
        else if (k == "Name" && out.name.empty())
            out.name = v;
        else if (k == "Exec")
            out.exec = v;
        else if (k == "TryExec")
            out.tryexec = v;
        else if (k == "Icon")
            out.icon = v;
        else if (k == "Categories")
            out.categories = v;
        else if (k == "OnlyShowIn")
            out.only = v;
        else if (k == "NotShowIn")
            out.notshow = v;
        else if (k == "URL")
            out.url = v;
        else if (k == "Hidden")
            out.hidden = parse_bool(v);
        else if (k == "NoDisplay")
            out.nodisplay = parse_bool(v);
        else if (k == "Terminal")
            out.terminal = parse_bool(v);
    }
    fclose(f);
    return true;
}

bool command_exists(const std::string &cmd)
{
    if (cmd.empty())
        return false;
    if (cmd.find('/') != std::string::npos)
        return access(cmd.c_str(), X_OK) == 0;
    const char *path = getenv("PATH");
    std::string paths = (path && *path) ? path : "/usr/local/bin:/usr/bin:/bin";
    size_t i = 0;
    while (i < paths.size()) {
        size_t j = paths.find(':', i);
        if (j == std::string::npos)
            j = paths.size();
        std::string dir = paths.substr(i, j - i);
        std::string full = dir.empty() ? cmd : dir + "/" + cmd;
        if (access(full.c_str(), X_OK) == 0)
            return true;
        i = j + 1;
    }
    return false;
}

bool has_ci(const std::string &s, const char *k)
{
    if (s.empty() || !k || !*k)
        return false;
    std::string a = s, b = k;
    for (char &c : a)
        c = (char)tolower((unsigned char)c);
    for (char &c : b)
        c = (char)tolower((unsigned char)c);
    return a.find(b) != std::string::npos;
}

int icon_kind(const std::string &name, const std::string &exec, const std::string &icon, const std::string &cats,
              bool dir)
{
    auto has = [](const std::string &s, const char *k) { return has_ci(s, k); };
    if (dir || has(cats, "FileManager") || has(icon, "folder") || has(name, "Documents"))
        return 1;
    if (has(name, "My Computer") || has(name, "Computer") || has(icon, "computer"))
        return 0;
    if (has(cats, "Terminal") || has(icon, "terminal") || has(exec, "aterm") || has(exec, "xterm") ||
        has(exec, "terminal"))
        return 2;
    return 3;
}

std::string wrap_term(const std::string &cmd)
{
    std::string q = quote(cmd);
    return std::string("aterm -e sh -c ") + q + " || xterm -e sh -c " + q + " || x-terminal-emulator -e sh -c " + q;
}

bool is_shell_script(const std::string &path)
{
    return ends_with(path, ".sh");
}

// Directories open in Cabinet. .sh files run (with sh if they lack +x, like a
// .bat on the 90s desktop). Other +x files run as-is. Everything else is
// handed to xdg-open / editor.
std::string file_cmd(const std::string &path, bool dir)
{
    std::string q = quote(path);
    if (dir)
        return "cabinet " + q;
    if (is_shell_script(path))
        return access(path.c_str(), X_OK) == 0 ? q : ("sh " + q);
    if (access(path.c_str(), X_OK) == 0)
        return q;
    return "xdg-open " + q + " 2>/dev/null || editor " + q + " 2>/dev/null || aterm -e vi " + q + " || xterm -e vi " + q;
}

std::string display_name(const std::string &filename)
{
    std::string n = filename;
    if (ends_with(n, ".desktop"))
        n.resize(n.size() - 8);
    return n;
}

bool desktop_to_launch(const std::string &path, const DeskFile &df, bool for_menu, LaunchItem &out)
{
    if (df.hidden)
        return false;
    if (for_menu && df.nodisplay)
        return false;
    if (for_menu && !shown_here(df.only, df.notshow))
        return false;
    std::string type = df.type.empty() ? "Application" : df.type;
    out.path = path;
    out.name = df.name.empty() ? display_name(path.substr(path.rfind('/') + 1)) : df.name;
    if (type == "Link") {
        std::string url = df.url;
        if (url.compare(0, 7, "file://") == 0)
            url = url.substr(7);
        if (url.empty())
            return false;
        struct stat st{};
        bool dir = (stat(url.c_str(), &st) == 0 && S_ISDIR(st.st_mode));
        out.exec = file_cmd(url, dir);
        out.dir = dir;
        out.kind = icon_kind(out.name, out.exec, df.icon, df.categories, dir);
        if (!dir && is_shell_script(url))
            out.kind = 2;
        return true;
    }
    if (type != "Application")
        return false;
    if (df.exec.empty())
        return false;
    if (!df.tryexec.empty() && !command_exists(df.tryexec))
        return false;
    out.exec = strip_codes(df.exec);
    if (out.exec.empty())
        return false;
    if (df.terminal)
        out.exec = wrap_term(out.exec);
    out.dir = false;
    out.kind = icon_kind(out.name, out.exec, df.icon, df.categories, false);
    return true;
}

void write_if_absent(const std::string &path, const char *body)
{
    if (access(path.c_str(), F_OK) == 0)
        return;
    FILE *f = fopen(path.c_str(), "w");
    if (!f)
        return;
    fputs(body, f);
    fclose(f);
}

void app_dirs(std::vector<std::string> &dirs)
{
    std::string home = home_dir();
    if (const char *dh = getenv("XDG_DATA_HOME"); dh && *dh)
        dirs.push_back(std::string(dh) + "/applications");
    else
        dirs.push_back(home + "/.local/share/applications");
    if (const char *dd = getenv("XDG_DATA_DIRS"); dd && *dd) {
        std::vector<std::string> roots;
        split_colon(dd, roots);
        for (const auto &r : roots)
            dirs.push_back(r + "/applications");
    } else {
        dirs.emplace_back("/usr/local/share/applications");
        dirs.emplace_back("/usr/share/applications");
    }
    if (const char *data = getenv("CHIME_DATA_DIR"); data && *data)
        dirs.push_back(std::string(data) + "/applications");
    std::vector<std::string> uniq;
    for (const auto &d : dirs) {
        if (std::find(uniq.begin(), uniq.end(), d) == uniq.end())
            uniq.push_back(d);
    }
    dirs.swap(uniq);
}

void scan_apps(const std::string &dir, std::map<std::string, LaunchItem> &by_id, int depth)
{
    if (depth > 2)
        return;
    DIR *d = opendir(dir.c_str());
    if (!d)
        return;
    while (dirent *e = readdir(d)) {
        const char *n = e->d_name;
        if (n[0] == '.')
            continue;
        if (!strcmp(n, "screensavers"))
            continue;
        std::string full = dir + "/" + n;
        struct stat st{};
        if (stat(full.c_str(), &st) != 0)
            continue;
        if (S_ISDIR(st.st_mode)) {
            scan_apps(full, by_id, depth + 1);
            continue;
        }
        if (!ends_with(full, ".desktop"))
            continue;
        std::string id = n;
        if (by_id.count(id))
            continue;
        DeskFile df;
        if (!parse_desktop(full, df))
            continue;
        LaunchItem it;
        if (!desktop_to_launch(full, df, true, it))
            continue;
        by_id.emplace(id, std::move(it));
    }
    closedir(d);
}

} // namespace

std::string WM::xdg_documents_dir()
{
    std::string p = user_dir("XDG_DOCUMENTS_DIR", "Documents");
    mkdir_p(p);
    return p;
}

static std::string desk_file_name(const std::string &p)
{
    auto sl = p.find_last_of('/');
    return sl == std::string::npos ? p : p.substr(sl + 1);
}

void WM::save_icon_layout()
{
    std::string dir = home_dir() + "/.chime";
    mkdir_p(dir);
    FILE *f = fopen((dir + "/icon-layout").c_str(), "w");
    if (!f)
        return;
    for (const auto &it : desk_items)
        std::fprintf(f, "%d %d %s\n", it.x, it.y, desk_file_name(it.path).c_str());
    fclose(f);
}

void WM::apply_icon_layout()
{
    std::map<std::string, std::pair<int, int>> pos;
    FILE *f = fopen((home_dir() + "/.chime/icon-layout").c_str(), "r");
    if (f) {
        char line[768];
        while (fgets(line, sizeof line, f)) {
            int x = 0, y = 0;
            char name[512];
            if (sscanf(line, "%d %d %511[^\n]", &x, &y, name) == 3)
                pos[name] = {x, y};
        }
        fclose(f);
    }
    for (auto &it : desk_items) {
        auto found = pos.find(desk_file_name(it.path));
        if (found != pos.end()) {
            it.x = found->second.first;
            it.y = found->second.second;
        } else
            it.x = it.y = -1;
    }
    for (int i = 0; i < desk_n(); i++)
        if (desk_items[i].x < 0)
            auto_place_desk_icon(i);
    clamp_desk_icons();
    save_icon_layout();
}

void WM::seed_desktop()
{
    std::string desk = user_dir("XDG_DESKTOP_DIR", "Desktop");
    struct stat st{};
    bool fresh = stat(desk.c_str(), &st) != 0;
    mkdir_p(desk);
    mkdir_p(xdg_documents_dir());
    if (!fresh)
        return;

    auto put = [&](const char *filename, const std::string &name, const std::string &exec) {
        std::string path = desk + "/" + filename;
        std::string body = "[Desktop Entry]\nType=Application\nName=" + name + "\nExec=" + exec + "\n";
        write_if_absent(path, body.c_str());
    };
    put("My Computer.desktop", "My Computer", "cabinet /");
    put("My Documents.desktop", "My Documents", "cabinet " + quote(xdg_documents_dir()));
    put("Terminal.desktop", "Terminal", std::string("sh -c ") + quote(kTermCmd));
    put("Editor.desktop", "Editor", std::string("sh -c ") + quote(kEditorCmd));
}

void WM::load_desktop()
{
    seed_desktop();
    std::string dir = user_dir("XDG_DESKTOP_DIR", "Desktop");
    desk_dir = dir;
    struct stat dst{};
    desk_mtime = (stat(dir.c_str(), &dst) == 0) ? dst.st_mtime : 0;

    std::string keep;
    if (selected_icon >= 0 && selected_icon < (int)desk_items.size())
        keep = desk_items[selected_icon].path;
    std::vector<std::string> kept_paths;
    for (size_t i = 0; i < desk_items.size() && i < desk_sel.size(); i++)
        if (desk_sel[i])
            kept_paths.push_back(desk_items[i].path);

    desk_items.clear();
    DIR *d = opendir(dir.c_str());
    if (d) {
        while (dirent *e = readdir(d)) {
            const char *n = e->d_name;
            if (n[0] == '.')
                continue;
            std::string full = dir + "/" + n;
            struct stat st{};
            if (stat(full.c_str(), &st) != 0)
                continue;
            LaunchItem it;
            it.path = full;
            if (S_ISDIR(st.st_mode)) {
                it.name = n;
                it.dir = true;
                it.exec = file_cmd(full, true);
                it.kind = 1;
                desk_items.push_back(std::move(it));
                continue;
            }
            if (ends_with(full, ".desktop")) {
                DeskFile df;
                if (parse_desktop(full, df) && desktop_to_launch(full, df, false, it)) {
                    desk_items.push_back(std::move(it));
                    continue;
                }
            }
            it.name = n;
            it.dir = false;
            it.exec = file_cmd(full, false);
            it.kind = is_shell_script(full) ? 2 : 3;
            desk_items.push_back(std::move(it));
        }
        closedir(d);
    }
    std::sort(desk_items.begin(), desk_items.end(), [](const LaunchItem &a, const LaunchItem &b) {
        if (a.dir != b.dir)
            return a.dir;
        return strcasecmp(a.name.c_str(), b.name.c_str()) < 0;
    });
    if ((int)desk_items.size() > 256)
        desk_items.resize(256);

    apply_icon_layout();

    desk_sel.assign(desk_items.size(), 0);
    selected_icon = -1;
    for (int i = 0; i < (int)desk_items.size(); i++) {
        if (desk_items[i].path == keep)
            selected_icon = i;
        for (const auto &p : kept_paths)
            if (desk_items[i].path == p)
                desk_sel[i] = 1;
    }
    invalidate_icons();
}

void WM::maybe_reload_desktop()
{
    if (drag == DragMode::Select || drag == DragMode::Icons || desk_press_i >= 0)
        return;
    std::string dir = user_dir("XDG_DESKTOP_DIR", "Desktop");
    struct stat st{};
    time_t mt = (stat(dir.c_str(), &st) == 0) ? st.st_mtime : 0;
    if (dir == desk_dir && mt == desk_mtime)
        return;
    load_desktop();
}

void WM::load_programs()
{
    std::vector<std::string> dirs;
    app_dirs(dirs);
    std::map<std::string, LaunchItem> by_id;
    for (const auto &dir : dirs)
        scan_apps(dir, by_id, 0);
    programs.clear();
    programs.reserve(by_id.size());
    for (auto &kv : by_id)
        programs.push_back(std::move(kv.second));
    std::sort(programs.begin(), programs.end(),
              [](const LaunchItem &a, const LaunchItem &b) { return strcasecmp(a.name.c_str(), b.name.c_str()) < 0; });
    if ((int)programs.size() > 1000)
        programs.resize(1000);
    prog_scroll = 0;
}
