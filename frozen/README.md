# Frozen modules

These modules are kept in git but are **not built or tested**. They were frozen on
2026-10-26 when ZeroMesh moved to the core + plugin architecture
(`docs/superpowers/specs/2026-10-26-core-plugin-architecture-design.md`).
Most of them were stubs or local-only simulations at the time; see the spec's §3 audit.

| Module | Why frozen | What would unfreeze it | Revived in |
|---|---|---|---|
| `vfs/` (FUSE + chunk fetcher) | Real code, but coupled to the DHT; not needed until mirror mode | Swap the DHT for an `IChunkSource` backed by `NodeApi.PullBlobs` from the requester | M5, as `plugins/env-mirror` |
| `p2p_syncer/` | Client for a `GET_CAS` protocol nothing served | Replaced by `NodeApi.PushBlobs/PullBlobs` | Not revived |
| `dht/` | Every node used the same ID; not needed on a single LAN | Team-wide cache or chunk location across subnets | v2, if ever |
| `dsm/`, `include/zmdsm.h` | Local-only `mmap` + `memcpy`; page-level DSM collapses over commodity networks | A measured workload that needs it | Not planned |
| `gpu/` | Header only | A GPU environment provider plugin | v2+ |
| `verify/` | "Signatures" were plain strings | Real signing once results are shared team-wide | v2+ |
| `examples/` (GTK torrent GUI) | Side project outside the roadmap | — | Not planned |
| `tests/` | Tests for the modules above | Moves back with its module | — |

The REAPI v2 front-end and linker-pinning heuristic were revived for M6 in
`plugins/frontend-reapi/`; the former `reapi/` and `scheduler/` frozen directories have
been removed.
