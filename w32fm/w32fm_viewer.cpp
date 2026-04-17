#include "w32fm_viewer.h"

#include "w32fm_fs.h"
#include "w32fm_ui.h"

#include <windows.h>

#include <vector>

namespace {
constexpr size_t kMaxViewBytes = 4u * 1024u * 1024u;
}

void fm_view_file(const std::wstring& path, std::wstring& status) {
    std::string raw;
    std::wstring err;
    if (!fm_read_file_limited(path, kMaxViewBytes, raw, err)) {
        status = L"view failed: " + err;
        return;
    }

    std::vector<size_t> starts{0};
    for (size_t i = 0; i < raw.size(); ++i) {
        if (raw[i] == '\n') starts.push_back(i + 1);
    }
    if (starts.empty()) starts.push_back(0);

    size_t scroll = 0;
    for (;;) {
        UiLayout l = fm_ui_layout();
        size_t rows = static_cast<size_t>(l.list_rows > 0 ? l.list_rows : 1);
        if (scroll + rows > starts.size() && starts.size() > rows) scroll = starts.size() - rows;

        BrowserState fake{};
        fake.cwd = L"View: " + path;
        fake.cwd_writable = true;
        fm_ui_draw(fake, L"q close  Up/Dn Pg Home/End");

        HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
        for (size_t r = 0; r < rows; ++r) {
            size_t li = scroll + r;
            std::wstring line;
            if (li < starts.size()) {
                size_t begin = starts[li];
                size_t end = (li + 1 < starts.size()) ? starts[li + 1] : raw.size();
                while (end > begin && (raw[end - 1] == '\n' || raw[end - 1] == '\r')) --end;
                for (size_t i = begin; i < end; ++i) {
                    unsigned char c = static_cast<unsigned char>(raw[i]);
                    line.push_back((c >= 32 && c < 127) ? static_cast<wchar_t>(c) : L'.');
                }
            }
            if (line.size() > static_cast<size_t>(l.cols)) line.resize(static_cast<size_t>(l.cols));
            if (line.size() < static_cast<size_t>(l.cols)) line.append(static_cast<size_t>(l.cols) - line.size(), L' ');
            COORD p{0, static_cast<short>(l.list_top + static_cast<short>(r))};
            DWORD written = 0;
            WriteConsoleOutputCharacterW(hOut, line.c_str(), static_cast<DWORD>(line.size()), p, &written);
        }

        int k = fm_ui_read_key();
        if (k == 'q' || k == 'Q' || k == 27 || k == FMK_F10) {
            status = L"";
            return;
        }
        if (k == FMK_UP) {
            if (scroll > 0) --scroll;
        } else if (k == FMK_DOWN) {
            if (scroll + 1 < starts.size()) ++scroll;
        } else if (k == FMK_PGUP) {
            if (scroll > rows) scroll -= rows;
            else scroll = 0;
        } else if (k == FMK_PGDN) {
            scroll += rows;
            if (scroll >= starts.size()) scroll = starts.empty() ? 0 : starts.size() - 1;
        } else if (k == FMK_HOME) {
            scroll = 0;
        } else if (k == FMK_END) {
            if (!starts.empty()) scroll = starts.size() - 1;
        }
    }
}
