// SPDX-License-Identifier: GPL-3.0-or-later
#include "smg3ds/exi_hle.h"

#include <string.h>
#include <time.h>

enum {
    SMG3DS_EXI_CHANNELS = 3,
    SMG3DS_EXI_REGISTERS = 5,
    SMG3DS_EXI_CHANNEL_SIZE = 0x14,
    SMG3DS_EXI_SIZE = SMG3DS_EXI_CHANNELS * SMG3DS_EXI_CHANNEL_SIZE,
    SMG3DS_EXI_BASE_GC = 0x0c006800,
    SMG3DS_EXI_BASE_WII = 0x0d006800,
    SMG3DS_EXI_MAX_DMA = 0x200000,
    SMG3DS_EXI_MEM2_BASE = 0x10000000,
    SMG3DS_EXI_MEM2_SIZE = 0x04000000,
    SMG3DS_EXI_GC_EPOCH = 946684800,

    SMG3DS_EXI_STATUS = 0,
    SMG3DS_EXI_DMA_ADDRESS = 1,
    SMG3DS_EXI_DMA_LENGTH = 2,
    SMG3DS_EXI_CONTROL = 3,
    SMG3DS_EXI_IMMEDIATE = 4,

    SMG3DS_EXI_EXIINTMASK = 0x0001,
    SMG3DS_EXI_EXIINT = 0x0002,
    SMG3DS_EXI_TCINTMASK = 0x0004,
    SMG3DS_EXI_TCINT = 0x0008,
    SMG3DS_EXI_CLOCK_MASK = 0x0070,
    SMG3DS_EXI_CHIP_SELECT_MASK = 0x0380,
    SMG3DS_EXI_EXTINTMASK = 0x0400,
    SMG3DS_EXI_EXTINT = 0x0800,
    SMG3DS_EXI_EXT = 0x1000,
    SMG3DS_EXI_ROMDIS = 0x2000,

    SMG3DS_EXI_TSTART = 0x0001,
    SMG3DS_EXI_DMA = 0x0002,
    SMG3DS_EXI_RW_SHIFT = 2,
    SMG3DS_EXI_TLEN_SHIFT = 4,

    SMG3DS_EXI_READ = 0,
    SMG3DS_EXI_WRITE = 1,
    SMG3DS_EXI_READWRITE = 2,

    SMG3DS_IPL_ROM_SIZE = 0x200000,
    SMG3DS_IPL_SRAM_BASE = 0x800000,
    SMG3DS_IPL_SRAM_SIZE = 0x44,
    SMG3DS_IPL_UART_BASE = 0x800400,
    SMG3DS_IPL_UART_SIZE = 0x50,
    SMG3DS_IPL_WII_RTC_BASE = 0x840000,
    SMG3DS_IPL_WII_RTC_FLAGS = 0x840020,
    SMG3DS_IPL_WII_RTC_SIZE = 0x40,
    SMG3DS_IPL_EUART_BASE = 0xc00000,
    SMG3DS_IPL_EUART_SIZE = 8
};

typedef struct Smg3dsExiChannel {
    u32 status;
    u32 dma_address;
    u32 dma_length;
    u32 control;
    u32 immediate;
} Smg3dsExiChannel;

typedef struct Smg3dsIplDevice {
    u32 command;
    u32 command_bytes;
    u32 cursor;
    u8 sram[SMG3DS_IPL_SRAM_SIZE];
    u8 rtc_flags;
} Smg3dsIplDevice;

typedef struct Smg3dsExiState {
    Smg3dsExiChannel channels[SMG3DS_EXI_CHANNELS];
    Smg3dsIplDevice ipl;
    Smg3dsExiStats stats;
} Smg3dsExiState;

typedef struct Smg3dsExiWriteGroup {
    u32 register_index;
    u32 bits;
    u32 mask;
} Smg3dsExiWriteGroup;

static Smg3dsExiState g_exi;

static const char g_ipl_header[] =
    "(C) 1999-2001 Nintendo.  All rights reserved."
    "(C) 1999 ArtX Inc.  All rights reserved.";

static void store_be32(u8 *destination, u32 value)
{
    destination[0] = (u8)(value >> 24);
    destination[1] = (u8)(value >> 16);
    destination[2] = (u8)(value >> 8);
    destination[3] = (u8)value;
}

