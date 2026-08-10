// SPDX-License-Identifier: GPL-3.0-or-later
#include "smg3ds/petari_bridge.h"
#include "smg3ds/petari_overrides.h"

static int bridge_mr_is_near_zero_f32_f32(CPUState* cpu)
{
    const float x = (float)cpu->fpr[1];
    const float tolerance = (float)cpu->fpr[2];
    const bool negative = x < 0.0f;

    /* Match the generated entry block before checking the guest FP facility. */
    cpu->downcount -= 3;
    if (!ppc_fp_available(
            cpu, SMG3DS_PETARI_MR_IS_NEAR_ZERO_F32_F32_ADDRESS)) {
        return 1;
    }

    /* Preserve DolRecomp's block-budget charge for the selected PPC path. */
    cpu->downcount -= negative ? 5 : 4;
    cpu->gpr[3] = smg3ds_petari_mr_is_near_zero_f32_f32(x, tolerance)
                      ? 1u
                      : 0u;
    cpu->pc = cpu->lr & ~3u;
    return 1;
}

#define SMG3DS_PETARI_DISPATCH_CASE(                                   \
    CPU, NAME, ADDRESS, SIZE, ENABLED)                                 \
    SMG3DS_PETARI_IF(ENABLED)(                                         \
        case ADDRESS:                                                  \
            return bridge_##NAME(CPU);)

int smg3ds_petari_dispatch(CPUState* cpu, u32 address)
{
    switch (address) {
        SMG3DS_PETARI_OVERRIDES(SMG3DS_PETARI_DISPATCH_CASE, cpu)
    default:
        return 0;
    }
}

#undef SMG3DS_PETARI_DISPATCH_CASE
