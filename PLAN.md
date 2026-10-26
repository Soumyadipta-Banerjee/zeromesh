# ZeroMesh Architecture & Living Execution Plan

> **Note for AI Agents & Developers:** Keep this document continuously updated. When you complete an objective or discover an architectural requirement, update the statuses (`[ ]` -> `[x]`), refine module contracts, and record design decisions in the ADR log at the bottom.

---

## 1. High-Level Architectural Design

ZeroMesh operates as a single binary daemon (`zmd` / `zm`) installed on each machine. It functions simultaneously as a **Client Proxy**, a **REAPI Service Provider**, a **P2P CAS Node**, and an **Unprivileged Hermetic Executor**.

```text
+---------------------------------------------------------------------------------+
|                                  Host Machine                                   |
|                                                                                 |
|  [ Build Tool ] (CMake + recc / Bazel / Buck2 / Meson / zm run)                 |
|         |                                                                       |
|         v (Standard REAPI v2 gRPC: localhost:8980)                              |
|  +---------------------------------------------------------------------------+  |
|  |                          ZeroMesh Daemon (zmd)                            |  |
|  |                                                                           |  |
|  |  +------------------------------+     +--------------------------------+  |  |
|  |  |      REAPI gRPC Gateway      |     |    Locality-Aware Scheduler    |  |  |
|  |  | - ExecutionService           | <-> | - Graph DAG analysis           |  |  |
|  |  | - CASService                 |     | - Network latency & load weight|  |  |
|  |  | - ActionCacheService         |     | - Critical path local pinning  |  |  |
|  |  +------------------------------+     +--------------------------------+  |  |
|  |                 |                                     |                   |  |
|  |                 v                                     v                   |  |
|  |  +------------------------------+     +--------------------------------+  |  |
|  |  |     P2P CAS (Blob Engine)    |     |    Sandboxed Task Executor     |  |  |
|  |  | - SHA-256 Merkle Store       |     | - Bubblewrap (bwrap) isolation |  |  |
|  |  | - Peer-to-peer blob transfer |     | - Landlock filesystem lockdown |  |  |
|  |  | - Missing chunk puller       |     | - SCHED_IDLE task priority     |  |  |
|  |  +------------------------------+     | - Battery / Thermal preemption |  |  |
|  |                 |                     +--------------------------------+  |  |
|  |                 v                                     |                   |  |
|  |  +------------------------------+                     v                   |  |
|  |  |     mDNS Peer Mesh Engine    |         Isolated Linux Subprocess       |  |  |
|  |  | - Node discovery & heartbeat |        (No host/network access)         |  |  |
|  |  | - Hardware telemetry gossip  |                                         |  |  |
|  |  +------------------------------+                                         |  |
|  +---------------------------------------------------------------------------+  |
+---------------------------------------------------------------------------------+
                                     |
               (Encrypted LAN P2P Traffic: mTLS / Noise)
                                     v
+---------------------------------------------------------------------------------+
|                                 Donor Peer 1..N                                 |
| - Identical zmd daemon running in background                                    |
| - Contributes idle CPU cycles automatically via SCHED_IDLE                      |
+---------------------------------------------------------------------------------+
```

---

## 2. Component Design & Responsibilities

