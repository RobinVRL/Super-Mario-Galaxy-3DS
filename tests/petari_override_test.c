// SPDX-License-Identifier: GPL-3.0-or-later
#include "cpu/cpu.h"
#include "smg3ds/petari_bridge.h"
#include "smg3ds/petari_overrides.h"

#include <math.h>
#include <stdio.h>

void func_803E30A0(CPUState* ctx);

typedef struct PetariCase {
    float x;
    float tolerance;
    u32 expected;
} PetariCase;

static int compare_case(const CPUState* base, const PetariCase* test,
                        u32 index)
{
    CPUState original = *base;
    CPUState petari = *base;
    const u32 return_pc = 0x81234000u;

    original.pc = SMG3DS_PETARI_MR_IS_NEAR_ZERO_F32_F32_ADDRESS;
    original.lr = return_pc;
    original.gpr[2] = 0x80001000u - 6956u;
    original.fpr[1] = (f64)test->x;
    original.fpr[2] = (f64)test->tolerance;
    original.downcount = 0;
    original.exception = 0;
    original.msr |= 0x00002000u;
    petari = original;

    func_803E30A0(&original);
    if (!smg3ds_petari_dispatch(
            &petari, SMG3DS_PETARI_MR_IS_NEAR_ZERO_F32_F32_ADDRESS)) {
        fprintf(stderr, "case %u: enabled Petari route was not handled\n", index);
        return 0;
    }

    if (original.gpr[3] != test->expected ||
        petari.gpr[3] != test->expected ||
        original.gpr[3] != petari.gpr[3] ||
        original.pc != return_pc || petari.pc != return_pc ||
        original.downcount != petari.downcount ||
        original.exception != petari.exception) {
        fprintf(stderr,
                "case %u mismatch: original={r3=%u pc=%08X dc=%lld ex=%u} "
                "petari={r3=%u pc=%08X dc=%lld ex=%u}\n",
                index, original.gpr[3], original.pc,
                (long long)original.downcount, original.exception,
                petari.gpr[3], petari.pc, (long long)petari.downcount,
                petari.exception);
        return 0;
    }

    return 1;
}

int main(void)
{
    static const PetariCase cases[] = {
        {0.0f, 0.001f, 1u},
        {-0.0f, 0.001f, 1u},
        {0.0005f, 0.001f, 1u},
        {-0.0005f, 0.001f, 1u},
        {0.001f, 0.001f, 0u},
        {-2.0f, 1.0f, 0u},
        {INFINITY, 1.0f, 0u},
        {-INFINITY, 1.0f, 0u},
        {NAN, 1.0f, 0u},
        {0.0f, -1.0f, 0u},
    };
    CPUState base;

    if (!DOLRECOMP_FORCE_DISPATCH(
            SMG3DS_PETARI_MR_IS_NEAR_ZERO_F32_F32_ADDRESS) ||
        DOLRECOMP_FORCE_DISPATCH(
            SMG3DS_PETARI_MR_IS_NEAR_ZERO_F32_F32_ADDRESS + 4u)) {
        fputs("manifest force-dispatch predicate is inconsistent\n", stderr);
        return 1;
    }

    if (!cpu_init(&base)) {
        fputs("could not allocate host test MEM1\n", stderr);
        return 1;
    }
    mem_write32(&base, 0x80001000u, 0u);

    for (u32 i = 0; i < (u32)(sizeof(cases) / sizeof(cases[0])); ++i) {
        if (!compare_case(&base, &cases[i], i)) {
            cpu_free(&base);
            return 1;
        }
    }

    CPUState unknown = base;
    unknown.pc = 0x12345678u;
    if (smg3ds_petari_dispatch(
            &unknown,
            SMG3DS_PETARI_MR_IS_NEAR_ZERO_F32_F32_ADDRESS + 4u) != 0 ||
        unknown.pc != 0x12345678u) {
        fputs("unknown address did not remain on the DolRecomp fallback path\n",
              stderr);
        cpu_free(&base);
        return 1;
    }

    cpu_free(&base);
    printf("Petari/DolRecomp differential passed: %u scalar cases.\n",
           (u32)(sizeof(cases) / sizeof(cases[0])));
    return 0;
}
