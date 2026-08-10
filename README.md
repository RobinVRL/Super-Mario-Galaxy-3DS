# Super Mario Galaxy 3DS

An experimental static-recompilation bring-up of **Super Mario Galaxy (NTSC-U,
RMGE01)** for New Nintendo 3DS hardware and the Azahar emulator.

This is a research port, not a playable release. It recompiles the game's Broadway
PowerPC code to C with a pinned [DolRecomp](https://github.com/ExpansionPak/DolRecomp)
fork, runs that code against a small project-owned Wii compatibility layer, and presents
early GX output through a software renderer. The immediate goal is deterministic forward
progress through the retail executable while replacing Wii services one subsystem at a
time.

No Nintendo code or game data is included. You must supply your own legally obtained,
extracted NTSC-U disc.

## Current status

The project can configure and compile the retail RMGE01 DOL into a `.3dsx`, load its
MEM1/MEM2 sections, index extracted disc assets, initialize the minimum Wii boot state,
and execute generated blocks under a bounded frame loop. It is instrumented as a
bring-up environment: the bottom screen reports CPU, interrupt, MMIO, IOS, EXI, disc,
and GX activity while the top screen receives the current software-rendered framebuffer.

Implemented today:

- DolRecomp's generated C backend and matching portable Broadway CPU state.
- 24 MiB MEM1 plus sparse, on-demand 64 MiB MEM2 with big-endian guest access.
- Retail DOL loading, Wii low-memory values, region settings, and boot metadata.
- FST-indexed disc access backed by extracted files on SD/Azahar storage.
- A focused IOS IPC layer for the services reached so far, including `/dev/di`,
  `/dev/stm/*`, and the title's early USB request path.
- EXI register handling with RTC/SRAM behavior, Hollywood GPIO/I2C state, VI/PI/IPC
  interrupts, the decrementer, timebase accounting, system calls, and FP-unavailable
  recovery.
- An early GX FIFO decoder and software geometry rasterizer with EFB/XFB presentation.
- Per-function native replacements through a fail-closed Petari/DolRecomp bridge. The
  first enabled replacement is `MR::isNearZero(float, float)` and is differentially
  tested against the retained generated body.
- Runtime fault logging to `sdmc:/smg3ds-runtime.log`.

Major missing pieces:

- Complete Wii OS, IOS, MMIO, and device behavior.
- A production GX/TEV renderer: textures, materials, lighting, blending, depth behavior,
  and many command/state paths remain incomplete.
- Game input mapping beyond bring-up controls, DSP/audio output, save/NAND behavior,
  networking, and broad title compatibility.
- Performance and memory work required for sustained gameplay on real New 3DS hardware.

Old 3DS is not a realistic target. The Wii memory model alone consumes 88 MiB before
generated code, graphics, audio, and runtime state.

## How it fits together

```text
RMGE01 main.dol
      |
      v
DolRecomp generated C + Broadway CPUState
      |
      +--> project-owned replacements / Petari ABI bridges
      +--> Wii memory, interrupts, EXI, GPIO, and IOS HLE
      +--> FST-backed reads from extracted disc files
      +--> GX FIFO decoder -> software EFB/XFB -> 3DS top screen
```

DolRecomp is pinned to a private project fork because this target adds a configurable
local-branch dispatch boundary and the CPU support needed by the current Wii exception
path. ModernGekko is retained only as a behavioral and ABI reference; it is not linked
into the 3DS binary. See [docs/PORTING.md](docs/PORTING.md) and
[docs/PETARI_HYBRID.md](docs/PETARI_HYBRID.md) for the detailed boundary.

## Requirements

- Windows PowerShell.
- Git with submodule support.
- CMake and a working host C compiler. The current Windows setup uses MSYS2 MinGW.
- [devkitPro](https://devkitpro.org/) with the `3ds-dev` package group; `DEVKITPRO` and
  `DEVKITARM` must be available to the build.
- Azahar, or a New 3DS capable of launching `.3dsx` homebrew.
- An extracted NTSC-U disc tree with this layout:

```text
DATA/
  sys/
    main.dol
    boot.bin
    fst.bin
  files/
    ...
```

The enabled Petari manifest is pinned to the exact supported RMGE01 DOL hash and fails
closed for a different revision.

## Set up the checkout

Initialize the two pinned reference repositories:

```powershell
git submodule update --init external/DolRecomp external/ModernGekko
```

Access to the private DolRecomp dependency is required for a fresh clone of this private
project repository.

## Configure private game input

Place your extracted disc under `extracted/DATA`, or pass another extracted DATA root.
Configuration validates the DOL, `boot.bin`, `fst.bin`, and the Petari revision contract;
builds DolRecomp when needed; emits split generated C; and transactionally installs only
the private runtime inputs required in RomFS.

```powershell
.\tools\configure.ps1 `
  -Dol .\extracted\DATA\sys\main.dol `
  -TitleId RMGE01
```

`generated/`, `romfs/game/`, the extracted disc, ISO images, and compiled artifacts are
ignored by Git. Do not force-add or redistribute them.

## Build

Use the persistent object cache during normal development:

```powershell
.\tools\build.ps1 -Incremental
```

The build first runs the render-path preflight. That check validates the retail DOL,
generated inventory, disc assets, Petari manifest, software-rasterizer proof, EXI
transactions, IOS/interrupt behavior, and the configured Azahar asset path before any
3DS compilation starts.

Omit `-Incremental` for a clean isolated rebuild. Build outputs are copied to the project
root, including `smg3ds-bringup.3dsx`.

## Run in Azahar

Expose the extracted disc files to Azahar's emulated SD card. The helper creates a
validated junction at `sdmc:/smg3ds/DATA` and refuses to replace an unrelated path:

```powershell
.\tools\prepare-azahar-assets.ps1 `
  -DataRoot .\extracted\DATA
```

Then launch the `.3dsx` directly or use:

```powershell
.\tools\run-azahar.ps1 -Azahar "C:\Program Files\Azahar\azahar.exe"
```

For real hardware, copy the extracted `DATA/files` tree to
`sdmc:/smg3ds/DATA/files`. The DOL and disc metadata used at startup are packaged from
your local ignored `romfs/game` configuration.

Runtime controls:

- `A`: pause or resume PPC execution.
- `START`: exit.

When every startup check passes, PPC execution starts automatically. A fault pauses
execution and records detailed state on the debug console and in
`sdmc:/smg3ds-runtime.log`.

## Validation

Run the focused native-replacement checks with:

```powershell
.\tools\verify-petari-manifest.ps1 -Dol .\extracted\DATA\sys\main.dol
.\tools\test-petari-override.ps1
```

The differential test covers normal values, signed zero, boundary values, infinities,
NaN, return state, exception state, and generated cycle accounting. The regular
incremental build runs the broader project preflight.

## Repository layout

- `source/` and `include/smg3ds/` - 3DS host runtime, HLE, loader, renderer, and Petari
  bridge.
- `tools/` - transactional configuration, validation, build, Azahar asset, and launch
  helpers.
- `tests/` - focused host-side differential tests.
- `external/DolRecomp/` - pinned, project-modified static recompiler dependency.
- `external/ModernGekko/` - pinned behavioral/runtime reference only.
- `generated/` and `romfs/game/` - ignored private/generated inputs; only `.gitkeep` is
  tracked.
- `docs/` - port boundary and per-function replacement architecture.

## Legal and licensing

This repository does not provide a game, ROM, DOL, extracted assets, generated game code,
keys, or firmware. Use only game data that you are legally entitled to use and do not
redistribute Nintendo content.

Project-owned source is GPL-3.0-or-later because the target links GPL-licensed upstream
CPU/runtime code. Third-party submodules retain their own copyright and license notices.