| Subsystem | Primary Responsibilities | Core Dependencies | Key Files |
| :--- | :--- | :--- | :--- |
| **`discovery/`** | Broadcast node capability over mDNS (`_zeromesh._tcp`); maintain dynamic peer routing table, heartbeat pings, and hardware telemetry (cores, load, battery). | Avahi / mDNS socket, POSIX sockets | `src/discovery/mdns.hpp`, `src/discovery/peer_table.hpp` |
| **`reapi/`** | Standard REAPI v2 gRPC server handling `Execute`, `FindMissingBlobs`, `BatchUpdateBlobs`, `BatchReadBlobs`, `GetActionResult`, `UpdateActionResult`. | gRPC C++, Protobuf C++ | `src/reapi/server.hpp`, `src/reapi/execution_service.hpp`, `src/reapi/cas_service.hpp`, `src/reapi/action_cache.hpp` |
| **`cas/`** | Content-Addressed Storage engine; SHA-256 chunking and hashing; Merkle tree builder for directories; P2P chunk sync over LAN. Default storage: `~/.zeromesh/cas`. | OpenSSL (SHA-256), `std::filesystem` | `src/cas/store.hpp`, `src/cas/merkle.hpp`, `src/cas/p2p_syncer.hpp` |
| **`scheduler/`** | Cost model evaluating network latency vs. missing byte transfer cost; critical-path detection (pin linkers/fat tasks locally); remote dispatch & re-queueing. | Dynamic peer metrics, DAG heuristics | `src/scheduler/scheduler.hpp`, `src/scheduler/heuristics.hpp` |
| **`executor/`** | Unprivileged hermetic sandbox; Bubblewrap (`bwrap`) mounts, Landlock direct kernel syscalls; `sched_setscheduler(SCHED_IDLE)` and thermal/battery preemption. | `bwrap`, Linux Landlock (`linux/landlock.h`), `sched.h` | `src/executor/runner.hpp`, `src/executor/bwrap.hpp`, `src/executor/landlock.hpp`, `src/executor/host_guard.hpp` |
| **`ui/`** | Interactive terminal status monitor and live core utilization visualizer. | ANSI escape codes / FTXUI / Ncurses | `src/ui/tui.hpp` |
| **`main`** | Unified CLI entry point supporting `zm up`, `zm run`, `zm status`, `zm monitor`, and daemon lifecycle management. | CLI parser / `CLI11` | `src/main.cpp` |

---

## 3. Implementation Checklist & Development Tasks

### Phase 1: Foundations & REAPI Skeleton
- [x] Create project layout and C++20 / Meson build configuration (`meson.build`)
- [x] Specify official REAPI v2 Protobuf definitions (`proto/remote_execution.proto`)
- [ ] Implement local gRPC server listening on `127.0.0.1:8980`
- [ ] Implement baseline in-memory and local disk CAS blob store with SHA-256 verification
- [ ] Implement Action Cache (AC) for storing and looking up previous action digests
- [ ] Support `recc` compiler launcher by answering basic `FindMissingBlobs` and `BatchUpdateBlobs`

### Phase 2: Hermetic Execution & Sandboxing
- [ ] Implement Bubblewrap (`bwrap`) wrapper with `--ro-bind` for system toolchains and `--unshare-all`
- [ ] Implement Landlock sandbox ruleset (`SYS_landlock_create_ruleset`, `SYS_landlock_restrict_self`)
- [ ] Implement `SCHED_IDLE` (or `nice 19`) task scheduling to prevent donor machine stutter
- [ ] Add thermal and battery guard checks (suspend donor execution if battery < 20% or discharging)

### Phase 3: P2P Mesh & Blob Transfer
- [ ] Implement mDNS publisher and subscriber (`_zeromesh._tcp`) for zero-configuration subnet discovery
- [ ] Implement PeerTable with automatic stale node eviction and heartbeat gossiping
- [ ] Implement P2P missing blob syncer to stream input trees between nodes over LAN
- [ ] Add team-wide Action Cache lookups across peers to avoid duplicate compilation

### Phase 4: Locality-Aware Scheduler
- [ ] Implement cost heuristics comparing network round-trip + transfer time against local compilation cost
- [ ] Implement critical path detection to pin linker tasks (`ld`, `lld`, `ar`) locally
- [ ] Implement worker timeout, node health degradation, and automatic job re-dispatch

### Phase 5: Ergonomics & Observability
- [ ] Provide one-touch daemon management (`zm up`, `zm down`, `zm status`)
- [ ] Provide terminal TUI monitor (`zm monitor`) showing cluster nodes, live core loads, and transfer speeds
- [ ] Provide compiler integration script (`scripts/install_recc.sh`) for CMake, Make, and Meson builds

---

## 4. Architectural Decision Records (ADR)

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
