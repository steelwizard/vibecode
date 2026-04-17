#define UNICODE
#define _UNICODE

#include "w32fm_ui.h"

#include <windows.h>

#include <algorithm>
#include <iterator>
#include <vector>

namespace {
HANDLE g_out = INVALID_HANDLE_VALUE;
HANDLE g_in = INVALID_HANDLE_VALUE;
DWORD g_prev_mode = 0;
WORD g_default_attr = static_cast<WORD>(FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE);

WORD attr_normal(bool writable) {
    (void)writable;
    return static_cast<WORD>(FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE);
}
WORD attr_header(bool writable) {
    (void)writable;
    return static_cast<WORD>(FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_INTENSITY);
}
WORD attr_dir(bool writable) {
    (void)writable;
    return static_cast<WORD>(FOREGROUND_GREEN | FOREGROUND_INTENSITY);
}
WORD attr_file(bool writable) {
    (void)writable;
    return static_cast<WORD>(FOREGROUND_RED | FOREGROUND_BLUE | FOREGROUND_INTENSITY);
}
WORD attr_selected() {
    return static_cast<WORD>(FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE | FOREGROUND_INTENSITY);
}

void write_line(short y, WORD attr, const std::wstring& text, short width) {
    (void)attr;
    if (width < 1) return;
    std::wstring line = text;
    if (line.size() > static_cast<size_t>(width)) line.resize(static_cast<size_t>(width));
    if (line.size() < static_cast<size_t>(width)) line.append(static_cast<size_t>(width) - line.size(), L' ');
    COORD pos{0, y};
    DWORD written = 0;
    WriteConsoleOutputCharacterW(g_out, line.c_str(), static_cast<DWORD>(line.size()), pos, &written);
}

std::wstring trim_to_width(const std::wstring& s, size_t w) {
    if (s.size() <= w) return s;
    if (w <= 3) return s.substr(0, w);
    return s.substr(0, w - 3) + L"...";
}
}

bool fm_ui_init() {
    g_out = GetStdHandle(STD_OUTPUT_HANDLE);
    g_in = GetStdHandle(STD_INPUT_HANDLE);
    if (g_out == INVALID_HANDLE_VALUE || g_in == INVALID_HANDLE_VALUE) return false;
    CONSOLE_SCREEN_BUFFER_INFO csbi{};
    if (GetConsoleScreenBufferInfo(g_out, &csbi)) g_default_attr = csbi.wAttributes;
    if (!GetConsoleMode(g_in, &g_prev_mode)) return false;
    DWORD mode = ENABLE_WINDOW_INPUT | ENABLE_PROCESSED_INPUT | ENABLE_EXTENDED_FLAGS;
    if (!SetConsoleMode(g_in, mode)) return false;
    return true;
}

void fm_ui_shutdown() {
    if (g_in != INVALID_HANDLE_VALUE && g_prev_mode != 0) SetConsoleMode(g_in, g_prev_mode);
}

UiLayout fm_ui_layout() {
    CONSOLE_SCREEN_BUFFER_INFO csbi{};
    if (!GetConsoleScreenBufferInfo(g_out, &csbi)) {
        UiLayout fallback{};
        return fallback;
    }
    short cols = static_cast<short>(csbi.srWindow.Right - csbi.srWindow.Left + 1);
    short rows = static_cast<short>(csbi.srWindow.Bottom - csbi.srWindow.Top + 1);
    UiLayout l{};
    l.cols = std::max<short>(cols, 20);
    l.rows = std::max<short>(rows, 8);
    l.header_y = 0;
    l.list_top = 2;
    l.status_y = static_cast<short>(l.rows - 1);
    l.list_bottom = static_cast<short>(l.rows - 2);
    l.list_rows = std::max<short>(1, static_cast<short>(l.list_bottom - l.list_top + 1));
    return l;
}

