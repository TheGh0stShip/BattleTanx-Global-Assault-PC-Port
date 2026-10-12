# Static-recomp compatibility path

This path translates the verified ROM using the matching project's authoritative
function-boundary TOML with the official N64Recomp tool. Generated C and ROM-derived binary data are ignored and never
committed. It is the short path to complete original-game compatibility while
the native source/runtime path under `port/pc` replaces N64 services.

Bootstrap the pinned official recompiler, then generate the compatibility C:

```sh
port/pc/recomp/bootstrap.sh
port/pc/recomp/bootstrap_runtime.sh
port/pc/recomp/prepare.sh
```

The current catalogue generates all 1,879 functions into 38 translation units.
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

The probe registers the translated entrypoint but does not start the game. Ten
libultra internals not supplied by the modern runtime currently fail loudly if
reached; replacing those guards and providing renderer, RSP, audio and input
callbacks are explicit remaining boot work.

An experimental headless boot path validates the local ROM again, registers it
with N64ModernRuntime, and runs until the first unsupported runtime boundary or
for five seconds:

```sh
./build-pc-runtime/btga_runtime_probe --boot /path/to/battletanx_ga.z64
```

The configuration currently establishes the complete code-generation boundary.
Runtime integration still requires N64ModernRuntime/RT64 service glue and RSP
microcode recompilation; generated code alone is not presented as playable.
