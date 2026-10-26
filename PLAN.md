# ZeroMesh Architecture & Living Execution Plan

> **Note for AI Agents & Developers:** Keep this document continuously updated. When you complete an objective or discover an architectural requirement, update the statuses (`[ ]` -> `[x]`), refine module contracts, and record design decisions in the ADR log at the bottom.

---

## 1. High-Level Architectural Design

ZeroMesh operates as a single binary daemon (`zmd` / `zm`) installed on each machine. It functions simultaneously as a **Client Proxy**, a **REAPI Service Provider**, a **P2P CAS Node**, and an **Unprivileged Hermetic Executor**.

```text
+---------------------------------------------------------------------------------+
|                                  Host Machine                                   |
|                                                                                 |
|  [ Workloads ] (CMake/Bazel / zm run / Native C App with libzmdsm / zm fs mount)|
|         |                                 |                       |             |
|         v (REAPI v2: :8980)               v (DSM Page Traps)      v (POSIX VFS) |
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
|  |  |   Secure FUSE-FS (zm-fs)     |     |    Sandboxed Task Executor     |  |  |
|  |  | - fuse3 user-space mount     |     | - Bubblewrap (bwrap) isolation |  |  |
|  |  | - Lazy block faulting        |     | - Landlock filesystem lockdown |  |  |
|  |  | - ChaCha20-Poly1305 encrypt  |     | - SCHED_IDLE task priority     |  |  |
|  |  +------------------------------+     | - Heterogeneous wgpu / CUDA    |  |  |
|  |                 |                     +--------------------------------+  |  |
|  |                 v                                     |                   |  |
|  |  +------------------------------+                     v                   |  |
|  |  |   Network DSM (libzmdsm)     |         Isolated Linux Subprocess       |  |  |
|  |  | - Shared memory over LAN     |        (No host/network access)         |  |  |
|  |  | - Twin-page diffs & barriers |                                         |  |  |
|  |  +------------------------------+                                         |  |
|  |                 |                                                         |  |
|  |                 v                                                         |  |
|  |  +------------------------------+     +--------------------------------+  |  |
|  |  |  BitTorrent Kademlia DHT     |     |   2% Byzantine Spot Checker    |  |  |
|  |  | - 256-bit BLAKE3 key space   | <-> | - Shadow re-execution quorum   |  |  |
|  |  | - Chunk provider registry    |     | - Hardware TPM/Enclave signing |  |  |
|  |  | - Action Cache memoization   |     | - Automated peer quarantine    |  |  |
|  |  +------------------------------+     +--------------------------------+  |  |
|  |                 |                                                         |  |
|  |                 v                                                         |  |
|  |  +------------------------------+                                         |  |
|  |  |     mDNS Peer Mesh Engine    |                                         |  |
|  |  | - Node discovery & heartbeat |                                         |  |
|  |  | - Hardware telemetry gossip  |                                         |  |
|  |  +------------------------------+                                         |  |
|  +---------------------------------------------------------------------------+  |
+---------------------------------------------------------------------------------+
                                     |
                (Encrypted LAN P2P Traffic: mTLS / Noise)
                                     v
+---------------------------------------------------------------------------------+
|                                 Donor Peer 1..N                                 |
| - Identical zmd daemon running in background                                    |
| - Contributes idle CPU/GPU cycles automatically via SCHED_IDLE                  |
+---------------------------------------------------------------------------------+
```

---

## 2. Component Design & Responsibilities

