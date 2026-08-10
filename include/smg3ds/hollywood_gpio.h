// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef SMG3DS_HOLLYWOOD_GPIO_H
#define SMG3DS_HOLLYWOOD_GPIO_H

#include <stdbool.h>
#include <stdint.h>

typedef struct Smg3dsHollywoodGpioStats {
    uint32_t reads;
    uint32_t writes;
    uint32_t output;
    uint32_t direction;
    bool self_test_passed;
} Smg3dsHollywoodGpioStats;

void smg3ds_hollywood_gpio_init(void);
bool smg3ds_hollywood_gpio_handles(uint32_t physical, uint8_t size);
uint64_t smg3ds_hollywood_gpio_read(uint32_t physical, uint8_t size,
                                    bool disc_present);
void smg3ds_hollywood_gpio_write(uint32_t physical, uint64_t value,
                                 uint8_t size);
bool smg3ds_hollywood_gpio_self_test(void);
const Smg3dsHollywoodGpioStats* smg3ds_hollywood_gpio_get_stats(void);

#endif
