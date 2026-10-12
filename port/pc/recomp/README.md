# Static-recomp compatibility path

This path translates the verified ROM using the matching project's authoritative
function-boundary TOML with the official N64Recomp tool. Generated C and ROM-derived binary data are ignored and never
committed. It is the short path to complete original-game compatibility while
the native source/runtime path under `port/pc` replaces N64 services.

Bootstrap the pinned official recompiler, then generate the compatibility C:

```sh
port/pc/recomp/bootstrap.sh
port/pc/recomp/bootstrap_runtime.sh
port/pc/recomp/bootstrap_rt64.sh
port/pc/recomp/prepare.sh
```

The current catalogue generates all 1,879 functions into 38 translation units,
plus the game's `n_aspMain` audio microcode through RSPRecomp.
Six N64-only cache/exception routines are explicitly stubbed; their services
belong to the host runtime rather than translated game code.

Compile-check every generated translation unit against the runtime ABI:

```sh
cmake -S port/pc -B build-pc-recomp -G Ninja \
  -DCMAKE_BUILD_TYPE=Release -DBTGA_BUILD_RECOMP=ON
cmake --build build-pc-recomp --target btga_recompiled_game
```

This produces a host static library and deliberately does not embed the ROM or
generated files in the repository. The ordinary native prototype build keeps
`BTGA_BUILD_RECOMP` disabled and therefore has no dependency on these inputs.

The next integration gate links that complete library with the pinned official
N64ModernRuntime checkout:

```sh
cmake -S port/pc -B build-pc-runtime -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DBTGA_BUILD_RECOMP=ON -DBTGA_BUILD_RUNTIME_PROBE=ON
cmake --build build-pc-runtime --target btga_runtime_probe
./build-pc-runtime/btga_runtime_probe
```

Without `--boot`, the probe registers the translated entrypoint and exits. Ten
libultra internals not supplied by the modern runtime fail loudly if reached.
The interactive build supplies the production renderer, audio, and input
backends used by the playable Windows/Linux runtime.

An experimental headless boot path validates the local ROM again, registers it
with N64ModernRuntime, and runs until the first unsupported runtime boundary or
for five seconds:

```sh
./build-pc-runtime/btga_runtime_probe --boot /path/to/battletanx_ga.z64
```

The current boot probe reaches five game-created N64 threads and a steady 60 Hz
VI loop. Over the default five-second window it processes about 300 VI events
and roughly 150 display lists/screens and audio RSP tasks before exiting
normally. Exact counts can vary by a frame. It reports thread, VI,
display-list, screen-update, and RSP counts at the end of the run. The bounded
probe deliberately uses headless callbacks; `--play` selects the interactive
RT64/SDL backends.

For a longer stability run, select an explicit duration from one second through
one hour:

```sh
./build-pc-runtime/BattleTanxGA --boot-seconds 300 /path/to/battletanx_ga.z64
```

`runtime_patches.cpp` contains required scheduler portability patches. The
controller, scheduler-record, graphics-completion, and render-slot busy-waits
now cooperatively yield. Those original loops depend on R4300 interrupt
preemption, which a cooperatively scheduled host thread cannot reproduce merely
by translating instructions.

It also corrects the shield-hit flash lifetime at its truncation boundary. The
original subtracts the integer part of a frame step that is just over 1.0 on
N64 hardware; at an exact host 60 Hz it can remain just below 1.0 and truncate
to zero. Without the site-specific fractional-step hook, translucent shield
impact shards never expire and accumulate around vehicles. The diagnosis was
independently confirmed against the GPL-3.0 `wootbeer/btgarecomp` compatibility
work (commit `12dfa75`).

The same frame-step boundary affects one AI turn limit, one tracking deadline,
and four results-screen counters. Their narrowly located hooks retain the real
fractional elapsed time (carrying fractions for the integer deadline) so host
timing cannot freeze those systems when a cast would otherwise produce zero.

The audio microcode configuration uses its real IMEM load address (`0x1080`)
and declares the command-dispatch table's indirect handler destinations. The
generated implementation is derived locally from the verified ROM and remains
ignored, like all other ROM-derived output.

The configuration establishes the complete code-generation boundary. The
interactive runtime now supplies RT64 rendering, SDL audio, keyboard/controller
input, rumble, and a user-facing executable loop. The headless path remains a
bounded CI and diagnostics mode.

`bootstrap_rt64.sh` pins the RT64 revision used by the runtime integration. It
applies a small recorded patch that supplies a non-interactive file-dialog
backend, because choosing the ROM remains the launcher's job and rendering must
not require GTK or D-Bus. The renderer builds with `RT64_STATIC=ON`,
`RT64_SDL_WINDOW_VULKAN=ON`, and `NFD_STUB=ON`. RT64 remains optional at CMake
configure time so headless CI does not require a GPU.

The RT64 adapter can be built with:

```sh
cmake -S port/pc -B build-pc-rt64 -G Ninja \
  -DCMAKE_BUILD_TYPE=Release -DBTGA_BUILD_RECOMP=ON \
  -DBTGA_BUILD_RUNTIME_PROBE=ON -DBTGA_BUILD_RT64=ON
cmake --build build-pc-rt64 --target btga_rt64_renderer
```

This builds the full RT64 shader pipeline and the BattleTanx bridge
that connects RDRAM, RDP/VI registers, microcode selection, display lists, and
screen presentation. Windows selects D3D12 and Linux selects Vulkan. With
`BTGA_BUILD_RT64=ON`, the SDL window/input loop is included in the target and
can be launched with:

```sh
./build-pc-rt64/BattleTanxGA --play /path/to/battletanx_ga.z64
```

The interactive mode creates a resizable native window, sends the game's
display lists through RT64, queues stereo audio through SDL, and supplies a
basic keyboard mapping: WASD for the analog stick, X/Z for A/B, left Shift for
Z, Enter for Start, Q/E for L/R, arrows for the D-pad, and IJKL for C buttons.
SDL game controllers use the left stick, face/shoulder buttons, left trigger,
D-pad, and rumble. The bounded `--boot` mode remains headless for CI.

Save data is written outside the build/install tree: under
`%LOCALAPPDATA%\BattleTanxGA` on Windows, or
`$XDG_DATA_HOME/BattleTanxGA` (falling back to
`~/.local/share/BattleTanxGA`) on Linux. Closing the window requests an orderly
runtime shutdown so pending EEPROM writes can be flushed.

For renderer diagnosis, set `BTGA_RT64_DEVELOPER=1` and press F1 during play.
Set `BTGA_RT64_INSPECTOR=1` to open the inspector at startup. Normal launches
leave RT64 developer mode disabled.
