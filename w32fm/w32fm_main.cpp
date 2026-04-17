#define UNICODE
#define _UNICODE

#include "w32fm_fs.h"
#include "w32fm_model.h"
#include "w32fm_ui.h"
#include "w32fm_viewer.h"

#include <windows.h>

namespace {
void set_status(std::wstring& status, const std::wstring& s) { status = s; }
}

int wmain(int argc, wchar_t** argv) {
    if (!fm_ui_init()) return 1;

    BrowserState st{};
    std::wstring status;
    std::wstring err;

    std::wstring start = (argc > 1) ? argv[1] : L".";
    if (!fm_load_directory(st, start, err)) {
        fm_ui_shutdown();
        return 1;
    }

    bool running = true;
    while (running) {
        err.clear();
        UiLayout l = fm_ui_layout();
        fm_ensure_visible(st, static_cast<size_t>(l.list_rows));
        fm_ui_draw(st, status);

        int k = fm_ui_read_key();
        switch (k) {
            case FMK_UP: fm_move_selection_up(st); break;
            case FMK_DOWN: fm_move_selection_down(st); break;
            case FMK_PGUP: fm_page_up(st, static_cast<size_t>(l.list_rows)); break;
            case FMK_PGDN: fm_page_down(st, static_cast<size_t>(l.list_rows)); break;
            case FMK_HOME: fm_home(st); break;
            case FMK_END: fm_end(st); break;
            case FMK_RESIZE: break;
            case FMK_F10:
            case 'q':
            case 'Q':
                running = false;
                break;
            case 13: {
                if (!fm_open_selected(st, err) && !err.empty()) set_status(status, err);
                break;
            }
            case 'v':
            case 'V': {
                const Entry* e = fm_selected_entry(st);
                if (!e || e->is_dir || e->is_parent) {
                    set_status(status, L"Select a regular file.");
                } else {
                    fm_view_file(fm_selected_full_path(st), status);
                }
                break;
            }
            case 'e':
            case 'E': {
                const Entry* e = fm_selected_entry(st);
                if (!e || e->is_dir || e->is_parent) {
                    set_status(status, L"Select a regular file.");
                    break;
                }
                std::wstring p = fm_selected_full_path(st);
                if (fm_launch_editor(p, err)) set_status(status, L"Launched editor.");
                else set_status(status, L"edit failed: " + err);
                break;
            }
            case 'd':
            case 'D': {
                const Entry* e = fm_selected_entry(st);
                if (!e || e->is_dir || e->is_parent) {
                    set_status(status, L"Select a regular file.");
                    break;
                }
                if (!fm_ui_confirm(L"Delete", L"Delete " + e->name + L"?")) break;
                std::wstring p = fm_selected_full_path(st);
                if (!fm_delete_file(p, err)) set_status(status, L"delete failed: " + err);
                else {
                    if (fm_load_directory(st, st.cwd, err)) set_status(status, L"Deleted.");
                    else set_status(status, L"reload failed: " + err);
                }
                break;
            }
            case 'c':
            case 'C':
            case 'm':
            case 'M': {
                bool is_move = (k == 'm' || k == 'M');
                const Entry* e = fm_selected_entry(st);
                if (!e || e->is_dir || e->is_parent) {
                    set_status(status, L"Select a regular file.");
                    break;
                }
                std::wstring name;
                if (!fm_ui_prompt_text(is_move ? L"Move" : L"Copy", L"Destination filename", name)) break;
                std::wstring why;
                if (!fm_is_valid_filename(name, why)) {
                    set_status(status, why);
                    break;
                }
                std::wstring src = fm_selected_full_path(st);
                std::wstring dst = fm_join_path(st.cwd, name);
                bool ok = is_move ? fm_move_file(src, dst, err) : fm_copy_file(src, dst, err);
                if (!ok) set_status(status, (is_move ? L"move failed: " : L"copy failed: ") + err);
                else {
                    if (fm_load_directory(st, st.cwd, err)) set_status(status, is_move ? L"Moved." : L"Copied.");
                    else set_status(status, L"reload failed: " + err);
                }
                break;
            }
            case 'n':
            case 'N': {
                std::wstring name;
                if (!fm_ui_prompt_text(L"New file", L"Filename", name)) break;
                std::wstring why;
                if (!fm_is_valid_filename(name, why)) {
                    set_status(status, why);
                    break;
                }
                std::wstring p = fm_join_path(st.cwd, name);
                if (!fm_create_empty_file(p, err)) set_status(status, L"create failed: " + err);
                else {
                    if (fm_load_directory(st, st.cwd, err)) set_status(status, L"Created.");
                    else set_status(status, L"reload failed: " + err);
                }
                break;
            }
            default:
                break;
        }
    }

    fm_ui_shutdown();
    return 0;
}