static void update_rtc(void)
{
    const time_t now = time(NULL);
    u32 seconds = 0;

    if (now > (time_t)SMG3DS_EXI_GC_EPOCH)
        seconds = (u32)(now - (time_t)SMG3DS_EXI_GC_EPOCH);
    store_be32(g_exi.ipl.sram, seconds);
}

static void initialize_sram(void)
{
    memset(g_exi.ipl.sram, 0, sizeof(g_exi.ipl.sram));
    g_exi.ipl.sram[4] = 0x00;
    g_exi.ipl.sram[5] = 0x2c;
    g_exi.ipl.sram[6] = 0xff;
    g_exi.ipl.sram[7] = 0xd0;
    g_exi.ipl.sram[0x16] = 0;
    g_exi.ipl.sram[0x17] = 0x2c;
    memcpy(&g_exi.ipl.sram[0x18], "DOLPHINSLOTA", 12);
    memcpy(&g_exi.ipl.sram[0x24], "DOLPHINSLOTB", 12);
    g_exi.ipl.sram[0x3e] = 0x6e;
    g_exi.ipl.sram[0x3f] = 0x6d;
    update_rtc();
}

void smg3ds_exi_init(void)
{
    memset(&g_exi, 0, sizeof(g_exi));
    g_exi.channels[0].status = SMG3DS_EXI_EXTINT;
    g_exi.channels[1].status =
        SMG3DS_EXI_EXTINT | (1u << 7);
    initialize_sram();
    g_exi.stats.initialized = true;
}

static bool exi_offset(u32 physical, u8 size, u32 *offset)
{
    u32 base;
    u64 end;

    if (physical >= SMG3DS_EXI_BASE_GC &&
        physical < SMG3DS_EXI_BASE_GC + SMG3DS_EXI_SIZE) {
        base = SMG3DS_EXI_BASE_GC;
    } else if (physical >= SMG3DS_EXI_BASE_WII &&
               physical < SMG3DS_EXI_BASE_WII + SMG3DS_EXI_SIZE) {
        base = SMG3DS_EXI_BASE_WII;
    } else {
        return false;
    }
    if (size == 0u || size > 8u)
        return false;
    end = (u64)(physical - base) + size;
    if (end > SMG3DS_EXI_SIZE)
        return false;
    if (offset != NULL)
        *offset = physical - base;
    return true;
}

bool smg3ds_exi_handles(u32 physical, u8 size)
{
    return exi_offset(physical, size, NULL);
}

static u32 selected_device(const Smg3dsExiChannel *channel)
{
    return (channel->status & SMG3DS_EXI_CHIP_SELECT_MASK) >> 7;
}

static bool valid_device_select(u32 select)
{
    return select == 1u || select == 2u || select == 4u;
}

static bool selected_ipl(u32 channel, u32 select)
{
    return channel == 0u && select == 2u;
}

static void reset_ipl_transaction(void)
{
    g_exi.ipl.command = 0;
    g_exi.ipl.command_bytes = 0;
    g_exi.ipl.cursor = 0;
}

static u8 ipl_transfer_byte(u8 input)
{
    u8 output = input;
    u32 address;
    u32 device_address;
    const bool write = (g_exi.ipl.command & 0x80000000u) != 0u;

    if (g_exi.ipl.command_bytes < 4u) {
        g_exi.ipl.command =
            (g_exi.ipl.command << 8) | (u32)input;
        ++g_exi.ipl.command_bytes;
        if (g_exi.ipl.command_bytes == 4u) {
            update_rtc();
            ++g_exi.stats.ipl_commands;
            g_exi.stats.last_command = g_exi.ipl.command;
        }
        return 0xffu;
    }

    address = (g_exi.ipl.command >> 6) & 0x01ffffffu;
    output = 0;
    if (address < SMG3DS_IPL_ROM_SIZE) {
        device_address = (address + g_exi.ipl.cursor++) %
                         SMG3DS_IPL_ROM_SIZE;
        if (!write && device_address < sizeof(g_ipl_header) - 1u)
            output = (u8)g_ipl_header[device_address];
    } else if (address >= SMG3DS_IPL_SRAM_BASE &&
               address < SMG3DS_IPL_SRAM_BASE +
                         SMG3DS_IPL_SRAM_SIZE) {
        device_address =
            (address - SMG3DS_IPL_SRAM_BASE + g_exi.ipl.cursor++) %
            SMG3DS_IPL_SRAM_SIZE;
        if (write)
            g_exi.ipl.sram[device_address] = input;
        else
            output = g_exi.ipl.sram[device_address];
    } else if (address >= SMG3DS_IPL_UART_BASE &&
               address < SMG3DS_IPL_UART_BASE +
                         SMG3DS_IPL_UART_SIZE) {
        output = 0;
    } else if (address >= SMG3DS_IPL_WII_RTC_BASE &&
               address < SMG3DS_IPL_WII_RTC_BASE +
                         SMG3DS_IPL_WII_RTC_SIZE &&
               address == SMG3DS_IPL_WII_RTC_FLAGS) {
        if (write)
            g_exi.ipl.rtc_flags = input;
        else
            output = g_exi.ipl.rtc_flags;
    } else if (address >= SMG3DS_IPL_EUART_BASE &&
               address < SMG3DS_IPL_EUART_BASE +
                         SMG3DS_IPL_EUART_SIZE) {
        output = 0;
    }
    return output;
}

