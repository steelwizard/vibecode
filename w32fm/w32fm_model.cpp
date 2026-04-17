#define UNICODE
#define _UNICODE

#include "w32fm_model.h"

#include "w32fm_fs.h"

#include <windows.h>

#include <algorithm>

namespace {
int compare_entries(const Entry& a, const Entry& b) {
    if (a.is_parent != b.is_parent) return a.is_parent ? -1 : 1;
    if (a.is_dir != b.is_dir) return a.is_dir ? -1 : 1;
    int c = CompareStringOrdinal(a.name.c_str(), -1, b.name.c_str(), -1, TRUE);
    if (c == CSTR_LESS_THAN) return -1;
    if (c == CSTR_GREATER_THAN) return 1;
    return 0;
}
}

bool fm_load_directory(BrowserState& st, const std::wstring& path, std::wstring& err) {
    std::wstring canonical;
    if (!fm_get_canonical_path(path, canonical, err)) return false;
    if (!fm_is_directory(canonical)) {
        err = L"Not a directory.";
        return false;
    }

    std::vector<Entry> out;
    std::wstring pattern = fm_join_path(canonical, L"*");
    WIN32_FIND_DATAW fd{};
    HANDLE h = FindFirstFileW(pattern.c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) {
        err = fm_last_error_message(GetLastError());
        return false;
    }

    do {
        const wchar_t* n = fd.cFileName;
        if (wcscmp(n, L".") == 0 || wcscmp(n, L"..") == 0) continue;
        bool is_dir = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
        bool is_regular = !is_dir && (fd.dwFileAttributes & FILE_ATTRIBUTE_DEVICE) == 0;
        if (!is_dir && !is_regular) continue;
        out.push_back(Entry{n, is_dir, false});
    } while (FindNextFileW(h, &fd));
    FindClose(h);

    std::sort(out.begin(), out.end(), [](const Entry& a, const Entry& b) { return compare_entries(a, b) < 0; });

    if (!fm_is_root_path(canonical)) out.insert(out.begin(), Entry{L"[..]", true, true});

    st.cwd = canonical;
    st.entries = std::move(out);
    st.selected = 0;
    st.scroll = 0;
    st.cwd_writable = fm_cwd_writable(st.cwd);
    return true;
}

const Entry* fm_selected_entry(const BrowserState& st) {
    if (st.entries.empty() || st.selected >= st.entries.size()) return nullptr;
    return &st.entries[st.selected];
}

std::wstring fm_selected_full_path(const BrowserState& st) {
    const Entry* e = fm_selected_entry(st);
    if (!e || e->is_parent) return L"";
    return fm_join_path(st.cwd, e->name);
}

bool fm_open_selected(BrowserState& st, std::wstring& err) {
    const Entry* e = fm_selected_entry(st);
    if (!e) return false;
    if (e->is_parent) {
        std::wstring p;
        if (!fm_parent_path(st.cwd, p)) return false;
        return fm_load_directory(st, p, err);
    }
    if (!e->is_dir) return false;
    return fm_load_directory(st, fm_join_path(st.cwd, e->name), err);
}

void fm_move_selection_up(BrowserState& st) {
    if (st.selected > 0) st.selected--;
}

void fm_move_selection_down(BrowserState& st) {
    if (st.selected + 1 < st.entries.size()) st.selected++;
}

void fm_page_up(BrowserState& st, size_t page_rows) {
    if (st.selected > page_rows) st.selected -= page_rows;
    else st.selected = 0;
}

void fm_page_down(BrowserState& st, size_t page_rows) {
    if (st.entries.empty()) return;
    st.selected = (std::min)(st.selected + page_rows, st.entries.size() - 1);
}

void fm_home(BrowserState& st) { st.selected = 0; }

void fm_end(BrowserState& st) {
    if (!st.entries.empty()) st.selected = st.entries.size() - 1;
}

void fm_ensure_visible(BrowserState& st, size_t visible_rows) {
    if (visible_rows == 0) return;
    if (st.selected < st.scroll) st.scroll = st.selected;
    if (st.selected >= st.scroll + visible_rows) st.scroll = st.selected - visible_rows + 1;
}
