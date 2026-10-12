# BattleTanx: Global Assault — native PC port

[![PC: Windows playable](https://img.shields.io/badge/PC-Windows%20playable-brightgreen)](port/pc/README.md)
[![Decompilation: 100%](https://img.shields.io/badge/decompilation-100%25-brightgreen)](https://decomp.dev/TheGh0stShip/BattleTanx-Global-Assault)
[![Functions: 1879/1879](https://img.shields.io/badge/functions-1879%2F1879-brightgreen)](https://decomp.dev/TheGh0stShip/BattleTanx-Global-Assault)
[![License: GPL-3.0](https://img.shields.io/badge/license-GPL--3.0-blue)](COPYING)

This branch contains a playable native PC port of **BattleTanx: Global
Assault**, built from the completed matching Nintendo 64 decompilation. The
Windows runtime boots the original game, renders through RT64/Direct3D 12,
plays audio through SDL2, supports keyboard and SDL game controllers, and has
been played through the original menus into campaign gameplay. Linux uses
RT64's Vulkan backend.

The port uses N64Recomp for the recovered CPU program, RSPRecomp for audio
microcode, N64ModernRuntime for N64 operating-system services, and RT64 for
display-list rendering. It is a native host executable—not an emulator bundle.
The completed byte-matching decompilation remains in this repository as the
auditable source and compatibility foundation.

No ROM or ROM-derived assets are included. Supply your own legally obtained dump.

## Playable PC port demonstration

[![BattleTanx: Global Assault native Windows port demonstration](https://img.youtube.com/vi/PrhwXBr1qJI/maxresdefault.jpg)](https://www.youtube.com/watch?v=PrhwXBr1qJI)

The video above demonstrates the game running as a native Windows executable
from this branch: original menus and campaign gameplay,
RT64 Direct3D 12 rendering, SDL audio, and live keyboard input. Click the image
to play the full demonstration on YouTube.

## PC port status

- Native Windows x64 executable with Direct3D 12 rendering.
- Native Linux build path with Vulkan rendering.
- Original menus and campaign gameplay are playable.
- Keyboard and SDL game-controller input, rumble, and SDL audio are connected.
- Save data is kept in a stable per-user directory and window close requests
  an orderly runtime shutdown.
- RT64's live developer inspector is available by setting
  `BTGA_RT64_DEVELOPER=1` and pressing F1.
- Distribution packages are ROM-free; the player supplies a verified local
  USA Rev 0 `.z64` dump at launch.

This is still a development port. Long-session stability, every game mode,
representative GPUs/controllers, save persistence, and visual effects remain
part of the release validation matrix.

## Build and run the PC port

The complete native-runtime guide is in [port/pc/README.md](port/pc/README.md).
On Windows, use a native NTFS checkout with Visual Studio's **Desktop
development with C++** workload. After generating the local translated inputs
and bootstrapping the pinned dependencies described in
[port/pc/recomp/README.md](port/pc/recomp/README.md):

```powershell
Set-ExecutionPolicy -Scope Process Bypass
./port/pc/build-windows.ps1 -Rom 'C:\Games\BattleTanxGA.z64' -Run
```

Create a ROM-free distributable ZIP with:

```powershell
./port/pc/package-windows.ps1 -BuildDirectory build-pc-windows -Version 0.1.0
```

Controls, Linux/headless commands, save locations, renderer diagnostics, and
packaging contents are documented in the [PC runtime guide](port/pc/README.md).

## Supported ROM

- Title: `BATTLETANXGA`
- Region/revision: USA, Rev 0 (`NBQE`)
- Format: big-endian `.z64`
- Size: 8 MiB
- SHA-1: `805248fb0a0ee694cad8d7dc927b631d860dd8cf`
- SHA-256: `88268ce770ff39f2ca839848b842c87866df6c5ddaf775f180b2522c18ff6221`

## Reproduce the original N64 build from your own ROM

```sh
python3 tools/reproduce.py /absolute/path/to/BattleTanxGA.z64
```

The input must be an existing local big-endian `.z64` dump. The command
verifies it before use, installs pinned local toolchains, compiles the recovered
source, extracts the complete ignored resource tree, rebuilds the full 8 MiB
cartridge image, and runs the code, asset, hash, test, and repository-safety
gates. Generated assembly is intentionally ignored; reviewed C, authentic RSP
assembly, and project metadata are the source of truth.

See [the complete reproduction and resource guide](docs/REPRODUCING.md) for
individual stages, output paths, editing/repacking commands, prerequisites,
and the reusable region-manifest interface.

For native-port resource packaging, see the [O2R runtime-resource
path](docs/O2R_PORT_PATH.md). It records what is genuinely structured today
and prevents container-only `.o2r` claims without runtime resource factories.

## Matching-decompilation status

- **Code: 100%.** All 1,879 catalogue functions and all 632,132 function-body bytes are represented by production C or authentic source assembly.
- **Data: 100%.** All 115,617 tracked initialized/read-only bytes in the loaded executable scope are source-owned and matched.
- **Binary gate: exact.** `make verify-code` reconstructs ROM offsets `0x000000–0x101000` byte-for-byte, including the header, IPL3, loaded program, RSP boot, F3DEX FIFO 2.07, and n_audio microcode.
- **Whole-ROM build: exact.** The compiled `0x101000`-byte prefix and locally
  extracted resource regions assemble into an 8 MiB image with SHA-256
  `88268ce770ff39f2ca839848b842c87866df6c5ddaf775f180b2522c18ff6221`.
- The function catalogue, corrected boundaries, compiler identifications, imported-source provenance, and normalizer-assisted exceptions are documented and reproducible.
- The matching LZARI codec and audited ROM tables identify 271 compressed streams: 75 game-world bundles, 195 images, and one leftover BattleTanx world. Their 1,174,975 stored bytes expand to 2,886,765 bytes and canonically re-encode to every retail byte. `tools/inventory_lzari_assets.py` inventories them, `tools/extract_lzari_images.py` converts all six observed N64 texture layouts into local PNG previews, and `tools/extract_lzari_worlds.py` emits validated fixed-record world manifests without committing ROM-derived data.
- The complete loading path is reconstructed in matching C: individual asset DMA/loading, the eight-function LZARI decoder, both world loaders, relocation walkers, and the compressed-bank directory. `make verify-assets` independently parses and losslessly rebuilds every known resource family while accounting for all 8 MiB of the retail ROM.
- The complete 26-file libmus store is independently validated and can be losslessly split/rebuilt locally: 76 SFX waves, 93 effects, 231 music waves, and 21 songs occupying 2,289,292 indexed ROM bytes plus 66 alignment bytes. All 307 non-overlapping ADPCM waves are independently replaceable and inspection-decodable; `tools/pack_libmus_assets.py` reconstructs both large wave banks and the complete store without opaque fallback slices.
- Normalizer-assisted units require narrowly gated, documented rules with exact fire counts; they are identified separately from pure source matches in [the normalizer-assisted record](docs/NORMALIZER_ASSISTED.md).
- CI publishes Code and Data categories in the objdiff v2 `us_report` artifact. Data progress counts only source-owned bytes already proven identical; merely locating or extracting an opaque asset does not count as a match.

### What “100%” means

The completion claim covers the repository's defined matching-decompilation
scope: the 1,879-function catalogue and tracked initialized data in the loaded
N64 program image. Remaining generated `asm` segments are data, alignment,
RSP/microcode material, or binary regions intentionally outside the C-function
catalogue—not missing gameplay functions. Semantic renaming can continue
without changing the 100% binary-match result.

`make assembly-report` currently finds no remaining full generated-code
candidate. Its generated rows classify as embedded data (61), tiny ambiguous
fragments without a function return (17), mixed code/data boundary fragments
(3), RSP/microcode material (1), or authentic source assembly (24). The
ambiguous fragments stay visible until their exact ownership is proven; they
are not counted as functions or silently discarded as padding.

### Next phase

The matching milestone is complete. This repository's next maturity phase is
to improve source comprehension without weakening provenance: recover original
translation units, consolidate types and interfaces, and replace address names
only when behavior, callers, data flow, or recovered symbols support the name.
See the [source-maturity and naming policy](docs/SOURCE_MATURITY.md).

A wholesale function-by-function rename based only on resemblance to another
routine is deliberately out of scope. Similar code is useful evidence, but it
does not prove identical purpose, ownership, or an original name. Directory
moves follow proven library, translation-unit, or subsystem boundaries and are
made incrementally so each change remains reviewable and byte-verifiable.

The downstream portable runtime and PS Vita target must preserve the original
32-bit game semantics, use the ARMv7-A ILP32 toolchain, verify hard-float/softfp
ELF compatibility, and pass supervised physical-hardware testing before any
release claim. See the
[architecture and portability contract](docs/ARCHITECTURE.md). The generated
[native-port blocker inventory](docs/PORTABILITY_INVENTORY.md) identifies the
MIPS/RSP assembly, MMIO, platform services, display-list construction, and
matching-only register constraints that need adapters or portable replacements.
The [native-port architecture](docs/NATIVE_PORT.md) defines the dual build
pipelines, platform interface, data rules, staged Windows/Vita bring-up, and
acceptance criteria.

## Repository map

- `src/code/<subsystem>/` — recovered game source. Identified units use
  `ADDRESS_descriptive_name.c`; genuinely anonymous units use `ADDRESS.c` in
  `src/code/unknown/`. `config/source_layout.tsv` is the authoritative path and
  address catalogue. Run `make check-source-names` to enforce it.
- `src/libmus/`, `src/n_audio/`, and `src/libultra/` — recovered middleware and
  SDK runtimes, kept separate from game-owned source and retaining authentic
  upstream-style names where known.
- `src/asm/` — reviewed source assembly such as RSP and microcode material;
  generated assembly is ignored.
- `config/us/` — ROM layout, symbol, ownership, and matching-evidence tables.
- `decompkit/` — reusable Python library for ROM regions and resource tooling.
- `tools/` — project-facing build, extraction, verification, and reporting
  entry points.
- `tests/` — portable synthetic tests that do not require copyrighted assets.
- `docs/` — architecture, compiler, matching, provenance, asset-format, and
  tooling references.

One incidental discovery from the source recovery: rodata at `0x80072694`
contains developer-credit Easter-egg strings including “Murray likes fries.”

See also the [reusable decompilation toolkit](docs/DECOMPKIT.md),
[reproduction and resource guide](docs/REPRODUCING.md),
[tool and test map](docs/TOOLS.md),
[source-maturity and naming policy](docs/SOURCE_MATURITY.md),
[native-port architecture](docs/NATIVE_PORT.md),
[reproducible matching research](docs/MATCHING_RESEARCH.md),
[library fingerprints](docs/LIBRARIES.md), [asset formats](docs/ASSET_FORMATS.md),
[compiler notes](docs/COMPILER.md), and
[research provenance](docs/PROVENANCE.md).

## License

Project-authored code and imported GPL research are licensed under GPL-3.0; see `COPYING`. BattleTanx: Global Assault and its game data remain the property of their respective rights holders.