| Subsystem | Primary Responsibilities | Core Dependencies | Key Files |
| :--- | :--- | :--- | :--- |
| **`discovery/`** | Broadcast node capability over mDNS (`_zeromesh._tcp`); maintain dynamic peer routing table, heartbeat pings, and hardware telemetry (cores, load, battery). | Avahi / mDNS socket, POSIX sockets | `src/discovery/mdns.hpp`, `src/discovery/peer_table.hpp` |
| **`dht/`** | BitTorrent-style Kademlia Distributed Hash Table; 256-bit BLAKE3 keys, $k$-buckets, chunk provider announcements (`ANNOUNCE_CHUNK`/`GET_PROVIDERS`), and Action Cache memoization. | Encrypted UDP, BLAKE3 | `src/dht/kademlia.hpp` |
| **`vfs/`** | Distributed Secure FUSE Filesystem (`zm-fs`) via Linux `fuse3`; lazy block-level chunk faulting, local LRU NVMe cache, and ChaCha20-Poly1305 encryption. | `fuse3`, OpenSSL/libhydrogen | `src/vfs/fuse_fs.hpp` |
| **`dsm/`** | Network Distributed Shared Memory engine; virtual memory page fault traps (`userfaultfd`/`mprotect`), twin-page bitwise diffing, and release-consistency synchronization. | POSIX `mmap`, `userfaultfd` | `src/dsm/dsm_engine.hpp`, `include/zmdsm.h` |
| **`reapi/`** | Standard REAPI v2 gRPC server handling `Execute`, `FindMissingBlobs`, `BatchUpdateBlobs`, `BatchReadBlobs`, `GetActionResult`, `UpdateActionResult`. | gRPC C++, Protobuf C++ | `src/reapi/server.hpp`, `src/reapi/execution_service.hpp`, `src/reapi/cas_service.hpp`, `src/reapi/action_cache.hpp` |
| **`cas/`** | Content-Addressed Storage engine; SHA-256 / BLAKE3 chunking; Merkle tree builder for directories; P2P chunk sync over LAN. Default storage: `~/.zeromesh/cas`. | OpenSSL, `std::filesystem` | `src/cas/store.hpp`, `src/cas/merkle.hpp`, `src/cas/p2p_syncer.hpp` |
| **`scheduler/`** | Cost model evaluating network latency vs. missing byte transfer cost; critical-path detection (pin linkers/fat tasks locally); remote dispatch & re-queueing. | Dynamic peer metrics, DAG heuristics | `src/scheduler/scheduler.hpp`, `src/scheduler/heuristics.hpp` |
| **`executor/`** | Unprivileged hermetic sandbox; Bubblewrap (`bwrap`) mounts, Landlock direct kernel syscalls; `sched_setscheduler(SCHED_IDLE)` and thermal/battery preemption. | `bwrap`, Linux Landlock (`linux/landlock.h`), `sched.h` | `src/executor/runner.hpp`, `src/executor/bwrap.hpp`, `src/executor/landlock.hpp`, `src/executor/host_guard.hpp` |
| **`gpu/`** | Heterogeneous accelerator mesh managing compute dispatch via `wgpu` (Vulkan/Metal/DX12) and CUDA, Apple Silicon UMA pooling, and link bandwidth gate. | `wgpu`, Vulkan/Metal | `src/gpu/gpu_engine.hpp` |
| **`verify/`** | 2% randomized Byzantine spot-check shadow execution, TPM 2.0 / Apple Secure Enclave signing, and automatic rogue peer quarantine. | Hardware TPM/Enclave, Ed25519 | `src/verify/byzantine_spot_checker.hpp` |
| **`ui/`** | Interactive terminal status monitor and live core utilization visualizer. | ANSI escape codes / FTXUI / Ncurses | `src/ui/tui.hpp` |
| **`main`** | Unified CLI entry point supporting `zm up`, `zm run`, `zm fs`, `zm status`, `zm monitor`, and daemon lifecycle management. | CLI parser / `CLI11` | `src/main.cpp` |

---

## 3. Implementation Checklist & Development Tasks

### Phase 1: Foundations & REAPI Skeleton
- [x] Create project layout and C++20 / Meson build configuration (`meson.build`)
- [x] Specify official REAPI v2 Protobuf definitions (`proto/remote_execution.proto`)
- [x] Implement local gRPC server listening on `127.0.0.1:8980`
- [x] Implement baseline in-memory and local disk CAS blob store with SHA-256 verification
- [x] Implement Action Cache (AC) for storing and looking up previous action digests
- [x] Support `recc` compiler launcher by answering basic `FindMissingBlobs` and `BatchUpdateBlobs`

