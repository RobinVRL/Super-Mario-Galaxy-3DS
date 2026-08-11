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
    uint32_t xfb_copies;
    uint32_t unknown_commands;
    uint32_t display_lists;
    uint32_t vertices;
    uint32_t triangles;
    uint32_t rasterized_pixels;
    uint32_t colored_pixels;
    uint32_t xfb_colored_pixels;
    uint32_t color_write_draws;
    uint32_t textured_draws;
    uint32_t texture_decodes;
    uint32_t texture_samples;
    uint32_t unsupported_texture_formats;
    uint32_t last_texture_width;
    uint32_t last_texture_height;
    uint32_t last_texture_storage_width;
    uint32_t last_texture_storage_height;
    uint32_t last_texture_format;
    uint32_t last_texture_base;
    uint32_t depth_rejected_pixels;
    uint32_t last_z_mode;
    uint32_t last_blend_mode;
    uint32_t last_vertex_color;
    uint32_t last_texture_image0;
    uint32_t last_texture_image3;
    uint32_t last_gen_mode;
    uint32_t last_tev_order;
    uint32_t last_tev_color_env;
    uint32_t last_tev_alpha_env;
    uint32_t textured_gen_mode;
    uint32_t textured_tev_order;
    uint32_t textured_tev_color_env;
    uint32_t textured_tev_alpha_env;
    uint32_t clipped_vertices;
    uint32_t decode_failures;
    uint32_t incomplete_commands;
    bool geometry_self_test_passed;
    bool video_started;
} Smg3dsGxStats;

void smg3ds_gx_init(CPUState* cpu);
void smg3ds_gx_fifo_write(uint64_t value, uint8_t size);
bool smg3ds_gx_take_finish_request(void);
void smg3ds_gx_present_top(void);
const Smg3dsGxStats* smg3ds_gx_get_stats(void);

