# ZeroMesh: Living Plan

> **For developers and AI agents:** keep this file true. A box is ticked only when the
> milestone's acceptance test has passed. Never tick work that is stubbed or simulated.

**Design:** [`docs/superpowers/specs/2026-10-26-core-plugin-architecture-design.md`](docs/superpowers/specs/2026-10-26-core-plugin-architecture-design.md)
**Current implementation plan:** [`docs/superpowers/plans/2026-10-26-core-plugin-part1-m0-m3.md`](docs/superpowers/plans/2026-10-26-core-plugin-part1-m0-m3.md)

---

## 1. Architecture

ZeroMesh is a small, stable core (`zmd`) that pools idle Linux machines on a LAN. Domain
tools are **apps** built on a public client API. Gazebo is the structural model: a
small core, with features attached through narrow plugin interfaces.

```
   ┌──────────────────────── apps (out-of-process, any language) ────────────────────────┐
   │  zm CLI (run, run --each, status, monitor)   zm-render   (future: zm-sweep, zm-sim)  │
   └───────────────────────────────┬──────────────────────────────────────────────────────┘
                                   │ zeromesh.v1.ClientApi  (gRPC over a unix socket, owner-only)
   ┌───────────────────────────────▼─────────── zmd (daemon) ────────────────────────────┐
   │  node/      ClientApi service · NodeApi service · job manager                        │
   │  sched/     task graph · placement · retries · re-dispatch on drain/loss             │
   │  exec/      sandbox runner (bwrap + SCHED_IDLE)       ◄── IEnvProvider plugins       │
   │  store/     CAS + Merkle (Objects)                                                   │
   │  net/       discovery (team-scoped) · peer table · telemetry · secure transport      │
   │  plugin/    loader + interfaces                       ◄── IFrontend plugins          │
   └───────────────────────────────┬──────────────────────────────────────────────────────┘
                                   │ zeromesh.v1.NodeApi  (gRPC over mTLS, team members only)
                         other zmd nodes on the LAN
```

## 2. Module status

| Module | Status |
|---|---|
| CAS store + Merkle | Real (`core/store`) |
| Sandbox executor (bwrap, SCHED_IDLE) | Real (`core/exec`) |
| LAN discovery + peer table | Real but unauthenticated (`core/net`); not started by `zmd` until M3 |
| `zm` CLI + TUI (`cli/`) | Real; a ClientApi client (`run`, `status`, `monitor`, `version`) |
| `zmd` daemon, ClientApi, job manager, local execution | Real (`core/node`, `zmd/`); tests in `tests/integration/test_local_daemon.py` |
| NodeApi, placement, team security | **Not built yet** (M3) |
| FUSE filesystem | Frozen until M5 (`env-mirror` plugin) |
| REAPI front-end | Frozen until M6 (it was a print-only stub) |
| DHT, DSM, GPU, Byzantine/TPM, torrent GUI | Frozen; see [`frozen/README.md`](frozen/README.md) |

## 3. Milestones

Each milestone is done **only** when its acceptance test passes.

- [ ] **M0: Honesty pass.** *(Local build and tests green; tick once CI passes on GitHub.)* Rewrite PLAN.md and README.md to match the audit and the design. Move frozen modules out of the build. Make CI install gRPC and run the tests.
  *Acceptance:* CI green on a clean Ubuntu runner. PLAN.md has no `[x]` on unimplemented work. README describes only what works.
- [x] **M1: Contracts.** v1 protos, plugin interfaces, generated C++ and Python stubs.
  *Acceptance:* a Python script submits a 3-task job and receives results and events.
- [x] **M2: Core relocation.** Keep modules moved into `core/`. Real telemetry. Persistent node IDs. `zmd` runs as a long-lived daemon with a real `ClientApi` and local execution.
  *Acceptance:* `zm run -- echo hi` prints `hi` with exit 0. Two runs with different commands give two real results, with no false cache hits. Killing `zmd` and restarting it keeps the node ID.