### Phase 2: Hermetic Execution & Sandboxing
- [x] Implement Bubblewrap (`bwrap`) wrapper with `--ro-bind` for system toolchains and `--unshare-all`
- [x] Implement Landlock sandbox ruleset (`SYS_landlock_create_ruleset`, `SYS_landlock_restrict_self`)
- [x] Implement `SCHED_IDLE` (or `nice 19`) task scheduling to prevent donor machine stutter
- [x] Add thermal and battery guard checks (suspend donor execution if battery < 20% or discharging)

### Phase 3: P2P Mesh & Blob Transfer
- [ ] Implement mDNS publisher and subscriber (`_zeromesh._tcp`) for zero-configuration subnet discovery
- [x] Implement PeerTable with automatic stale node eviction and heartbeat gossiping
- [ ] Implement P2P missing blob syncer to stream input trees between nodes over LAN
- [x] Add team-wide Action Cache lookups across peers to avoid duplicate compilation

### Phase 4: Locality-Aware Scheduler
- [x] Implement cost heuristics comparing network round-trip + transfer time against local compilation cost
- [x] Implement critical path detection to pin linker tasks (`ld`, `lld`, `ar`) locally
- [ ] Implement worker timeout, node health degradation, and automatic job re-dispatch

### Phase 5: Ergonomics & Observability
- [x] Provide one-touch daemon management (`zm up`, `zm down`, `zm status`)
- [ ] Provide terminal TUI monitor (`zm monitor`) showing cluster nodes, live core loads, and transfer speeds
- [ ] Provide compiler integration script (`scripts/install_recc.sh`) for CMake, Make, and Meson builds

### Phase 6: BitTorrent-Style Kademlia DHT Engine
- [ ] 256-bit BLAKE3 key space and $k$-bucket routing table ($k=8$).
- [ ] Implement `ANNOUNCE_CHUNK` and `GET_PROVIDERS` RPCs for trackerless CAS chunk location.
- [ ] Distributed Action Cache storage and lookup over encrypted UDP datagrams.
- [ ] 1-hop LAN shortcut routing table initialization via mDNS discovery.

### Phase 7: Distributed Secure FUSE Filesystem (`zm-fs`)
- [ ] Implement user-space filesystem driver via `fuse3` (`src/vfs/fuse_fs.hpp`).
- [ ] Lazy block faulting: pull only requested 512 KB chunks on `read()` syscalls via DHT providers.
- [ ] Authenticated at-rest and in-transit chunk encryption via ChaCha20-Poly1305.
- [ ] Local NVMe/SSD LRU chunk cache for offline read resilience.

### Phase 8: Network Distributed Shared Memory (DSM) C Library (`libzmdsm`)
- [ ] Implement public C library interface (`include/zmdsm.h`).
- [ ] Virtual memory page trap engine using Linux `userfaultfd` and `mprotect`.
- [ ] Twin-page dirty detection and bitwise XOR diff propagation over TCP/QUIC.
- [ ] Distributed barrier synchronization (`zmdsm_barrier`) and distributed mutex (`zmdsm_lock`).

### Phase 9: Heterogeneous Accelerator & GPU Engine (`wgpu` / CUDA)
- [ ] Implement `wgpu` compute shader dispatch (Vulkan on Linux, Metal on macOS, DX12 on Windows).
- [ ] Direct CUDA / MLIR compute channel for Linux workstations.
- [ ] Apple Silicon Unified Memory Architecture (UMA) pooling for large model/matrix transformations.
- [ ] Dynamic bandwidth-throttling gate evaluating FLOP/byte density before offloading.

### Phase 10: Zero-Trust Security & Byzantine Spot-Check Quorums
- [x] Hardware-backed Ed25519 signing using Linux TPM 2.0 and Apple Secure Enclave.
- [x] Randomized 2% shadow re-execution of completed background tasks on independent peers.
- [x] Automatic cryptographic digest cross-validation and rogue peer quarantine.

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
