# ZeroMesh ⚡

ZeroMesh lets a group of people on the same LAN pool their idle Linux machines and offload
work to them with zero setup: install one daemon, join a team, done. It is the shared
plumbing (finding workers, moving data, sandboxing, not annoying machine owners) that every
distributed tool otherwise rebuilds. Domain tools such as a render farm or a parameter-sweep
runner are built on top as apps.

## Status: under restructuring

ZeroMesh is moving to a **core + plugin architecture**. See the
[design](docs/superpowers/specs/2026-10-26-core-plugin-architecture-design.md) and
[PLAN.md](PLAN.md) for milestones and what is ticked.

**What works today:**
- A local content-addressed store (SHA-256) and directory tree hashing.
- A sandbox executor (bubblewrap, `SCHED_IDLE`).
- The `zmd` daemon: a gRPC `ClientApi` on an owner-only unix socket, a job manager for static
  task DAGs, an action cache, and local sandboxed execution.
- `zm run [-o PATH]... -- CMD...` (runs CMD on this machine with the current directory as
  input and writes its outputs back), `zm status`, `zm monitor` and `zm version`.

**What does not work yet:** running tasks on other machines, team security, and the SDK. These are milestones M2–M5. Earlier code for a REAPI server, FUSE filesystem, DHT,
distributed shared memory and GPU offload is parked in [`frozen/`](frozen/README.md). Most of
it was stubbed, and none of it is built.

## Building

Requirements: Linux, a C++20 compiler (GCC ≥ 11 or Clang ≥ 13), Meson + Ninja, bubblewrap,
gRPC C++, protobuf, libsodium and OpenSSL.

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
core/       the daemon's library: store, exec, net, node, plugin
plugins/    core plugins (env-host)
zmd/        the daemon executable
cli/        the zm command-line client
proto/      zeromesh.v1 gRPC contracts
tests/      unit and integration tests
frozen/     parked modules: not built, kept for later milestones
docs/       design spec and implementation plans
```
