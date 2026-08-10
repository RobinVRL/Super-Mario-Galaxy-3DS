# Petari/DolRecomp hybrid architecture

This target can replace individual RMGE01 guest functions with portable source
reconstructed by Petari while retaining the generated DolRecomp implementation
for every other address. It does not link Petari as a Wii binary, emulate Wii
hardware, or embed Dolphin.

## Upstream findings

Petari's current build targets the Korean `RMGK01` revision with Metrowerks
PowerPC flags and Wii SDK/JSystem headers. Its translation units are not a
portable ARM library. Historical Petari revision
`e8d8a89963f9a694d1e7bddad12fa3fe1f81451e` contains an RMGE symbol map, but
every address from it must be checked against the local DOL and generated PPC
body. The current Petari source remains useful at function granularity once a
function's dependencies and ABI have been isolated.

RecompCore was also evaluated. Its useful contract is guest-PC keyed
`dispatch(CPUState*, address) -> handled`, with version/game checks and a
fallback when an address is not covered. RecompCore itself is not used here:
it is a desktop Dolphin fork, its uncovered-code path is Dolphin's interpreter,
its module ABI has a different `CPUState`, and Wii is untested. The 3DS target
keeps a statically linked, project-owned equivalent of the small dispatch
contract.

## Routing

```text
guest call / function pointer (original Wii address)
                    |
                    v
       DolRecomp generated dispatch boundary
                    |
                    v
       dolrecomp_dispatch_replacement
                    |
          +---------+----------+
          |                    |
   enabled Petari entry   unknown/disabled entry
          |                    |
   CPUState ABI bridge    generated DolRecomp body
          |
   isolated portable source
```

`include/smg3ds/petari_overrides.h` is the tracked manifest. Each row records
the RMGE01 guest address, original size, and an `ENABLED` value. The manifest
also pins the exact DOL SHA-256. Both configuration and build preflight fail
closed if an override is enabled for another binary revision.

DolRecomp's outer dispatcher already asks for a replacement before calling an
original chunk. Direct branches inside one generated chunk were the exception:
the C emitter lowered them to `goto label_ADDRESS`, bypassing the dispatcher.
The emitter now places `DOLRECOMP_FORCE_DISPATCH(address)` before every local
direct branch. The project force-includes its manifest, so an enabled target
sets `ctx->pc` and returns to the outer dispatcher. With no project macro, the
generated default is zero and DolRecomp behaves exactly as before. Generated
files are regenerated, never patched by hand.

The native functions have project-owned host names. They do not claim a
`func_XXXXXXXX` symbol, so the original chunk stays linked and
`dolrecomp_call_original` remains available. Duplicate enabled addresses also
produce duplicate `case` labels in the router and fail at compile time.

## First override

The first enabled row is Petari `MR::isNearZero(float, float)`:

- RMGE01 address: `0x803E5934`
- original size: `0x28`
- Petari source: `src/Game/Util/MathUtil.cpp`
- host source: `source/petari/petari_math_util.c`
- PPC bridge: `source/petari/petari_cpu_bridge.c`

The bridge reads the two PPC `f32` arguments from FPR1/FPR2, checks guest FP
availability, calls the isolated Petari algorithm, normalizes the C `bool` to
GPR3 `0` or `1`, charges the same generated block budget, and returns through
the guest LR. It never passes a guest pointer or object to native C.

The local DOL contains 163 direct calls to this address. Of those, 161 already
crossed a chunk boundary. The remaining callers at `0x803E5084` and
`0x803E54A0` are now forced through replacement dispatch. The original body at
`label_803E5934` remains in `func_803E30A0` for fallback and differential tests.

Run the focused checks with:

```powershell
.\tools\verify-petari-manifest.ps1 -Dol .\romfs\game\main.dol
.\tools\test-petari-override.ps1
```

The differential test executes both the retained generated function and the
Petari bridge over normal values, signed zero, the strict boundary, infinities,
NaN, and negative tolerance. It compares the result, return PC, exception
state, and generated cycle charge.

## ABI policy for later functions

- Guest addresses remain the symbol identity. Never route by an ARM linker
  address or Metrowerks-mangled C++ name.
- PPC arguments/results must be marshalled explicitly from `CPUState` according
  to the original PPC EABI. ARM AAPCS is not compatible.
- Wii pointers are 32-bit effective addresses into guest MEM1/MEM2. They are
  not native pointers. Read and write them through the big-endian guest-memory
  helpers.
- Petari class compatibility must be proven one type at a time with original
  size/offset/vtable data. Matching pointer width does not prove compatibility;
  Metrowerks and GCC may differ in alignment, enums, bitfields, inheritance,
  thunks, member pointers, and static initialization.
- Floating-point comparisons, NaNs, denormals, signed zero, FPSCR exceptions,
  and operation order need differential coverage. Do not enable fast-math.
- A bridge must account for guest exceptions, return PC, and scheduling charge.
  Volatile PPC FPR/CR state can still differ from the generated instruction
  trace and must be modeled if a caller is shown to depend on it.
- Import one function body at a time. Do not compile a full Petari translation
  unit until all of its Wii SDK, global, allocator, and object-layout
  dependencies have native contracts.

## Safest migration order

After verifying each RMGE address and generated body independently:

1. `MR::sign(float)` -- scalar comparisons; add NaN cases.
2. `MR::isInRange(float, float, float)` -- scalar comparisons only.
3. `MR::getInterpolateValue(float, float, float)` -- scalar arithmetic; preserve
   operation order and rounding.
4. `MR::normalizeAngleAbs(float)` -- scalar, but requires boundary/infinity/NaN
   tests and exact constants.
5. `MR::getHashCode(const char*)` -- simple algorithm, but first exercise the
   guest-string adapter and byte signedness.

`BitArray::isOn`/`set` are reasonable after a guest-object layout adapter exists.
GX/Citro3D, HID, filesystem, threading, audio, and Wii cache/memory replacement
remain separate subsystem projects and are intentionally outside this proof.
