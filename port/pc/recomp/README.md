# Static-recomp compatibility path

This path translates the verified ROM using the matching project's authoritative
function-boundary TOML with the official N64Recomp tool. Generated C and ROM-derived binary data are ignored and never
committed. It is the short path to complete original-game compatibility while
the native source/runtime path under `port/pc` replaces N64 services.

Bootstrap the pinned official recompiler, then generate the compatibility C:

```sh
port/pc/recomp/bootstrap.sh
port/pc/recomp/prepare.sh
```

The current catalogue generates all 1,879 functions into 38 translation units.
Six N64-only cache/exception routines are explicitly stubbed; their services
belong to the host runtime rather than translated game code.

The configuration currently establishes the complete code-generation boundary.
Runtime integration still requires N64ModernRuntime/RT64 service glue and RSP
microcode recompilation; generated code alone is not presented as playable.
