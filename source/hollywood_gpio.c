// SPDX-License-Identifier: GPL-3.0-or-later
#include "smg3ds/hollywood_gpio.h"

#include <string.h>

enum {
    SMG3DS_GPIOB_OUT = 0x0d8000c0u,
    SMG3DS_GPIOB_DIR = 0x0d8000c4u,
    SMG3DS_GPIOB_IN = 0x0d8000c8u,
    SMG3DS_GPIOB_OWNER_MASK = 0x0000c3a0u,
    SMG3DS_GPIO_SLOT_IN = 0x00000080u,
    SMG3DS_GPIOB_DIR_RESET = 0x00ffdf3fu
};

static Smg3dsHollywoodGpioStats g_stats;

static bool valid_size(uint8_t size)
{
    return size == 1u || size == 2u || size == 4u;
}

static bool register_access(uint32_t physical, uint8_t size,
                            uint32_t register_address)
{
    return valid_size(size) && physical >= register_address &&
           (uint64_t)physical + size <= (uint64_t)register_address + 4u;
}

static uint32_t lane_mask(uint8_t size)
{
    return size == 4u ? UINT32_MAX : (1u << (size * 8u)) - 1u;
}

static uint64_t read_lane(uint32_t value, uint32_t physical, uint8_t size,
                          uint32_t register_address)
{
    const uint32_t shift =
        (4u - (physical - register_address) - size) * 8u;
    return (value >> shift) & lane_mask(size);
}

static uint32_t merge_lane(uint32_t old_value, uint64_t value,
                           uint32_t physical, uint8_t size,
                           uint32_t register_address)
{
    const uint32_t shift =
        (4u - (physical - register_address) - size) * 8u;
    const uint32_t mask = lane_mask(size) << shift;
    return (old_value & ~mask) |
           (((uint32_t)value << shift) & mask);
}

void smg3ds_hollywood_gpio_init(void)
{
    memset(&g_stats, 0, sizeof(g_stats));
    g_stats.direction = SMG3DS_GPIOB_DIR_RESET;
}

bool smg3ds_hollywood_gpio_handles(uint32_t physical, uint8_t size)
{
    return register_access(physical, size, SMG3DS_GPIOB_OUT) ||
           register_access(physical, size, SMG3DS_GPIOB_DIR) ||
           register_access(physical, size, SMG3DS_GPIOB_IN);
}

uint64_t smg3ds_hollywood_gpio_read(uint32_t physical, uint8_t size,
                                    bool disc_present)
{
    uint32_t value = 0u;
    uint32_t register_address = SMG3DS_GPIOB_IN;

    ++g_stats.reads;
    if (register_access(physical, size, SMG3DS_GPIOB_OUT)) {
        value = g_stats.output;
        register_address = SMG3DS_GPIOB_OUT;
    } else if (register_access(physical, size, SMG3DS_GPIOB_DIR)) {
        value = g_stats.direction;
        register_address = SMG3DS_GPIOB_DIR;
    } else if (register_access(physical, size, SMG3DS_GPIOB_IN)) {
        /* The AV encoder is not decoded yet. SDA low supplies its ACK. */
        value = disc_present ? SMG3DS_GPIO_SLOT_IN : 0u;
    } else {
        return 0u;
    }
    return read_lane(value, physical, size, register_address);
}

void smg3ds_hollywood_gpio_write(uint32_t physical, uint64_t value,
                                 uint8_t size)
{
    uint32_t requested;

    ++g_stats.writes;
    if (register_access(physical, size, SMG3DS_GPIOB_OUT)) {
        requested = merge_lane(g_stats.output, value, physical, size,
                               SMG3DS_GPIOB_OUT);
        g_stats.output =
            (g_stats.output & ~SMG3DS_GPIOB_OWNER_MASK) |
            (requested & SMG3DS_GPIOB_OWNER_MASK);
    } else if (register_access(physical, size, SMG3DS_GPIOB_DIR)) {
        requested = merge_lane(g_stats.direction, value, physical, size,
                               SMG3DS_GPIOB_DIR);
        g_stats.direction =
            (g_stats.direction & ~SMG3DS_GPIOB_OWNER_MASK) |
            (requested & SMG3DS_GPIOB_OWNER_MASK);
    }
    /* GPIOB_IN is read-only. */
}

bool smg3ds_hollywood_gpio_self_test(void)
{
    bool passed;

    smg3ds_hollywood_gpio_init();
    smg3ds_hollywood_gpio_write(SMG3DS_GPIOB_OUT, UINT32_MAX, 4u);
    passed = smg3ds_hollywood_gpio_read(SMG3DS_GPIOB_OUT, 4u, true) ==
                 SMG3DS_GPIOB_OWNER_MASK &&
             smg3ds_hollywood_gpio_read(SMG3DS_GPIOB_IN, 4u, true) ==
                 SMG3DS_GPIO_SLOT_IN;
    smg3ds_hollywood_gpio_write(SMG3DS_GPIOB_OUT + 2u, 0x8000u, 2u);
    passed = passed &&
             smg3ds_hollywood_gpio_read(SMG3DS_GPIOB_OUT, 4u, true) ==
                 0x00008000u;
    smg3ds_hollywood_gpio_write(SMG3DS_GPIOB_DIR, 0u, 4u);
    passed = passed &&
             smg3ds_hollywood_gpio_read(SMG3DS_GPIOB_DIR, 4u, true) ==
                 (SMG3DS_GPIOB_DIR_RESET & ~SMG3DS_GPIOB_OWNER_MASK);

    smg3ds_hollywood_gpio_init();
    g_stats.self_test_passed = passed;
    return passed;
}

const Smg3dsHollywoodGpioStats* smg3ds_hollywood_gpio_get_stats(void)
{
    return &g_stats;
}
