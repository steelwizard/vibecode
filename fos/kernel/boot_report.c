/*
 * boot_report.c — Hardware/volume reports shown during boot (blue screen).
 *
 * Uses boot_line() / console_set_color(15,1) so output stays on the blue
 * boot background after the shell starts on black.
 */

#include "boot_report.h"
#include "block.h"
#include "vfs.h"
#include "console.h"
#include "string.h"

void boot_print_ata_disks(void) {
    int n = block_count();
    boot_line("  ATA disks");
    boot_line("  Phys  Bus:Dev  Size        Model");
    boot_line("  ----  ------  ----        -----");

    for (int i = 0; i < n; i++) {
        const block_dev_t *d = block_device(i);
        int logical = block_logical_for_phys(i);
        console_set_color(15, 1);
        console_write("  ");
        console_putchar((char)('0' + i));
        if (logical == 0) {
            console_write("* ");  /* marks the disk we booted from -> drive 0: */
        } else {
            console_write("  ");
        }
        console_putchar('0' + d->bus);
        console_write(":");
        console_putchar('0' + d->drive);
        console_write("    ");
        if (d->sector_count > 0) {
            console_write_size(d->sector_count * BLOCK_SECTOR_SIZE);
        } else {
            console_write("-");
        }
        console_write("    ");
        if (d->model[0]) {
            console_write_line(d->model);
        } else {
            console_write_line("(unknown)");
        }
    }
    boot_line("  (* = boot disk, mapped to drive 0:)");
}

void boot_print_drive_table(void) {
    console_set_color(15, 1);
    vfs_print_drive_table();
}

static void boot_print_color_strip(void) {
    int c;

    for (c = 0; c < 16; c++) {
        /* 13×5 + 3×4 = 77 — stay under 80 cols to avoid wrap + extra newline */
        console_write_color(15, (uint8_t)c, (c < 13) ? "     " : "    ");
    }
    console_write_color(15, 4, "\n");
}

void boot_print_logo(void) {
    /* White on dark red (VGA fg=15, bg=4). Block letters — F has no bottom bar. */
    console_write_line_color(15, 4, "");
    console_write_line_color(15, 4, "  #####    #####    ##### ");
    console_write_line_color(15, 4, "  #        #   #    #     ");
    console_write_line_color(15, 4, "  ####     #   #     #### ");
    console_write_line_color(15, 4, "  #        #   #        # ");
    console_write_line_color(15, 4, "  #        #####    ##### ");
    boot_print_color_strip();
    console_write_line_color(15, 4, "      Flash Operating System");
    console_write_line_color(15, 4, "");
}
