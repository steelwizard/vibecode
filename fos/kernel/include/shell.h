#pragma once

void shell_run(void);

/* Run a .BAT at an explicit path (no $PATH search). 0 = ran, -1 = missing. */
int shell_run_bat(const char *path, const char *args);
