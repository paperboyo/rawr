# Host behavior tests

These tests exercise results and data contracts, not source-file layout.
They need CMake, a C++20 compiler, Vulkan headers, uv, and FFmpeg. Python checks run through `uv run`.

```sh
cmake -S tests -B tmp/host-tests -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build tmp/host-tests --parallel 4
ctest --test-dir tmp/host-tests --output-on-failure
```

The existing `mov_muxer` scenarios also check all ten date fields and the keyed
creation date: MP4/MOV, representative UTC offsets, one 64-bit date-header case,
and interrupted recovery with a partial index tail and GPS/LOG coexistence.

For memory checks, configure a separate build with `-DRAWR_SANITIZERS=ON`.
Native libraries also retain their CMake test targets and reference fixtures;
GPU tests require a suitable Vulkan implementation. Kotlin tests run via Gradle.
