#ifndef SMG3DS_PETARI_BRIDGE_H
#define SMG3DS_PETARI_BRIDGE_H

#include "cpu/cpu.h"

/* Return nonzero when an enabled Petari implementation handled ADDRESS. */
int smg3ds_petari_dispatch(CPUState* cpu, u32 address);

/* Portable Petari source isolated from Wii SDK and Metrowerks headers. */
bool smg3ds_petari_mr_is_near_zero_f32_f32(float x, float tolerance);

#endif /* SMG3DS_PETARI_BRIDGE_H */
