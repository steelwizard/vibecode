#pragma once

#include "types.h"

typedef struct {
    uint32_t width;
    uint32_t height;
    uint32_t pitch;
    uint32_t bpp;
    uint64_t fb_addr;
} video_mode_t;

/* Read [video] mode from SYSTEM.INI (call after config_init).
 * Supports text, named presets (480p … 1080p), and custom WxH. */
int video_init_from_config(void);

int video_is_framebuffer(void);
const video_mode_t *video_current_mode(void);
