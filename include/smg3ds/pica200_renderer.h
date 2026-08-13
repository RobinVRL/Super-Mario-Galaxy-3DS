// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef struct Smg3dsPica200Stats {
    uint32_t presented_frames;
    uint32_t texture_uploads;
    uint32_t frame_failures;
    uint64_t uploaded_bytes;
    bool available;
} Smg3dsPica200Stats;

/* gfxInitDefault() must have completed before this is called. */
bool smg3ds_pica200_init(void);
void smg3ds_pica200_shutdown(void);

/* Pixels are row-major and use the GX renderer's 0xRRGGBBAA packing. */
bool smg3ds_pica200_present(const uint32_t* pixels,
                            uint16_t width, uint16_t height,
                            uint32_t clear_rgba);

const Smg3dsPica200Stats* smg3ds_pica200_get_stats(void);
