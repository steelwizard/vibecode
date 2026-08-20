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

static void boot_print_color_strip(int width) {
    int i;
    int run;

    if (width < 16) {
        width = 16;
    }
    run = width / 16;
    if (run < 1) {
        run = 1;
    }
    for (i = 0; i < 16; i++) {
        int n;
        for (n = 0; n < run; n++) {
            console_write_color(15, (uint8_t)i, " ");
        }
    }
    console_write_color(15, 4, "\n");
}

void boot_print_logo(void) {
    /* 5×5 bitmaps (MSB = left). F has no bottom bar. */
    static const uint8_t glyphs[3][5] = {
        { 0x1F, 0x10, 0x1E, 0x10, 0x10 },
        { 0x1F, 0x11, 0x11, 0x11, 0x1F },
        { 0x1F, 0x10, 0x0F, 0x01, 0x1F }
    };
    const char *tag = "Flash Operating System";
    char line[256];
    int cols = 80;
    int rows = 25;
    int scale;
    int gap;
    int width;
    int indent;
    int row;
    int rep;
    int i;
    int n;

    console_get_size(&cols, &rows);
    /* About half the old splash: 3× on 720p, 2× on VGA 80×25. */
    scale = (rows - 12) / 10;
    if (scale < 2) {
        scale = 2;
    }
    if (scale > 3) {
        scale = 3;
    }
    gap = scale;
    width = 15 * scale + 2 * gap;
    while (width + 2 > cols && scale > 1) {
        scale--;
        gap = scale;
        width = 15 * scale + 2 * gap;
    }
    indent = (cols - width) / 2;
    if (indent < 0) {
        indent = 0;
    }

    console_write_line_color(15, 4, "");
    for (row = 0; row < 5; row++) {
        for (rep = 0; rep < scale; rep++) {
            n = 0;
            for (i = 0; i < indent && n < (int)sizeof(line) - 2; i++) {
                line[n++] = ' ';
            }
            for (i = 0; i < 3; i++) {
                int bit;
                if (i > 0) {
                    int g;
                    for (g = 0; g < gap && n < (int)sizeof(line) - 2; g++) {
                        line[n++] = ' ';
                    }
                }
                for (bit = 4; bit >= 0; bit--) {
                    char ch = (glyphs[i][row] & (uint8_t)(1u << bit)) ? (char)0xDB : ' ';
                    int s;
                    for (s = 0; s < scale && n < (int)sizeof(line) - 2; s++) {
                        line[n++] = ch;
                    }
                }
            }
            line[n] = 0;
            console_write_line_color(15, 4, line);
        }
    }
    n = 0;
    for (i = 0; i < indent && n < (int)sizeof(line) - 2; i++) {
        line[n++] = ' ';
    }
    line[n] = 0;
    console_write_color(15, 4, line);
    boot_print_color_strip(width);
    n = 0;
    {
        int tag_len = (int)strlen(tag);
        int pad = indent + (width - tag_len) / 2;
        if (pad < 0) {
            pad = 0;
        }
        for (i = 0; i < pad && n < (int)sizeof(line) - 2; i++) {
            line[n++] = ' ';
        }
    }
    for (i = 0; tag[i] && n < (int)sizeof(line) - 2; i++) {
        line[n++] = tag[i];
    }
    line[n] = 0;
    console_write_line_color(15, 4, line);
    console_write_line_color(15, 4, "");
}
