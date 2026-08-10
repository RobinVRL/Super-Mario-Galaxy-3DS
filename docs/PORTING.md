# 3DS/Azahar port boundary

This repository is a bring-up environment, not a playable port. DolRecomp translates
Broadway instructions to C, but it does not translate Wii operating-system or hardware
behavior. ModernGekko currently gets those services from a desktop-oriented runtime and
large portions of Dolphin; that production chassis cannot be linked to libctru unchanged.

## What is wired up

- ARM11 compilation of DolRecomp's portable CPU support and generated C.
- A bounded DOL loader for MEM1/MEM2 sections and BSS.
- Guest big-endian memory validation.
- A 24 MiB MEM1 plus 64 MiB MEM2 allocation probe.
- Stubbed external/MMIO reads and writes with counters.
- A bounded, user-triggered generated-code dispatcher.
- `.3dsx` packaging for Azahar or Homebrew Launcher.

## Upstream compatibility finding

The current top-level DolRecomp and ModernGekko snapshots must not be mixed through
ModernGekko's dynamic module ABI without an audit. Their `CPUState` layouts have already
diverged: current DolRecomp carries an inline SPR array, while ModernGekko ABI v3 exposes
SPR callbacks and asserts a different structure size. This target intentionally compiles
DolRecomp's generated C and its matching CPU support from the same pinned submodule
commit. ModernGekko is retained as the behavioral runtime reference, not linked into the
3DS binary.

Portable Petari functions use the separate guest-address routing and ABI bridge
described in [PETARI_HYBRID.md](PETARI_HYBRID.md). RecompCore informed the
fail-closed dispatch design but is not linked or embedded.

## Required porting work

1. Replace Wii OS entry points (threads, alarms, heaps, DVD, NAND, time) through
   DolRecomp's host-call/replacement mechanism.
   Reproduce the required Wii low-memory boot values before entering the DOL as well.
2. Implement GX command translation on citro3d. The 3DS PICA200 is not compatible with
   the Wii GX FIFO, so ModernGekko's desktop backends are useful behavior references,
   not drop-in code.
3. Map PAD/WPAD to libctru HID and decide how pointer, shake, and Nunchuk inputs map to
   buttons, circle pad, C-stick, gyro, and touchscreen.
4. Stream DSP audio through ndsp instead of emulating AI/DSP registers alone.
5. Replace disc reads with indexed RomFS/SD assets. Do not redistribute Nintendo data.
6. Audit executable-memory writes reported by DolRecomp; static recompilation cannot
   reproduce self-modifying code without explicit patches.
7. Establish memory tiers. Full retail Wii memory is 88 MiB before generated code,
   runtime state, graphics, and audio. Old 3DS is not a credible target; New 3DS/Azahar
   remains tight enough that asset streaming and memory reduction are mandatory.

## First useful milestone

Stop at the earliest OS function, replace it with a host call that prints its address and
arguments, then advance one subsystem at a time. Do not attempt an unbounded dispatcher
loop while external reads still return zero.
