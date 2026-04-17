#pragma once

#include <string>
#include <vector>

struct Entry {
    std::wstring name;
    bool is_dir = false;
    bool is_parent = false;
};

struct BrowserState {
    std::wstring cwd;
    std::vector<Entry> entries;
    size_t selected = 0;
    size_t scroll = 0;
    bool cwd_writable = false;
};

bool fm_load_directory(BrowserState& st, const std::wstring& path, std::wstring& err);
bool fm_open_selected(BrowserState& st, std::wstring& err);
void fm_move_selection_up(BrowserState& st);
void fm_move_selection_down(BrowserState& st);
void fm_page_up(BrowserState& st, size_t page_rows);
void fm_page_down(BrowserState& st, size_t page_rows);
void fm_home(BrowserState& st);
void fm_end(BrowserState& st);
const Entry* fm_selected_entry(const BrowserState& st);
std::wstring fm_selected_full_path(const BrowserState& st);
void fm_ensure_visible(BrowserState& st, size_t visible_rows);
