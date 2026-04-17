#pragma once

#include "w32fm_model.h"

#include <string>

struct UiLayout {
    short cols = 80;
    short rows = 25;
    short header_y = 0;
    short list_top = 2;
    short list_bottom = 22;
    short status_y = 24;
    short list_rows = 21;
};

enum {
    FMK_UP = 1001,
    FMK_DOWN,
    FMK_PGUP,
    FMK_PGDN,
    FMK_HOME,
    FMK_END,
    FMK_RESIZE,
    FMK_F10
};

bool fm_ui_init();
void fm_ui_shutdown();
UiLayout fm_ui_layout();
void fm_ui_draw(const BrowserState& st, const std::wstring& status);
bool fm_ui_prompt_text(const std::wstring& title, const std::wstring& prompt, std::wstring& out);
bool fm_ui_confirm(const std::wstring& title, const std::wstring& msg);
int fm_ui_read_key();