static u8 transfer_byte(u32 channel, u32 select, u8 input)
{
    if (selected_ipl(channel, select))
        return ipl_transfer_byte(input);
    return 0;
}

static bool dma_range_valid(CPUState *cpu, u32 address, u32 length)
{
    const u32 physical = address & 0x3fffffffu;
    const u64 end = (u64)physical + length;

    if (cpu == NULL || length > SMG3DS_EXI_MAX_DMA)
        return false;
    if (physical < cpu->ram_size && end <= cpu->ram_size)
        return true;
    return physical >= SMG3DS_EXI_MEM2_BASE &&
           end <= (u64)SMG3DS_EXI_MEM2_BASE + SMG3DS_EXI_MEM2_SIZE;
}

static void complete_transfer(CPUState *cpu, u32 channel_index)
{
    Smg3dsExiChannel *channel = &g_exi.channels[channel_index];
    const u32 select = selected_device(channel);
    const u32 rw = (channel->control >> SMG3DS_EXI_RW_SHIFT) & 3u;
    u32 length;
    u32 position;

    if (!valid_device_select(select)) {
        ++g_exi.stats.stalled_transfers;
        return;
    }

    if ((channel->control & SMG3DS_EXI_DMA) == 0u) {
        u32 result = 0;

        length = ((channel->control >> SMG3DS_EXI_TLEN_SHIFT) & 3u) + 1u;
        ++g_exi.stats.immediate_transfers;
        for (position = 0; position < length; ++position) {
            const u32 shift = 24u - position * 8u;
            const u8 input = (u8)(channel->immediate >> shift);
            u8 output = 0;

            if (rw == SMG3DS_EXI_READ) {
                output = transfer_byte(channel_index, select, 0);
            } else if (rw == SMG3DS_EXI_WRITE) {
                (void)transfer_byte(channel_index, select, input);
            } else if (rw == SMG3DS_EXI_READWRITE) {
                output = transfer_byte(channel_index, select, input);
            } else {
                ++g_exi.stats.transfer_failures;
            }
            result |= (u32)output << shift;
        }
        if (rw == SMG3DS_EXI_READ || rw == SMG3DS_EXI_READWRITE)
            channel->immediate = result;
    } else {
        length = channel->dma_length;
        ++g_exi.stats.dma_transfers;
        if (!dma_range_valid(cpu, channel->dma_address, length) ||
            (rw != SMG3DS_EXI_READ && rw != SMG3DS_EXI_WRITE)) {
            ++g_exi.stats.transfer_failures;
        } else {
            for (position = 0; position < length; ++position) {
                const u32 address = channel->dma_address + position;
                if (rw == SMG3DS_EXI_READ) {
                    mem_write8(cpu, address,
                               transfer_byte(channel_index, select, 0));
                } else {
                    (void)transfer_byte(channel_index, select,
                                        mem_read8(cpu, address));
                }
            }
        }
    }

    g_exi.stats.transferred_bytes += length;
    channel->control &= ~SMG3DS_EXI_TSTART;
    channel->status |= SMG3DS_EXI_TCINT;
}

static u32 read_register(u32 register_index)
{
    const u32 channel_index =
        register_index / SMG3DS_EXI_REGISTERS;
    const u32 local_register =
        register_index % SMG3DS_EXI_REGISTERS;
    Smg3dsExiChannel *channel = &g_exi.channels[channel_index];

    switch (local_register) {
    case SMG3DS_EXI_STATUS:
        channel->status &= ~SMG3DS_EXI_EXT;
        return channel->status;
    case SMG3DS_EXI_DMA_ADDRESS:
        return channel->dma_address;
    case SMG3DS_EXI_DMA_LENGTH:
        return channel->dma_length;
    case SMG3DS_EXI_CONTROL:
        return channel->control;
    case SMG3DS_EXI_IMMEDIATE:
        return channel->immediate;
    default:
        return 0;
    }
}