- [ ] **M3: Remote execution + security.** Team create/join, admission, mTLS, `NodeApi`, blob transfer, `env-host` plugin, placement.
  *Acceptance:* **Two machines:** on A, `zm run --remote-only -- sh -c 'hostname > out.txt'` with output `out.txt` produces a file containing **B's** hostname. A third machine not in the team never appears in `zm status` and can't call `NodeApi`.
- [ ] **M4: Fan-out + resilience.** `zm run --each`, job DAGs, retries, drain/loss re-dispatch, donor policy file.
  *Acceptance:* `zm run --each 'cases/*' -- ./sim {}` with 20 CPU-bound cases (each ~30 s single-threaded, inputs under 10 MB) on 3 machines completes in under 45% of the single-machine time. Killing one donor's `zmd` mid-job still completes all 20 correctly.
- [ ] **M5: SDK + first app + mirror mode.** Python SDK, `zm-render`, and the `env-mirror` plugin.
  *Acceptance:* `zm-render shot.blend --frames 1-240` on 4 machines produces the same frames as a local render. A donor without Blender installed renders frames through mirror mode.
- [ ] **M6: REAPI front-end plugin.**
  *Acceptance:* a CMake project builds through `recc` against `zmd`, with compile actions observed running on a peer.

## 4. Architectural Decision Records

### ADR-011: Core + plugin architecture (2026-10-26)
- **Context:** The audit found the central piece (running a task on another machine) missing, while the leaf modules worked. Every domain tool (Flamenco, distcc, CFD sweeps) rebuilds the same plumbing.
- **Decision:** Build a small general core (Tasks, Objects, static DAGs) with in-process core plugins (`IEnvProvider`, `IFrontend`) and out-of-process apps on a public `ClientApi`. REAPI becomes one optional front-end plugin.
- **Consequence:** Domain logic stays out of `core/`. Apps can't reach internals. Several people can work in parallel against stable contracts.

### ADR-012: bwrap mount layout instead of stacked Landlock (2026-10-26)
- **Context:** The old design stacked Landlock with bubblewrap. The Linux kernel docs (v7.3.0) state: *"Threads sandboxed with filesystem restrictions cannot modify filesystem topology, whether via mount(2) or pivot_root(2)."* bwrap needs mount(2).
- **Decision:** bwrap's mount layout is the write boundary: inputs at `/work`, and only `/work` and `/tmp` are writable. `landlock.*` stays in the tree, unused.
- **Consequence:** The same write limits as intended, without a sandbox that can't start.

## 5. Historical ADRs (superseded by the 2026-10-26 spec)

### ADR-001 (Protocol Selection): Adopt Google/Linux Foundation REAPI v2
- **Context:** We need a standard remote execution wire protocol supported by industry build systems without requiring custom compiler plugins or proprietary agents.
- **Decision:** Adopt the standard Remote Execution API v2 (REAPI v2) over gRPC.
- **Consequence:** Immediate compatibility with existing tools like `recc` (CMake/Make/Meson), Bazel, and Buck2 out of the box.

### ADR-002: Hermetic Sandboxing (CAS-Materialized Toolchain + Minimal bwrap)
- **Context:** Developers on different Linux distros (e.g. Debian vs Arch vs Ubuntu) have conflicting glibc and system libraries. Blindly mounting host `/usr` and `/lib` leads to subtle ABI breaks and un-reproducible builds.
- **Decision:** Compiler toolchains and libraries must be materialized from CAS digests directly into the sandbox root. Only minimal pseudo-devices (`/dev/null`, `/dev/zero`, `/dev/urandom`) and an ephemeral memory `/tmp` scratch space are mounted. Unprivileged user namespaces (`bwrap`) + Landlock lockdown enforce isolation.
- **Consequence:** 100% hermetic and bit-for-bit reproducible cross-machine compilation without host distro/glibc pollution.

### ADR-003: Pure Local P2P Discovery & Sub-500ms Fast Preemption
- **Context:** Relying solely on passive heartbeat timeouts (e.g. 30s) leaves in-flight compilation jobs stranded when a donor laptop closes its lid, unplugs from AC, or experiences heavy interactive user load.
- **Decision:** Combine mDNS (`_zeromesh._tcp`) with an explicit high-priority `NodeDraining` / `Preempt` multicast signal. When donor battery drops below threshold, CPU thermal throttling engages, or user interaction begins, the daemon immediately broadcasts a preemption packet.
- **Consequence:** In-flight tasks are gracefully cancelled on the donor and re-queued by the requester within < 500ms.

