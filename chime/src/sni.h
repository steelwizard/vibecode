#pragma once

#include "wm.h"

bool sni_start(WM *wm);
void sni_stop(WM *wm);
void sni_dispatch(WM *wm);
int sni_fd(WM *wm); // -1 if unused
bool sni_handle_click(WM *wm, const XButtonEvent *b);
