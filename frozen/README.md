# Frozen modules

These modules are kept in git but are **not built or tested**. They were frozen on
2026-10-26 when ZeroMesh moved to the core + plugin architecture
(`docs/superpowers/specs/2026-10-26-core-plugin-architecture-design.md`).
Most of them were stubs or local-only simulations at the time; see the spec's §3 audit.

| Module | Why frozen | What would unfreeze it | Revived in |
|---|---|---|---|
| `vfs/` (FUSE + chunk fetcher) | Real code, but coupled to the DHT; not needed until mirror mode | Swap the DHT for an `IChunkSource` backed by `NodeApi.PullBlobs` from the requester | M5, as `plugins/env-mirror` |
| `reapi/` | Print-only stub; no gRPC server ever listened | A real REAPI v2 gRPC front-end on top of `ClientApi` | M6, as `plugins/frontend-reapi` |
| `scheduler/` | Compile-specific heuristics; remote branch ran locally | The linker-pinning heuristic moves into the REAPI front-end | M6 (heuristic only) |
| `p2p_syncer/` | Client for a `GET_CAS` protocol nothing served | Replaced by `NodeApi.PushBlobs/PullBlobs` | Not revived |
| `dht/` | Every node used the same ID; not needed on a single LAN | Team-wide cache or chunk location across subnets | v2, if ever |
| `dsm/`, `include/zmdsm.h` | Local-only `mmap` + `memcpy`; page-level DSM collapses over commodity networks | A measured workload that needs it | Not planned |
| `gpu/` | Header only | A GPU environment provider plugin | v2+ |
| `verify/` | "Signatures" were plain strings | Real signing once results are shared team-wide | v2+ |
| `examples/` (GTK torrent GUI) | Side project outside the roadmap | — | Not planned |
| `tests/` | Tests for the modules above | Moves back with its module | — |
