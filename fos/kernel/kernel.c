/*
 * kernel.c — Kernel entry after bootloader handoff.
 *
 * Boot flow:
 *   1. Console + blue boot screen
 *   2. CPU info, keyboard, FOS API setup
 *   3. VFS mount, read \SYSTEM.INI for keyboard layout
 *   4. Disk/volume report (stays on screen)
 *   5. Shell on black background
 *
 * Boot messages use boot_line() so they stay visible after the shell starts.
 */

#include "console.h"
#include "keyboard.h"
#include "vfs.h"
#include "shell.h"
#include "cpu.h"
#include "boot_report.h"
#include "fos_api.h"
#include "config.h"
#include "video.h"
#include "timer.h"
#include "rtc.h"
#include "memory.h"
#include "sb16.h"

void kmain(void) {
    /* --- Phase 1: console and CPU --- */
    console_init();
    console_clear_color(15, 1);

    console_write_line_color(1, 14, "FOS - Flash Operating System");
    boot_line("============================");
    boot_line("");

    timer_init();
    memory_init();
    {
        extern char _end[];
        memory_set_kernel_size((uint64_t)(uintptr_t)_end - 0x100000ULL);
    }
    boot_line("[boot] CPU");
    console_set_color(15, 1);
    cpu_print_info();
    boot_line("[boot] PIT 1000 Hz (IRQ0)");
    boot_line("[boot] RTC");
    console_set_color(15, 1);
    rtc_print();
    boot_line("");

    /* Keyboard HW init; layout comes from SYSTEM.INI after VFS is up. */
    keyboard_init();
    fos_api_init();
    boot_line("[boot] Input ready (PS/2 keyboard + COM1 serial)");
    boot_line("");

    /* --- Phase 2: disks and config --- */
    boot_line("[boot] Probing disks and mounting volumes...");
    vfs_init();

    /* \SYSTEM.INI on drive 0: — see fos/system.ini in the repo. */
    config_init(0);
    {
        const char *layout = config_get("keyboard", "layout");
        if (layout && layout[0]) {
            keyboard_set_layout(layout);
        }
    }
    video_init_from_config();
    if (video_is_framebuffer()) {
        console_clear_color(15, 1);
    }
    sb16_init();
    boot_line("");

    console_set_color(15, 1);
    boot_print_ata_disks();
    boot_line("");
    boot_print_drive_table();
    boot_line("");

    boot_print_logo();
    boot_line("[boot] System ready - starting shell.");
    boot_line("");

    /* Switch to black background for the interactive shell. */
    console_set_color(15, 0);
    shell_run();
}
