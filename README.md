# ZeroMesh ⚡

ZeroMesh lets a group of people on the same LAN pool their idle Linux machines and offload
work to them with zero setup: install one daemon, join a team, done. It is the shared
plumbing (finding workers, moving data, sandboxing, not annoying machine owners) that every
distributed tool otherwise rebuilds. Domain tools such as a render farm or a parameter-sweep
runner are built on top as apps.

## Status: v1 built, v2 in progress

ZeroMesh uses a **core + plugin architecture**. See the
[design](docs/superpowers/specs/2026-10-26-core-plugin-architecture-design.md) and
[PLAN.md](PLAN.md) for milestones and what is ticked. Continuing the work? Start with [docs/HANDOFF.md](docs/HANDOFF.md).
The v2 plan (data moved once, owner comfort, churn-proof completion, each measured against HTCondor and BOINC) is in
[ZeroMesh v2 Blueprint.md](ZeroMesh%20v2%20Blueprint.md).

**What works today:**
- A local content-addressed store (SHA-256) and directory tree hashing.
- A sandbox executor (bubblewrap, `SCHED_IDLE`).
- The `zmd` daemon: a gRPC `ClientApi` on an owner-only unix socket, a job manager for static
  task DAGs, an action cache, and local sandboxed execution.
- `zm run [-o PATH]... -- CMD...` (runs CMD on this machine with the current directory as
  input and writes its outputs back), `zm status`, `zm monitor` (peers + task table that
  keeps WORKING/DONE rows), `zm team` and `zm version`.
- Team security and remote execution: team create/join, pinned mTLS, admission, and the NodeApi
  (Offer, Push, Pull, Run, Cancel, Drain). The placer picks local or remote donors and retries a
  task on another donor when one is lost. Covered by automated tests on one host.
- The `env-mirror` plugin (FUSE lazy fetch of the requester's binaries; needs `/dev/fuse`) and the
  `frontend-reapi` plugin (CMake builds through `recc`; enable with `ZM_ENABLE_REAPI=1`).
- A Python SDK (`sdk/python`) and two apps built on it: `zm-render` (Blender) and `zm-cfd` (OpenFOAM sweeps).
- The action-cache key includes a digest of the host toolchain, and a donor whose toolchain differs
  refuses the task, so a different compiler can't return a wrong cache hit.

**Not done yet:**
- The two-machine and three-machine acceptance runs ([`tests/acceptance/`](tests/acceptance/README.md)) have not
  been recorded on real hardware, so M3–M5 stay unticked in PLAN.md.
- There is no committed measurement of speedup, bytes moved or owner impact.
- The REAPI front-end keeps its action cache in memory and stubs `WaitExecution`.
- A departing donor (low battery, owner back, lid closed, suspend) freezes its tasks with `cgroup.freeze` for
  `freeze_ttl_s` instead of killing them, and tasks can save finished units with `commit_unit` so a retry runs
  only the rest. Both are covered by unit tests on one host only; no two-machine or measured result yet.

Earlier DHT, distributed-shared-memory, GPU and torrent code is parked in [`frozen/`](frozen/README.md). None of it is built.

## Building

Requirements: Linux, a C++20 compiler (GCC ≥ 11 or Clang ≥ 13), Meson + Ninja, bubblewrap ≥ 0.12.0
(a setuid `bwrap` is refused), gRPC C++, protobuf, libsodium, OpenSSL and FUSE 3. The Python tests also need
pytest: `python3 -m venv ~/.venvs/zeromesh && ~/.venvs/zeromesh/bin/pip install './sdk/python[dev]'`, then run
`make test` with that venv's `bin` first on `PATH` (meson records the interpreter when it configures).

```bash
# Arch
sudo pacman -S meson ninja bubblewrap grpc protobuf libsodium openssl python-grpcio python-grpcio-tools
# Ubuntu
sudo apt-get install meson ninja-build g++ pkg-config bubblewrap libgrpc++-dev protobuf-compiler-grpc \
  libprotobuf-dev protobuf-compiler libsodium-dev libssl-dev python3-grpcio python3-grpc-tools

make build     # meson setup + ninja
make test      # meson test (unit + integration)
./build/zmd &  # the daemon
./build/zm run -- echo hi
```

## Repository layout

```
core/       the daemon's library: store, exec, net, node, sched, plugin
plugins/    env-host, env-mirror, frontend-reapi
zmd/        the daemon executable
cli/        the zm command-line client
proto/      zeromesh.v1 gRPC contracts
sdk/        Python SDK (sdk/python)
apps/       zm-render, zm-cfd
packaging/  Debian and Arch packaging, systemd unit
scripts/    install helpers, demo scripts, baselines/ (rootless HTCondor for the v2 comparison)
tests/      unit, integration and acceptance tests
frozen/     parked modules: not built, kept for later milestones
docs/       design spec and implementation plans
```
