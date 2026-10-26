# ZeroMesh ⚡

A zero-config, peer-to-peer remote execution mesh that aggregates idle developer laptops on a local Wi-Fi or LAN into an elastic compile and compute cluster.

Built on the **Remote Execution API (REAPI v2)**. Unmodified **CMake**, **Make**, **Ninja**, **Bazel**, and **Buck2** builds offload compilation tasks to nearby machines in seconds without code changes or centralized servers.

---

## 1. Directory Hierarchy & File Placement Map

```text
zeromesh/
├── meson.build                           # Root Meson build configuration (C++20)
├── subprojects/                          # Dependency fallbacks and wrap specifications
├── include/
│   └── zmdsm.h                           # Public C library header for Distributed Shared Memory (DSM)
├── proto/
│   └── remote_execution.proto            # Official REAPI v2 protocol definitions
├── src/
│   ├── main.cpp                          # Entry point: CLI parsing, daemon lifecycle
│   ├── discovery/
│   │   ├── mdns.hpp / .cpp               # mDNS broadcast & listener (_zeromesh._tcp)
│   │   └── peer_table.hpp / .cpp         # Node telemetry, ping, and peer registry
│   ├── dht/
│   │   └── kademlia.hpp / .cpp           # BitTorrent Kademlia DHT (chunk providers, action cache)
│   ├── vfs/
│   │   └── fuse_fs.hpp / .cpp            # Distributed Secure FUSE Filesystem (fuse3 / lazy CAS)
│   ├── dsm/
│   │   └── dsm_engine.hpp / .cpp         # Network Distributed Shared Memory engine (LRC / page traps)
│   ├── reapi/
│   │   ├── server.hpp / .cpp             # gRPC server setup (127.0.0.1:8980)
│   │   ├── execution_service.hpp / .cpp  # REAPI Execution service implementation
│   │   ├── cas_service.hpp / .cpp        # ContentAddressableStorage service
│   │   └── action_cache.hpp / .cpp       # ActionCache get/update
│   ├── cas/
│   │   ├── store.hpp / .cpp              # Local content-addressed store (SHA-256 / BLAKE3)
│   │   ├── merkle.hpp / .cpp             # Directory tree hasher / digest calculator
│   │   └── p2p_syncer.hpp / .cpp         # Local LAN blob streamer
│   ├── scheduler/
│   │   ├── scheduler.hpp / .cpp          # Locality & DAG-aware task assignment
│   │   └── heuristics.hpp / .cpp         # Bandwidth cost & critical-path evaluation
│   ├── executor/
│   │   ├── runner.hpp / .cpp             # Task execution coordinator
│   │   ├── bwrap.hpp / .cpp              # bubblewrap CLI builder & fork/exec supervisor
│   │   ├── landlock.hpp / .cpp           # Direct Linux Landlock syscall setup
│   │   └── host_guard.hpp / .cpp         # sched_setscheduler(SCHED_IDLE) & power monitor
│   ├── gpu/
│   │   └── gpu_engine.hpp / .cpp         # Heterogeneous accelerator mesh (wgpu / CUDA)
│   ├── verify/
│   │   └── byzantine_spot_checker.hpp    # 2% Byzantine spot-checking & TPM/Enclave signing
│   └── ui/
│       └── tui.hpp / .cpp                # Terminal monitor (ftxui / ncurses)
├── PLAN.md                               # Living architectural state and roadmap
└── README.md                             # Project overview and code organization guide
```

---

## 2. Module Responsibilities & Entry-Point Function Map

