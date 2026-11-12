# Build requirements and reproducibility

## Included host-test recipe
`tests/run_host_tests.sh <pinned-rt64-3ds-src/renderer> <build-dir>` configures CMake with `RT64_3DS_RENDERER_DIR`, builds the host adapter test, then runs it. This recipe is present in source but was not executed for this handoff. It requires CMake 3.20+, a C++20 compiler, and the RT64-3DS renderer source files `rt64_3ds_dl.cpp` and `rt64_3ds_interp.cpp`.

The prior handoff identifies RT64-3DS revision `7c9f9b51aaabf55bc936641682ee666cf940a169`. That dependency is not included here; verify its license and availability before use.

## Not a device build
The included CMake project is explicitly for a host interpreter test, not an ARM11 application or game build. Historical separate runtime-baseline work used devkitARM/ARM11 tools and a user-supplied ROM, but its build tree, generated sources, ROM-backed objects, and scripts are excluded. Its prior build command is not a reproducible recipe for this candidate.

No game build or test was run while preparing this handoff.
