// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <3ds.h>

#include "cpu/cpu.h"

#include <stdbool.h>
#include <stdint.h>

typedef struct Smg3dsGxStats {
    uint32_t fifo_bytes;
    uint32_t fifo_writes;
    uint32_t fifo_overflows;
    uint32_t commands;
    uint32_t bp_writes;
    uint32_t draw_calls;
    uint32_t efb_copies;
    uint32_t unknown_commands;
    uint32_t display_lists;
    uint32_t vertices;
    uint32_t triangles;
    uint32_t rasterized_pixels;
    uint32_t clipped_vertices;
    uint32_t decode_failures;
    uint32_t incomplete_commands;
    bool geometry_self_test_passed;
    bool video_started;
} Smg3dsGxStats;

void smg3ds_gx_init(CPUState* cpu);
void smg3ds_gx_fifo_write(uint64_t value, uint8_t size);
void smg3ds_gx_present_top(void);
const Smg3dsGxStats* smg3ds_gx_get_stats(void);