| Module & Path | Primary Duty | Key Classes / Structs | Primary Entry Functions |
| :--- | :--- | :--- | :--- |
| **`src/main.cpp`** | CLI dispatcher and daemon coordinator | `DaemonApp` | `main(int argc, char** argv)` |
| **`src/discovery/mdns.*`** | Advertises local node on `_zeromesh._tcp` and listens for LAN peers | `MdnsEngine`, `MdnsConfig` | `StartBroadcaster()`, `StartDiscovery()`, `Stop()` |
| **`src/discovery/peer_table.*`**| Tracks active peers, health metrics, and hardware telemetry | `Peer`, `PeerTable` | `Upsert(peer)`, `ListActive()`, `SelectBestWorker()` |
| **`src/reapi/server.*`** | Host gRPC server listening on `127.0.0.1:8980` | `ReapiServer` | `Start(port)`, `Stop()`, `Wait()` |
| **`src/reapi/execution_service.*`** | Implements REAPI `Execution::Execute` | `ExecutionServiceImpl` | `Execute(context, request, writer)` |
| **`src/reapi/cas_service.*`** | Implements REAPI `ContentAddressableStorage` | `CasServiceImpl` | `FindMissingBlobs()`, `BatchUpdateBlobs()`, `BatchReadBlobs()` |
| **`src/reapi/action_cache.*`** | Implements REAPI `ActionCache` | `ActionCacheServiceImpl` | `GetActionResult()`, `UpdateActionResult()` |
| **`src/cas/store.*`** | Local blob store in `~/.zeromesh/cas` with SHA-256 verification | `CasStore`, `Digest` | `PutBlob(data)`, `GetBlob(hash)`, `HasBlob(hash)` |
| **`src/cas/merkle.*`** | Reconstructs and hashes input/output directory trees | `MerkleTreeBuilder` | `BuildDirectoryTree(path)`, `MaterializeTree(digest, target)` |
| **`src/cas/p2p_syncer.*`** | Streams missing CAS chunks between LAN peers | `P2PSyncer` | `FetchBlob(digest, peers)`, `BroadcastBlob(digest)` |
| **`src/scheduler/scheduler.*`**| Dispatches compilation tasks locally or to peers | `Scheduler` | `ScheduleAction(action)`, `DispatchRemote(peer, action)` |
| **`src/scheduler/heuristics.*`**| Cost model evaluating missing transfer bytes vs compute | `SchedulerHeuristics`| `EstimateCost()`, `IsCriticalPath(command)` |
| **`src/executor/runner.*`** | Sandboxed execution lifecycle coordinator | `SandboxRunner` | `Execute(command, inputRoot, workingDir)` |
| **`src/executor/bwrap.*`** | Configures unprivileged Bubblewrap isolation boundaries | `BwrapBuilder` | `BuildArgs()`, `RunSandboxed()` |
| **`src/executor/landlock.*`** | Configures in-kernel Landlock filesystem access control | `LandlockSandbox` | `ApplyRuleset(roPaths, rwPaths)` |
| **`src/executor/host_guard.*`** | Enforces `SCHED_IDLE` and thermal/battery preemption | `HostGuard` | `EnforceIdlePriority(pid)`, `IsThrottled()`, `CheckBattery()` |
| **`src/dht/kademlia.*`** | BitTorrent Kademlia DHT for trackerless chunk provider discovery & action memoization | `KademliaDht`, `DhtNode` | `Start()`, `AnnounceChunk()`, `GetChunkProviders()`, `FindValue()` |
| **`src/vfs/fuse_fs.*`** | Distributed Secure FUSE-FS (`zm-fs`) via `fuse3` with lazy chunk faulting | `FuseFileSystem`, `FileInode` | `Mount()`, `Unmount()`, `PublishFile()` |
| **`src/dsm/dsm_engine.*`** | Network Distributed Shared Memory engine; page traps & release consistency | `DsmEngine`, `DsmRegion` | `Initialize()`, `AllocateRegion()`, `Barrier()`, `Lock()` |
| **`include/zmdsm.h`** | Public C API for Distributed Shared Memory applications | C API Functions | `zmdsm_init()`, `zmdsm_alloc()`, `zmdsm_barrier()`, `zmdsm_lock()` |
| **`src/gpu/gpu_engine.*`** | Heterogeneous accelerator mesh (`wgpu` / CUDA) with bandwidth gate | `GpuEngine`, `GpuProfile` | `ProbeHardware()`, `ShouldOffload()`, `ExecuteComputeKernel()` |
| **`src/verify/byzantine_spot_checker.*`** | 2% randomized shadow verification and TPM/Enclave signing | `ByzantineSpotChecker` | `ShouldSpotCheck()`, `VerifyResult()`, `QuarantinePeer()` |
| **`src/ui/tui.*`** | Live terminal core and network activity monitor | `TerminalUI` | `RenderDashboard(peerTable, stats)` |

