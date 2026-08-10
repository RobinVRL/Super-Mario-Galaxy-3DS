// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef SMG3DS_EXI_HLE_H
#define SMG3DS_EXI_HLE_H

#include <stdbool.h>

#include <3ds.h>

#include "cpu/cpu.h"

typedef struct Smg3dsExiStats {
    bool initialized;
    bool self_test_passed;
    u32 mmio_reads;
    u32 mmio_writes;
    u32 immediate_transfers;
    u32 dma_transfers;
    u32 transferred_bytes;
    u32 transfer_failures;
    u32 stalled_transfers;
    u32 ipl_commands;
    u32 last_command;
} Smg3dsExiStats;

void smg3ds_exi_init(void);

/*
 * Runs deterministic register, SRAM, RTC, W1C, interrupt, and DMA checks.
 * The complete EXI state and the touched guest bytes are restored afterward.
 */
bool smg3ds_exi_self_test(CPUState *cpu);

/* Accepts the physical 0x0c/0x0d mirrors after the PPC alias is stripped. */
bool smg3ds_exi_handles(u32 physical, u8 size);
u64 smg3ds_exi_read(CPUState *cpu, u32 physical, u8 size);
void smg3ds_exi_write(CPUState *cpu,
                      u32 physical,
                      u64 value,
                      u8 size);

bool smg3ds_exi_interrupt_pending(void);
const Smg3dsExiStats *smg3ds_exi_get_stats(void);

#endif