static void write_status(u32 channel_index, u32 bits, u32 mask)
{
    Smg3dsExiChannel *channel = &g_exi.channels[channel_index];
    const u32 old_select = selected_device(channel);
    u32 writable = SMG3DS_EXI_EXIINTMASK |
                   SMG3DS_EXI_TCINTMASK |
                   SMG3DS_EXI_CLOCK_MASK |
                   SMG3DS_EXI_CHIP_SELECT_MASK;
    u32 direct_mask;
    u32 new_select;

    if (channel_index < 2u)
        writable |= SMG3DS_EXI_EXTINTMASK;
    if (channel_index == 0u)
        writable |= SMG3DS_EXI_ROMDIS;

    direct_mask = mask & writable;
    channel->status =
        (channel->status & ~direct_mask) | (bits & direct_mask);
    if ((mask & bits & SMG3DS_EXI_EXIINT) != 0u)
        channel->status &= ~SMG3DS_EXI_EXIINT;
    if ((mask & bits & SMG3DS_EXI_TCINT) != 0u)
        channel->status &= ~SMG3DS_EXI_TCINT;
    if (channel_index < 2u &&
        (mask & bits & SMG3DS_EXI_EXTINT) != 0u)
        channel->status &= ~SMG3DS_EXI_EXTINT;

    new_select = selected_device(channel);
    if (old_select != new_select &&
        selected_ipl(channel_index, new_select))
        reset_ipl_transaction();
}

static void write_register(CPUState *cpu,
                           u32 register_index,
                           u32 bits,
                           u32 mask)
{
    const u32 channel_index =
        register_index / SMG3DS_EXI_REGISTERS;
    const u32 local_register =
        register_index % SMG3DS_EXI_REGISTERS;
    Smg3dsExiChannel *channel = &g_exi.channels[channel_index];

    switch (local_register) {
    case SMG3DS_EXI_STATUS:
        write_status(channel_index, bits, mask);
        break;
    case SMG3DS_EXI_DMA_ADDRESS:
        channel->dma_address =
            (channel->dma_address & ~mask) | (bits & mask);
        break;
    case SMG3DS_EXI_DMA_LENGTH:
        channel->dma_length =
            (channel->dma_length & ~mask) | (bits & mask);
        break;
    case SMG3DS_EXI_CONTROL:
        channel->control =
            (channel->control & ~mask) | (bits & mask);
        if ((mask & SMG3DS_EXI_TSTART) != 0u &&
            (channel->control & SMG3DS_EXI_TSTART) != 0u)
            complete_transfer(cpu, channel_index);
        break;
    case SMG3DS_EXI_IMMEDIATE:
        channel->immediate =
            (channel->immediate & ~mask) | (bits & mask);
        break;
    default:
        break;
    }
}

u64 smg3ds_exi_read(CPUState *cpu, u32 physical, u8 size)
{
    u32 offset;
    u64 result = 0;
    u8 position;
    (void)cpu;

    if (!exi_offset(physical, size, &offset))
        return 0;
    ++g_exi.stats.mmio_reads;
    for (position = 0; position < size; ++position) {
        const u32 current = offset + position;
        const u32 register_value = read_register(current >> 2);
        const u32 shift = 24u - (current & 3u) * 8u;
        result = (result << 8) | ((register_value >> shift) & 0xffu);
    }
    return result;
}

void smg3ds_exi_write(CPUState *cpu,
                      u32 physical,
                      u64 value,
                      u8 size)
{
    Smg3dsExiWriteGroup groups[3];
    u32 offset;
    u32 group_count = 0;
    u8 position;

    if (!exi_offset(physical, size, &offset))
        return;
    memset(groups, 0, sizeof(groups));
    ++g_exi.stats.mmio_writes;
    for (position = 0; position < size; ++position) {
        const u32 current = offset + position;
        const u32 register_index = current >> 2;
        const u32 register_shift = 24u - (current & 3u) * 8u;
        const u32 value_shift = (u32)(size - position - 1u) * 8u;
        const u32 byte = (u32)(value >> value_shift) & 0xffu;
        u32 group = 0;

        while (group < group_count &&
               groups[group].register_index != register_index)
            ++group;
        if (group == group_count) {
            if (group_count >= sizeof(groups) / sizeof(groups[0]))
                return;
            groups[group].register_index = register_index;
            ++group_count;
        }
        groups[group].mask |= 0xffu << register_shift;
        groups[group].bits |= byte << register_shift;
    }
    for (u32 group = 0; group < group_count; ++group) {
        write_register(cpu, groups[group].register_index,
                       groups[group].bits, groups[group].mask);
    }
}

