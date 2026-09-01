#include "wm.h"

int main()
{
    WM wm;
    if (!wm.init())
        return 1;
    wm.run();
    wm.finish();
    return 0;
}
