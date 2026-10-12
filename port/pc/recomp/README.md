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
libultra internals not supplied by the modern runtime fail loudly if reached;
replacing those guards and providing production renderer, audio, and input
backends remain explicit porting work.

An experimental headless boot path validates the local ROM again, registers it
with N64ModernRuntime, and runs until the first unsupported runtime boundary or
for five seconds:

```sh
./build-pc-runtime/btga_runtime_probe --boot /path/to/battletanx_ga.z64
```

The current boot probe reaches five game-created N64 threads and a steady 60 Hz
VI loop. Over the default five-second window it processes 300 VI events, four
display lists, and four audio RSP tasks before exiting normally. It reports
thread, VI, display-list, screen-update, and RSP counts at the end of the run.
The renderer, controller, and audio callbacks are deliberately headless test
implementations at this stage.

`runtime_patches.cpp` contains two required portability patches. The
controller-read and scheduler-record busy-waits now cooperatively yield. Both
original loops depend on R4300 interrupt preemption, which a cooperatively
scheduled host thread cannot reproduce merely by translating instructions.

The audio microcode configuration uses its real IMEM load address (`0x1080`)
and declares the command-dispatch table's indirect handler destinations. The
generated implementation is derived locally from the verified ROM and remains
ignored, like all other ROM-derived output.

The configuration currently establishes the complete code-generation boundary.
Runtime integration still requires a real renderer (RT64 or equivalent), audio
output, input mapping, and a user-facing executable loop; this headless boot
milestone is not presented as a playable build.

`bootstrap_rt64.sh` pins the same RT64 revision used by the reference runtime
integration. It applies a small recorded patch that supplies a non-interactive
file-dialog backend, because choosing the ROM remains the launcher's job and
rendering must not require GTK or D-Bus. The renderer itself has been verified
to configure and build with `RT64_STATIC=ON`, `RT64_SDL_WINDOW_VULKAN=ON`, and
`NFD_STUB=ON`. It is kept separate from the headless probe until the interactive
SDL window and renderer adapter are enabled, so CI does not require a GPU.

The optional adapter can be compile-checked with:

```sh
cmake -S port/pc -B build-pc-rt64 -G Ninja \
  -DCMAKE_BUILD_TYPE=Release -DBTGA_BUILD_RECOMP=ON \
  -DBTGA_BUILD_RUNTIME_PROBE=ON -DBTGA_BUILD_RT64=ON
cmake --build build-pc-rt64 --target btga_rt64_renderer
```

This builds the full RT64 shader and Vulkan pipeline and the BattleTanx bridge
that connects RDRAM, RDP/VI registers, microcode selection, display lists, and
screen presentation. The remaining interactive step is the SDL window/input
loop. With `BTGA_BUILD_RT64=ON`, that loop is included in
`btga_runtime_probe` and can be launched with:

```sh
./build-pc-rt64/btga_runtime_probe --play /path/to/battletanx_ga.z64
```

The interactive mode creates a resizable Vulkan window, sends the game's
display lists through RT64, queues stereo audio through SDL, and supplies a
basic keyboard mapping: WASD for the analog stick, X/Z for A/B, Enter for
Start, and Q/E for L/R. The bounded `--boot` mode remains headless for CI.
