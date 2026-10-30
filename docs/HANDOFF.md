# Handoff (updated 2026-10-29)

For the next person or agent continuing this work. Read this first, then the
[design](superpowers/specs/2026-10-26-core-plugin-architecture-design.md), [PLAN.md](../PLAN.md) and the
[v2 blueprint](../ZeroMesh%20v2%20Blueprint.md).

## Where things are

- **Branch:** `demo/m5-all` holds the v1 code through M6. `main` is the old architecture.
- **Build and test:**

  ```bash
  python3 -m venv ~/.venvs/zeromesh && ~/.venvs/zeromesh/bin/pip install './sdk/python[dev]'
  export PATH=~/.venvs/zeromesh/bin:$PATH   # meson records the interpreter when it configures
  make build && make test
  ```

  `make test` runs 10 meson tests: 171 unit tests plus the local, remote, fan-out, mirror, REAPI, SDK, `zm-render`
  and `zm-cfd` suites. All 10 pass on a host with `/dev/fuse`, `recc`, `bwrap` ≥ 0.12.0 and Blender.
- **Style:** `.clang-format` encodes the house style; `clang-format -i` any C++ you touch. `.clang-tidy` is a
  small bugprone/performance set: `clang-tidy -p build <file>`.
- **Baselines for the v2 comparison:** `scripts/baselines/htcondor.sh up [tuned]` starts a rootless HTCondor pool in
  Podman (`docker.io/htcondor/mini`), `submit FILE` runs a job, `down` removes it. The "BOINC-style" baseline is a
  policy in the harness (suspend on input, quorum 1 or 2), not the BOINC client.

## Status against PLAN.md

| Milestone | Status |
|---|---|
| M0 Honesty pass | Docs now match the code; tick once CI passes on GitHub |
| M1 Contracts, M2 Core + local daemon | Done |
| M3 Remote execution + security | Built and tested on one host; **two-machine acceptance pending** |
| M4 Fan-out + resilience | Built and tested (retries on node loss, sticky drain); three-machine acceptance pending |
| M5 SDK, `zm-render`, `env-mirror` | Built and tested; the 240-frame, 4-machine run is pending |
| M6 REAPI front-end | Done: `integration_reapi` passes with `recc` (both daemons on one host) |

## Known gaps

- No committed measurement of speedup, bytes moved or owner impact. This is what the v2 blueprint is for.
- Drain on low battery kills tasks; no `cgroup.freeze`, no partial results (`core/node/daemon.cpp`).
- The REAPI front-end keeps its action cache in memory and stubs `WaitExecution`.
- Plugin modules (`libzm_frontend_reapi.so`, `env-host`) are built but not installed by `meson install`, so the
  Debian and Arch packages can't ship REAPI yet.
- `zmd` is linked with `export_dynamic` so `dlopen`ed plugins can resolve the `zeromesh.v1` protobuf symbols.
  Without it the REAPI plugin fails to load, and the loader does not log why.
- No Landlock: bwrap's mount layout is the write boundary (ADR-012 in PLAN.md).
- Recorded in the 2026-10-26 ledger and not re-checked since: the keepalive-timeout path has no test, the peer RAM
  column shows 0 (`mem_total` is not in the discovery wire format), and `MerkleBuilder::Overlay` stores overlaid
  single files with `is_executable=false`.

## Where the record lives

- **Commit history** on `demo/m5-all`.
- **Decision ledger** for Part 1: `.superpowers/sdd/2026-10-26-core-plugin-part1-m0-m3/progress.md` in the original
  worktree. It is git-ignored scratch, so copy the rulings out if you delete that worktree.
