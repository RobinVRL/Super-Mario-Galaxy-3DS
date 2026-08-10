// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "cpu/cpu.h"

#include <stdbool.h>
#include <stdint.h>

typedef struct Smg3dsDolInfo {
    uint32_t entry_point;
    uint32_t loaded_sections;
    uint32_t loaded_bytes;
} Smg3dsDolInfo;

bool smg3ds_load_dol(const char* path, CPUState* cpu, Smg3dsDolInfo* info,
                     char* error, uint32_t error_size);

