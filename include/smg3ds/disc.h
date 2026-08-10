// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef SMG3DS_DISC_H
#define SMG3DS_DISC_H

#include <stdbool.h>
#include <stdint.h>

#include <3ds.h>

#include "cpu/cpu.h"

typedef struct Smg3dsDiscStats {
    bool initialized;
    u32 file_count;
    u32 read_requests;
    u32 read_failures;
    u64 bytes_read;
    u64 last_offset;
    u32 last_length;
} Smg3dsDiscStats;

/*
 * Loads the Wii disc ID and FST, indexes the extracted files, and publishes
 * the disc metadata in the guest low-memory fields used by the SDK.
 */
bool smg3ds_disc_init(CPUState *cpu);

/* Republishes boot metadata and the SDK FST cache after the DOL clears BSS. */
bool smg3ds_disc_publish_guest_state(CPUState *cpu);

/* Resolves a guest path against the validated host copy of the disc FST. */
bool smg3ds_disc_resolve_path(CPUState *cpu,
                              u32 guest_path_address,
                              s32 *entry_index);

/* Resolves a path and initializes the SDK DVDFileInfo fields for a file. */
bool smg3ds_disc_open_path(CPUState *cpu,
                           u32 guest_path_address,
                           u32 guest_file_info_address,
                           bool *opened);

/* Initializes DVDFileInfo directly from a validated FST entry index. */
bool smg3ds_disc_open_entry(CPUState *cpu,
                            s32 entry_index,
                            u32 guest_file_info_address,
                            bool *opened);

void smg3ds_disc_shutdown(void);

/* Copies up to the 32-byte disc ID to guest memory. */
bool smg3ds_disc_read_id(CPUState *cpu, u32 guest_address, u32 length);

/*
 * Reads a decrypted, partition-relative disc byte range into guest memory.
 * The whole range must be covered by indexed FST files; padding and other
 * unmapped disc areas are rejected instead of silently returning zeroes.
 */
bool smg3ds_disc_read(CPUState *cpu,
                      u32 guest_address,
                      u64 byte_offset,
                      u32 length);

bool smg3ds_disc_is_ready(void);

const Smg3dsDiscStats *smg3ds_disc_get_stats(void);

const char *smg3ds_disc_last_error(void);

#endif
