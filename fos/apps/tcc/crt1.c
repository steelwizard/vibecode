#include "setjmp.h"

extern int main(int argc, char **argv);
extern void exit(int status);
extern int fos_make_argv(const char *argv0, char **argv, int max);

extern jmp_buf fos_exit_buf;
extern int fos_exit_ready;

void _start(void) {
    char *argv[32];
    int argc;

    if (setjmp(fos_exit_buf) == 0) {
        fos_exit_ready = 1;
        argc = fos_make_argv("com", argv, 32);
        exit(main(argc, argv));
    }
}
