# zm-build / zm-cmake / zm-make

Thin demo UX for shared-compute C/C++ builds. **Not** a second build system —
your `CMakeLists.txt` / `Makefile` stay unchanged. Compiles go through `recc`
to `zmd`'s REAPI front-end (`plugins/frontend-reapi`).

```bash
# Terminal A — donor/requester daemon with REAPI
ZM_ENABLE_REAPI=1 ZM_PLUGIN_PATH=./build ./build/zmd

# Terminal B — build as usual, via the mesh
zm-cmake -S . -B build -j 8
zm-make -C . -j 8
zm-build          # auto-detects CMakeLists.txt or Makefile
```

Requires: `zmd` with `ZM_ENABLE_REAPI=1`, `recc` (`scripts/install_recc.sh`),
and `cmake` / `make` as appropriate. Donors need a matching compiler (or use
`--env` / mirror once available for hermetic toolchains).
