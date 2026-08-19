/*
 * kernel.c — Kernel entry after bootloader handoff.
 * Boot messages use white-on-blue per line (stays on screen after shell starts).
 */

#include "console.h"
#include "keyboard.h"
#include "vfs.h"
#include "shell.h"
#include "cpu.h"
#include "boot_report.h"
#include "fos_api.h"

void kmain(void) {
    console_init();
    console_clear_color(15, 1);

    boot_line("FOS — Flash Operating System");
    boot_line("============================");
    boot_line("");

    boot_line("[boot] CPU");
    console_set_color(15, 1);
    cpu_print_info();
    boot_line("");

    keyboard_init();
    fos_api_init();
    boot_line("[boot] Input ready (PS/2 keyboard + COM1 serial)");
    boot_line("");

    boot_line("[boot] Probing disks and mounting volumes...");
    vfs_init();
    boot_line("");

    console_set_color(15, 1);
    boot_print_ata_disks();
    boot_line("");
    boot_print_drive_table();
    boot_line("");

    boot_line("[boot] System ready — starting shell.");
    boot_line("");

    console_set_color(15, 0);
    shell_run();
}