void fm_ui_draw(const BrowserState& st, const std::wstring& status) {
    UiLayout l = fm_ui_layout();
    DWORD cells = static_cast<DWORD>(l.cols) * static_cast<DWORD>(l.rows);
    COORD origin{0, 0};
    DWORD written = 0;
    FillConsoleOutputAttribute(g_out, g_default_attr, cells, origin, &written);
    WORD bg = attr_normal(st.cwd_writable);
    WORD hdr = attr_header(st.cwd_writable);
    write_line(0, hdr, L" w32fm  " + trim_to_width(st.cwd, static_cast<size_t>(l.cols - 7)), l.cols);
    write_line(1, bg, L" Arrows/Pg/Home/End Enter c/m/d/v/e/n q/F10", l.cols);

    for (short r = 0; r < l.list_rows; ++r) {
        size_t idx = st.scroll + static_cast<size_t>(r);
        std::wstring line;
        WORD a = bg;
        if (idx < st.entries.size()) {
            const Entry& e = st.entries[idx];
            line = (idx == st.selected) ? L"> " : L"  ";
            line += e.name;
            if (e.is_dir && !e.is_parent) line += L"\\";
            if (idx == st.selected) a = attr_selected();
            else if (e.is_parent || e.is_dir) a = attr_dir(st.cwd_writable);
            else a = attr_file(st.cwd_writable);
        }
        write_line(static_cast<short>(l.list_top + r), a, trim_to_width(line, static_cast<size_t>(l.cols)), l.cols);
    }
    write_line(l.status_y, bg, trim_to_width(status, static_cast<size_t>(l.cols)), l.cols);
}

bool fm_ui_prompt_text(const std::wstring& title, const std::wstring& prompt, std::wstring& out) {
    out.clear();
    UiLayout l = fm_ui_layout();
    WORD bg = FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE;
    write_line(static_cast<short>(l.rows - 3), bg, trim_to_width(title + L": " + prompt, static_cast<size_t>(l.cols)), l.cols);
    write_line(static_cast<short>(l.rows - 2), bg, L"> ", l.cols);

    COORD pos{2, static_cast<short>(l.rows - 2)};
    SetConsoleCursorPosition(g_out, pos);
    DWORD old = 0;
    GetConsoleMode(g_in, &old);
    SetConsoleMode(g_in, ENABLE_PROCESSED_INPUT | ENABLE_LINE_INPUT | ENABLE_ECHO_INPUT);
    wchar_t buf[512]{};
    DWORD read = 0;
    BOOL ok = ReadConsoleW(g_in, buf, static_cast<DWORD>(std::size(buf) - 1), &read, nullptr);
    SetConsoleMode(g_in, old);
    if (!ok || read == 0) return false;
    out.assign(buf);
    while (!out.empty() && (out.back() == L'\r' || out.back() == L'\n')) out.pop_back();
    return true;
}

bool fm_ui_confirm(const std::wstring& title, const std::wstring& msg) {
    UiLayout l = fm_ui_layout();
    WORD a = FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_INTENSITY;
    write_line(static_cast<short>(l.rows - 3), a, trim_to_width(title, static_cast<size_t>(l.cols)), l.cols);
    write_line(static_cast<short>(l.rows - 2), a, trim_to_width(msg + L" [y/n]", static_cast<size_t>(l.cols)), l.cols);
    for (;;) {
        int k = fm_ui_read_key();
        if (k == 'y' || k == 'Y') return true;
        if (k == 'n' || k == 'N' || k == 27) return false;
    }
}

int fm_ui_read_key() {
    INPUT_RECORD rec{};
    DWORD n = 0;
    for (;;) {
        if (!ReadConsoleInputW(g_in, &rec, 1, &n)) return 0;
        if (rec.EventType == WINDOW_BUFFER_SIZE_EVENT) return FMK_RESIZE;
        if (rec.EventType != KEY_EVENT) continue;
        KEY_EVENT_RECORD k = rec.Event.KeyEvent;
        if (!k.bKeyDown) continue;
        switch (k.wVirtualKeyCode) {
            case VK_UP: return FMK_UP;
            case VK_DOWN: return FMK_DOWN;
            case VK_PRIOR: return FMK_PGUP;
            case VK_NEXT: return FMK_PGDN;
            case VK_HOME: return FMK_HOME;
            case VK_END: return FMK_END;
            case VK_RETURN: return 13;
            case VK_F10: return FMK_F10;
            default: break;
        }
        if (k.uChar.UnicodeChar != 0) return static_cast<int>(k.uChar.UnicodeChar);
    }
}