### ADR-004: Cache-First Short-Circuit Execution
- **Context:** Invoking the scheduler and querying peer topology for previously compiled actions wastes network round-trips and CPU cycles.
- **Decision:** Enforce an immediate `ActionCache` lookup in `ExecutionService::Execute` before invoking the scheduler. If the Action digest matches an existing entry, immediately return the cached `ActionResult`.
- **Consequence:** Instantaneous zero-overhead cache hits (< 1ms) for repeated builds across the team.

### ADR-005: Host Quality of Service (SCHED_IDLE & Interactive Preemption)
- **Context:** Donating CPU cycles must be completely imperceptible to laptop owners.
- **Decision:** Run all external worker processes under Linux `SCHED_IDLE` (or maximum niceness `nice 19`) and trigger immediate `NodeDraining` upon keyboard/mouse activity or low power.
- **Consequence:** Frictionless adoption among team members as donor machines never freeze or drain unexpectedly.

### ADR-006: BitTorrent-Style Kademlia DHT for Trackerless Chunk Location
- **Context:** Centralized metadata servers or coordinators create a single point of failure and violate the serverless zero-config philosophy.
- **Decision:** Adopt a 256-bit BLAKE3 Kademlia DHT with $k$-buckets. Instead of transferring file chunk tables centrally, nodes register provider records (`ANNOUNCE_CHUNK`) and query `GET_PROVIDERS`.
- **Consequence:** 100% trackerless, peer-to-peer chunk discovery with $O(1)$ LAN shortcuts and $O(\log N)$ scalability across subnets.

### ADR-007: Distributed Secure FUSE-FS (`zm-fs`) with Lazy Block Faulting
- **Context:** Transferring entire source directories before starting compilation saturates network links.
- **Decision:** Implement a user-space virtual filesystem using `fuse3`. Only the 512 KB chunks touched during compilation are pulled on-demand over QUIC. Chunks are encrypted with ChaCha20-Poly1305.
- **Consequence:** Near-instantaneous compilation startup without waiting for multi-gigabyte source trees to sync.

### ADR-008: Standalone Network Distributed Shared Memory C-Library (`libzmdsm`)
- **Context:** High-performance computing, scientific modeling, and distributed parameter sweeps require shared memory state without the complexity and code changes demanded by MPI.
- **Decision:** Provide a native C library (`#include <zmdsm.h>`) implementing virtual memory page-fault traps (`userfaultfd`/`mprotect`), twin-page bitwise diffs, and barrier/lock synchronization over TCP/QUIC.
- **Consequence:** Developers can write multi-machine parallel software using standard C pointers and shared arrays.

### ADR-009: 2% Byzantine Spot-Check Quorums for Cache Poisoning Prevention
- **Context:** In open or ad-hoc P2P networks, a faulty or compromised donor node could return poisoned build artifacts or corrupted compiler outputs to the Action Cache.
- **Decision:** The scheduler randomly dispatches 2% of all tasks to an independent second node in the background. If the resulting output digest diverges from the primary, the result is discarded and the rogue node is quarantined.
- **Consequence:** Robust Byzantine fault tolerance and cache integrity with only 2% compute overhead.

### ADR-010: Heterogeneous Accelerator Offloading (`wgpu` & Unified Memory)
- **Context:** Modern developer laptops include high-performance GPUs and unified memory (Apple Silicon, NVIDIA RTX) that idle during CPU compilation.
- **Decision:** Implement a heterogeneous compute channel using `wgpu` (Vulkan/Metal/DX12) and CUDA, with a dynamic FLOP/byte bandwidth guard to ensure only compute-heavy kernels are offloaded.
- **Consequence:** ZeroMesh expands beyond CPU compilation into distributed AI, simulations, and shader builds.