---

## 3. How It Works

ZeroMesh is designed to be completely zero-config and invisible to the user:

1. **Subnet Discovery:** When `zmd` (the daemon) launches, it immediately sends multicast DNS packets advertising its presence, core count, and memory.
2. **REAPI Interception:** Build tools (such as `recc`, Bazel, or Buck2) send standard REAPI v2 requests to `localhost:8980`.
3. **Locality-Aware Scheduling:**
   - Small compilation tasks (`g++ -c foo.cpp`) are scheduled to idle peers with the lowest network cost.
   - Heavy linking stages or actions with massive inputs are automatically pinned to the local machine to avoid unnecessary transfer latency.
4. **Hermetic & Safe Sandboxing:**
   - Tasks execute inside unprivileged **Bubblewrap** (`bwrap`) containers.
   - Linux **Landlock** locks down filesystem write permissions to the build sandbox only.
   - Tasks run under **`SCHED_IDLE`** so the host user experiences zero stutter or CPU lag.

---

## 4. Quickstart

### Prerequisites
- Modern Linux system (kernel ≥ 5.13 for Landlock support)
- `bwrap` (Bubblewrap)
- C++20 compiler (`gcc` ≥ 11 or `clang` ≥ 13)
- `meson` and `ninja`

### Building from Source

```bash
# Setup build directory
meson setup build

# Compile all targets
ninja -C build

# Install or run directly
./build/zm --help
```

### Starting the Daemon

```bash
# Start ZeroMesh daemon in background
./build/zm up

# Inspect active cluster peers and hardware utilization
./build/zm status

# Launch the live interactive terminal dashboard
./build/zm monitor
```

---

## 5. Build Integration

### CMake with `recc`

Configure CMake to use `recc` as the compiler launcher:

```bash
export RECC_SERVER=127.0.0.1:8980
export CMAKE_CXX_COMPILER_LAUNCHER=recc
export CMAKE_C_COMPILER_LAUNCHER=recc

cmake -B build -S .
cmake --build build -j$(nproc)
```

### Bazel

Add the local ZeroMesh endpoint to your Bazel build flags:

```bash
bazel build //... \
  --remote_executor=grpc://127.0.0.1:8980 \
  --remote_default_exec_properties=OSFamily=Linux
```

### Direct Run Wrapper

You can also run commands through the ZeroMesh launcher:

```bash
zm run -- g++ -c main.cpp -o main.o
```

---

## 6. Distributed Secure FUSE Filesystem (`zm-fs`)

Mount a virtual, peer-synchronized folder across all paired laptops with zero central storage servers:

```bash
# Mount the distributed mesh filesystem on local mountpoint
zm fs mount ~/zeromesh-shared

# Files are chunked (512 KB), encrypted with ChaCha20-Poly1305, and cached on local NVMe
cp my_dataset.tar.zst ~/zeromesh-shared/

# Other nodes in the mesh can read or execute it immediately via lazy on-demand chunk streaming
ls -lh ~/zeromesh-shared/
```

---

## 7. Network Distributed Shared Memory (DSM) C Library (`libzmdsm`)

For high-performance simulations, distributed matrix models, or multi-node parallel computing without MPI:

```c
#include <stdio.h>
#include "zmdsm.h"

int main() {
    // 1. Join cluster session
    zmdsm_ctx_t *ctx = zmdsm_init("mesh_compute_cluster", -1);

    // 2. Allocate named shared memory segment (synchronized across all nodes)
    size_t size = 1024 * 1024 * sizeof(double);
    double *shared_array = (double *)zmdsm_alloc(ctx, "global_array", size, ZMDSM_READ_WRITE);

    // 3. Coordinate work across nodes with barriers and locks
    zmdsm_barrier(ctx, "start_sync");

    int rank = zmdsm_get_node_id(ctx);
    shared_array[rank * 100] = 42.0 * rank;

    zmdsm_barrier(ctx, "end_sync");

    // 4. Cleanup
    zmdsm_free(ctx, shared_array);
    zmdsm_destroy(ctx);
    return 0;
}
```

Compile directly with `-lzmdsm`:
```bash
gcc -O3 worker.c -o worker -lzmdsm
./worker
```

