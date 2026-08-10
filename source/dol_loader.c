// SPDX-License-Identifier: GPL-3.0-or-later
#include "smg3ds/dol_loader.h"

#include <stdio.h>
#include <string.h>

enum { TEXT_COUNT = 7, DATA_COUNT = 11, DOL_HEADER_SIZE = 0x100 };

static uint32_t dol_read_be32(const unsigned char* bytes)
{
    return ((uint32_t)bytes[0] << 24) | ((uint32_t)bytes[1] << 16) |
           ((uint32_t)bytes[2] << 8) | bytes[3];
}

static void set_error(char* output, uint32_t size, const char* message)
{
    if (output != NULL && size != 0)
        snprintf(output, size, "%s", message);
}

static unsigned char* resolve_guest(CPUState* cpu, uint32_t address, uint32_t size)
{
    const uint32_t physical = address & 0x3fffffffu;
    if (physical < cpu->ram_size && size <= cpu->ram_size - physical)
        return cpu->ram + physical;
    if (physical >= 0x10000000u) {
        const uint32_t offset = physical - 0x10000000u;
        if (offset < cpu->mem2_size && size <= cpu->mem2_size - offset)
            return cpu->mem2 + offset;
    }
    return NULL;
}

bool smg3ds_load_dol(const char* path, CPUState* cpu, Smg3dsDolInfo* info,
                     char* error, uint32_t error_size)
{
    unsigned char header[DOL_HEADER_SIZE];
    FILE* file = fopen(path, "rb");
    if (file == NULL) {
        set_error(error, error_size, "main.dol is missing from RomFS");
        return false;
    }
    if (fread(header, 1, sizeof(header), file) != sizeof(header)) {
        fclose(file);
        set_error(error, error_size, "main.dol has a truncated header");
        return false;
    }

    Smg3dsDolInfo result = {dol_read_be32(header + 0xe0), 0, 0};
    const uint32_t bss_address = dol_read_be32(header + 0xd8);
    const uint32_t bss_size = dol_read_be32(header + 0xdc);
    if (bss_size != 0) {
        unsigned char* bss = resolve_guest(cpu, bss_address, bss_size);
        if (bss == NULL) {
            fclose(file);
            set_error(error, error_size, "DOL BSS is outside allocated MEM1/MEM2");
            return false;
        }
        /* Clear first: some DOLs describe BSS ranges that overlap loaded sections. */
        memset(bss, 0, bss_size);
    }

    for (uint32_t i = 0; i < TEXT_COUNT + DATA_COUNT; ++i) {
        const uint32_t offset_slot = i;
        const uint32_t address_slot = TEXT_COUNT + DATA_COUNT + i;
        const uint32_t size_slot = (TEXT_COUNT + DATA_COUNT) * 2 + i;
        const uint32_t file_offset = dol_read_be32(header + offset_slot * 4);
        const uint32_t address = dol_read_be32(header + address_slot * 4);
        const uint32_t size = dol_read_be32(header + size_slot * 4);
        if (size == 0)
            continue;
        unsigned char* destination = resolve_guest(cpu, address, size);
        if (destination == NULL) {
            fclose(file);
            set_error(error, error_size, "DOL section is outside allocated MEM1/MEM2");
            return false;
        }
        if (fseek(file, (long)file_offset, SEEK_SET) != 0 ||
            fread(destination, 1, size, file) != size) {
            fclose(file);
            set_error(error, error_size, "DOL section is truncated");
            return false;
        }
        ++result.loaded_sections;
        result.loaded_bytes += size;
    }

    fclose(file);
    cpu->pc = result.entry_point;
    if (info != NULL)
        *info = result;
    set_error(error, error_size, "ok");
    return true;
}