bool smg3ds_exi_interrupt_pending(void)
{
    u32 channel_index;

    for (channel_index = 0; channel_index < SMG3DS_EXI_CHANNELS;
         ++channel_index) {
        const u32 status = g_exi.channels[channel_index].status;
        if (((status & SMG3DS_EXI_EXIINT) != 0u &&
             (status & SMG3DS_EXI_EXIINTMASK) != 0u) ||
            ((status & SMG3DS_EXI_TCINT) != 0u &&
             (status & SMG3DS_EXI_TCINTMASK) != 0u) ||
            ((status & SMG3DS_EXI_EXTINT) != 0u &&
             (status & SMG3DS_EXI_EXTINTMASK) != 0u))
            return true;
    }
    return false;
}

const Smg3dsExiStats *smg3ds_exi_get_stats(void)
{
    return &g_exi.stats;
}

bool smg3ds_exi_self_test(CPUState *cpu)
{
    const u32 base = SMG3DS_EXI_BASE_WII;
    const u32 scratch = 0x80001020u;
    Smg3dsExiState saved;
    u8 scratch_saved[4];
    bool passed;
    u32 position;

    if (!g_exi.stats.initialized || cpu == NULL)
        return false;
    saved = g_exi;
    for (position = 0; position < 4u; ++position)
        scratch_saved[position] = mem_read8(cpu, scratch + position);

    passed =
        smg3ds_exi_read(cpu, base, 4u) == 0x00000800u &&
        smg3ds_exi_read(cpu, base + 0x14u, 4u) == 0x00000880u &&
        smg3ds_exi_read(cpu, base + 0x28u, 4u) == 0u;

    smg3ds_exi_write(cpu, base, 0x00000800u, 4u);
    passed = passed && smg3ds_exi_read(cpu, base, 4u) == 0u;

    smg3ds_exi_write(cpu, base, 0x00000154u, 4u);
    smg3ds_exi_write(cpu, base + 0x10u, 0x20000100u, 4u);
    smg3ds_exi_write(cpu, base + 0x0cu, 0x35u, 4u);
    passed = passed &&
        smg3ds_exi_read(cpu, base + 0x0cu, 4u) == 0x34u &&
        smg3ds_exi_read(cpu, base, 4u) == 0x0000015cu &&
        smg3ds_exi_interrupt_pending();

    smg3ds_exi_write(cpu, base + 0x0cu, 0x31u, 4u);
    passed = passed &&
        smg3ds_exi_read(cpu, base + 0x10u, 4u) == 0x002cffd0u;
    smg3ds_exi_write(cpu, base, 0x0000015cu, 4u);
    passed = passed && !smg3ds_exi_interrupt_pending();

    smg3ds_exi_write(cpu, base, 0u, 4u);
    smg3ds_exi_write(cpu, base, 0x00000150u, 4u);
    smg3ds_exi_write(cpu, base + 0x10u, 0x21000800u, 4u);
    smg3ds_exi_write(cpu, base + 0x0cu, 0x35u, 4u);
    smg3ds_exi_write(cpu, base + 4u, scratch, 4u);
    smg3ds_exi_write(cpu, base + 8u, 4u, 4u);
    smg3ds_exi_write(cpu, base + 0x0cu, 0x03u, 4u);
    passed = passed &&
        smg3ds_exi_read(cpu, base + 0x0cu, 4u) == 0x02u &&
        mem_read8(cpu, scratch) == 0u &&
        mem_read8(cpu, scratch + 1u) == 0u &&
        mem_read8(cpu, scratch + 2u) == 0u &&
        mem_read8(cpu, scratch + 3u) == 0u;

    for (position = 0; position < 4u; ++position)
        mem_write8(cpu, scratch + position, scratch_saved[position]);
    g_exi = saved;
    g_exi.stats.self_test_passed = passed;
    return passed;
}
