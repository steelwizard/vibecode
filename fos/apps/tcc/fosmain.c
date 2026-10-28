#include "setjmp.h"

extern int main(int argc, char **argv);
extern int fos_make_argv(const char *argv0, char **argv, int max);

extern jmp_buf fos_exit_buf;
extern int fos_exit_ready;

void com_main(void) {
    char *argv[40];
    int argc;

    if (setjmp(fos_exit_buf) == 0) {
        fos_exit_ready = 1;
        argc = fos_make_argv("tcc", argv, 40);
        main(argc, argv);
    }
}
