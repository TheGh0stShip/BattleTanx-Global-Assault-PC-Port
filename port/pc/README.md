# BattleTanx: Global Assault PC runtime

This directory is the separate native-port consumer of the recovered game. It
does not alter the matching N64 build and never embeds the ROM or extracted
assets.

Current milestone: a cross-platform executable bring-up slice with strict local
USA Rev 0 `.z64` verification, deterministic tank movement and firing, a
software framebuffer, and native X11/Win32 presentation. It is infrastructure
for the complete port, not yet the complete retail game.

```sh
cmake -S port/pc -B build-pc -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build-pc
ctest --test-dir build-pc --output-on-failure
./build-pc/battletanx_ga_pc /absolute/path/to/BattleTanxGA.z64
./build-pc/battletanx_ga_pc --headless /absolute/path/to/BattleTanxGA.z64
```

Controls: W/S drive, A/D turn, Space fires, Escape quits.

The next integration steps are the verified extracted-resource reader, N64
display-list translation, audio, and replacement of the bring-up simulation by
the recovered gameplay entrypoints.
