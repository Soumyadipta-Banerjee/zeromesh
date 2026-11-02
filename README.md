# ZeroMesh ⚡

ZeroMesh lets a group of people on the same LAN pool their idle Linux machines and offload
work to them with zero setup: install one daemon, join a team, done. It is the shared
plumbing (finding workers, moving data, sandboxing, not annoying machine owners) that every
distributed tool otherwise rebuilds. Domain tools such as a render farm or a parameter-sweep
runner are built on top as apps.

## Status: v1 built, v2 in progress

ZeroMesh uses a **core + plugin architecture**. See the
[design](docs/design/architecture.md) and
[PLAN.md](PLAN.md) for milestones and what is ticked. Continuing the work? Start with [docs/HANDOFF.md](docs/HANDOFF.md).
The v2 plan (data moved once, owner comfort, churn-proof completion) is in
[ZeroMesh v2 Blueprint.md](ZeroMesh%20v2%20Blueprint.md). The next step is the
[adapter platform](docs/design/adapter-platform.md): adapters describe items (a frame, a translation
unit, a case) and the daemon owns batching, environments, retries, caching, progress and reports.
It is designed, not built; PLAN.md M7–M11 track it.

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
- A Python SDK (`sdk/python`) and three apps built on it: `zm-render` (Blender), `zm-cfd` (OpenFOAM
  sweeps) and `zm-cc` (C/C++/Fortran compilation from a compilation database).
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
- The apps still build on the raw Task API, so each one carries its own chunking, event handling,
  output placement and failure parsing. The adapter platform that removes this is designed, not built.

Earlier DHT, distributed-shared-memory, GPU and torrent code is parked in [`frozen/`](frozen/README.md). None of it is built.

## Building

Requirements: Linux, a C++20 compiler (GCC ≥ 11 or Clang ≥ 13), Meson + Ninja, bubblewrap ≥ 0.12.0
(a setuid `bwrap` is refused), gRPC C++, protobuf, libsodium, OpenSSL and FUSE 3. The Python tests also need
pytest: `python3 -m venv ~/.venvs/zeromesh && ~/.venvs/zeromesh/bin/pip install './sdk/python[dev]'`, then run
`make test` with that venv's `bin` first on `PATH` (meson records the interpreter when it configures).

Running work also needs cgroup v2 and a systemd user manager (every current desktop distribution has
both): zmd runs all work in a cgroup delegated to it, whose memory the kernel holds to `max_memory_pct`
of RAM (75% by default, `~/.config/zeromesh/donor.toml`), always leaving 512 MiB for the system. Its systemd unit has `Delegate=`; started any
other way (a terminal, SSH, the tests), zmd asks the user's systemd for a delegated scope itself. A
node without one runs no items and `zm doctor` says why.

On Ubuntu, AppArmor blocks unprivileged user namespaces by default. Without this, donors fail every
sandboxed task with `bwrap: loopback: Failed RTM_NEWADDR` (shared compiles fall back to local or
fail with `--no-retry-local`):

```bash
sudo sysctl kernel.apparmor_restrict_unprivileged_userns=0
# persist: copy packaging/sysctl/99-zeromesh-userns.conf to /etc/sysctl.d/
```

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
apps/       zm-render, zm-cfd, zm-cc
packaging/  Debian and Arch packaging, systemd unit
scripts/    install helpers and demo scripts
tests/      unit, integration and acceptance tests
frozen/     parked modules: not built, kept for later milestones
docs/       design spec (docs/design) and team notes
```

## License

Apache License 2.0. See [LICENSE](LICENSE) and [NOTICE](NOTICE).
