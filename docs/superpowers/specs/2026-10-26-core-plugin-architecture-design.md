# ZeroMesh Core + Plugin Architecture — Design

- **Date:** 2026-10-26
- **Status:** Draft for team review
- **Supersedes:** the "REAPI compile mesh" architecture in `PLAN.md` §1–§2 and its Phase 1–10 checklist

---

## 1. Intent: why this exists

ZeroMesh lets a group of people on the same LAN (lab, office, team, campus) pool their idle machines and offload work to them **with zero setup**: install one daemon, join a team, done. No cluster, no head node, no shared storage, no config files.

Every distributed tool today rebuilds the same plumbing. Flamenco needs a Manager plus shared storage that every worker can reach. distcc/icecream need matching compilers on every host. CFD sweeps use hand-written ssh loops. That plumbing covers finding workers, queueing, moving data, getting the same software onto every worker, and not annoying machine owners, and each tool redoes it, usually badly.

**ZeroMesh is that plumbing, built once, as a small stable core.** Domain tools ("apps", e.g. a Flamenco-style renderer, a CFD sweep runner, a simulation batch runner) are built on top through a public SDK, and only write the domain part: **how to split work, what to run per piece, and how to merge results.** Gazebo is the structural model: separate libraries and a small core, with features attached through narrow plugin interfaces.

### What changed from the old plan, and why

The old architecture centred on REAPI v2 for compiling C++. A code audit on 2026-10-26 found that the parts that work are leaf modules (CAS, sandbox executor, discovery, FUSE). The central piece, actually running a task on another machine, does not exist (§3). Since the middle has to be built anyway, we build it general from the start. REAPI becomes one optional front-end plugin instead of the centre.

## 2. Goals, non-goals, anti-goals

### Goals (v1)
1. A task submitted on machine A runs on machine B in a sandbox, and its outputs come back to A. This is the core promise.
2. Zero setup: install → `zm team join <secret>` → done. No config files are required.
3. A general unit of work (**Task**) and data (**Object**) that any app can use, not something tied to compilers.
4. A public client API (local socket) plus a **Python SDK**, so third parties can build apps without touching core code.
5. In-process **core plugins** (Gazebo-style) for things that must live inside the daemon: environment providers and front-ends.
6. Donor machines stay responsive: low priority, real battery/AC awareness, and draining with re-dispatch.
7. The team is the security boundary. Only paired machines can submit or run work, and all traffic between nodes is authenticated and encrypted.
8. One reference app, **`zm-render`** (Blender frame rendering), proves the SDK end to end.

### Non-goals (v1)
- Splitting a single process across machines. Research (GiantVM, Gazebo's own lockstep distributed sim) shows this collapses on commodity networks.
- Services and Channels (long-lived stateful processes and message streams). The proto reserves names for them, but they're v2.
- macOS/Windows donors. v1 is Linux-only, matching the sandbox stack.
- Cross-architecture execution (x86_64 ↔ arm64). Placement must match architecture.
- Internet/WAN meshes, relays across subnets (v2), and NAT traversal.
- GPU kernel offload, DSM, the DHT, Byzantine spot-checking, TPM signing. All are frozen (§10).

### Anti-goals: things that make this a failure even if it technically works
- **Fake success.** A stub that reports "done", or a checkbox ticked for code that doesn't run. Every milestone has an acceptance test that runs across two real processes, with real machines where stated.
- **Annoying donors.** If anyone notices their laptop lagging, people uninstall.
- **Strangers running code on your laptop.** For example, anyone on café Wi-Fi joining your mesh.
- **Domain logic leaking into core.** Nothing named "gcc", "blender" or "linker" lives in `core/`.
- **Apps reaching into internals.** Apps only use the public API, or the core can never change.

## 3. Current state (audit, 2026-10-26)

| Area | Status |
|---|---|
| CAS store (SHA-256, disk) + Merkle tree builder/materializer | **Real.** Keep. |
| Sandbox executor: fork/exec, `SCHED_IDLE`, Landlock, bwrap builder | **Real.** Keep and adapt. |
| Discovery: custom UDP multicast (`239.255.42.99:8988`, tab-separated `ZM_ANNOUNCE`/`ZM_DRAIN`) + `PeerTable` | **Real, but unauthenticated.** Keep and extend. |
| FUSE filesystem + chunk fetcher (~1,600 lines) | **Real**, but coupled to `dht::KademliaDht`. Adapt for mirror mode. |
| TUI | **Real.** Adapt to read from the client API. |
| REAPI gRPC server | **Stub.** `server.cpp` prints "listening" and opens no socket. gRPC isn't even installed, and `meson.build` marks it optional. |
| Remote dispatch | **Missing.** `execution_service.cpp:59-61` runs locally on both branches. There is no node-to-node protocol, and nothing serves `GET_CAS`. |
| `zm up` | **Broken.** Exits immediately (`main.cpp:105-107`). |
| `zm run` | **Broken.** Hard-coded `action_id` (`main.cpp:136`) means every run after the first is a fake cache hit. |
| Telemetry | **Fake.** `host_guard.hpp:32-40` hard-codes load, battery and AC. |
| DSM, Byzantine/TPM, chunk encryption, BLAKE3 | **Stubs or local-only**, despite `[x]` in PLAN.md. |
| DHT | Real code, but every node uses the ID `0x42…` (`main.cpp:158,186`). Frozen. |
| Tests | 1 Meson suite passes, but it tests the stubs as written. |

## 4. Architecture overview

```
   ┌──────────────────────── apps (out-of-process, any language) ────────────────────────┐
   │  zm CLI (run, run --each, status, monitor)   zm-render   (future: zm-sweep, zm-sim)  │
   └───────────────────────────────┬──────────────────────────────────────────────────────┘
                                   │ zeromesh.v1.ClientApi  (gRPC over a unix socket, owner-only)
   ┌───────────────────────────────▼─────────── zmd (daemon) ────────────────────────────┐
   │  node/      ClientApi service · NodeApi service · job manager                        │
   │  sched/     task graph · placement · retries · re-dispatch on drain/loss             │
   │  exec/      sandbox runner (bwrap + Landlock + SCHED_IDLE)  ◄── IEnvProvider plugins │
   │  store/     CAS + Merkle (Objects)                                                   │
   │  net/       discovery (team-scoped) · peer table · telemetry · secure transport      │
   │  plugin/    loader + interfaces   ◄── IFrontend plugins (e.g. REAPI gateway)         │
   └───────────────────────────────┬──────────────────────────────────────────────────────┘
                                   │ zeromesh.v1.NodeApi  (gRPC over mTLS, team members only)
                         other zmd nodes on the LAN
```

**Two kinds of extension, on purpose:**

| | Core plugins | Apps |
|---|---|---|
| Runs | inside `zmd` (shared library or statically linked) | separate process |
| Language | C++ | any (Python first) |
| Talks via | C++ plugin interfaces (§6.2) | `ClientApi` only (§6.1) |
| Trust | trusted, installed by the machine owner | untrusted relative to the core; can't reach donors except through tasks |
| Examples | `env-host`, `env-mirror`, `env-oci`, `frontend-reapi` | `zm` CLI, `zm-render` |

A crashing app can never take down the daemon, and app authors never need to compile against core internals.

## 5. Core concepts

- **Node:** one machine running `zmd`. It has a persistent random 128-bit `node_id` generated on first start and stored in the state dir. (This replaces the shared `0x42` ID.)
- **Team:** the set of nodes that trust each other. A node belongs to zero or one team in v1. A team is defined by a **team secret** (§9). Discovery and every RPC are scoped to the team. This is the Gazebo "partition" idea plus authentication.
- **Object:** immutable bytes addressed by digest (`sha256:<hex>`, size). It's stored in the CAS, can be fetched from any team node that has it, and is either a file blob or a directory tree (Merkle manifest).
- **Task:** one sandboxed execution:
  - `argv` and `env` vars
  - `input_root` (an Object directory tree)
  - `working_dir`
  - declared `outputs` (paths)
  - an `env` (environment provider name + params)
  - `resources` (cores, RAM, architecture)
  - a `policy` (preemptible, max retries, timeout, may-run-remote, may-run-local)

  **Task identity:** `task_key = sha256(canonical serialization of every field that affects the result)`. It's used for caching and deduplication. Fields that don't affect the result (priority, display name) are excluded.
- **Job:** a named group of tasks with dependencies (a DAG: a task can depend on other tasks' outputs). It's submitted, watched and cancelled as a unit. v1 graphs are **static at submit time**. Dynamic graphs (a task that emits tasks) are v2, but the proto must not preclude them.
- **Result:** `exit_code`, stdout/stderr (as Objects, with a short inline prefix), output Objects, the node that executed it, timings, and whether it was a cache hit. A cache hit must return the *real* stored result, never an empty placeholder.

## 6. Contracts

These are the first things to build (M1). After that, people can work in parallel.

### 6.1 `proto/zeromesh/v1/*.proto`

The builder chooses field names and numbering. The shape below is decided.

**`ClientApi`**: served by `zmd` on `unix://$XDG_RUNTIME_DIR/zeromesh/zmd.sock`, mode `0600`. Only the machine's owner can submit.
- `PutObjects(stream chunk) → digests` / `GetObjects(digests) → stream chunk` / `FindMissing(digests) → digests`
- `UploadTree(local path) → root digest`: a convenience call. The daemon hashes the path itself, so clients don't have to reimplement Merkle hashing. The client and daemon are on the same machine, so the daemon reads the path directly.
- `SubmitJob(JobSpec) → job_id`
- `WatchJob(job_id) → stream JobEvent`: task queued/placed/running/progress/finished/retried, plus log lines
- `CancelJob(job_id)`
- `GetJob(job_id) → JobStatus`
- `FetchOutputs(task_id, dest local path)`: materialize a task's outputs on the client's machine
- `ListPeers() → [PeerInfo]` (for `zm status`, the TUI and apps showing capacity)
- `TeamCreate() → secret`, `TeamJoin(secret)`, `TeamLeave()`, `TeamInfo()`

**`NodeApi`**: served by `zmd` on TCP port **8981** on the LAN interface. mTLS, team members only.
- `OfferTask(TaskSpec, missing-input query) → accept | reject(reason) + list of input digests the donor lacks`. The donor decides based on its policy, its free resources, whether it has the env, and its architecture.
- `PushBlobs(stream) / PullBlobs(digests) → stream`: blob transfer in both directions. This replaces the `GET_CAS` text protocol.
- `RunTask(task_id) → stream TaskEvent`: starts execution and streams progress, logs and the final result. Outputs are stored in the donor's CAS. The requester then pulls the digests it needs.
- `CancelTask(task_id)`
- `Drain(reason)`: a push notification to the requesters with in-flight tasks. It complements the multicast drain signal.
- `Ping() → telemetry`: health and latency measurement.

**Discovery announcements** keep using UDP multicast on `239.255.42.99:8988`. They're extended with: `team_id` (= the first 16 bytes of `sha256("zm-team-id" || secret)`, which is public), architecture, `NodeApi` port, real telemetry, and an HMAC-SHA256 tag keyed by the team secret. Nodes ignore announcements from other teams and any announcement with a bad tag. Switching from the tab-separated text format to protobuf is the builder's choice.

### 6.2 Core plugin interfaces (`core/plugin/`)

```cpp
namespace zeromesh::plugin {

// Prepares where and how a task's software comes from on the executing node.
class IEnvProvider {
public:
    virtual ~IEnvProvider() = default;
    virtual std::string_view Name() const = 0;                     // "host", "mirror", "oci"
    // Can this node run tasks with these env params? Called during OfferTask.
    virtual Availability Check(const EnvParams&) = 0;               // ok | missing(reason)
    // Returns the mounts/binds the sandbox must apply, plus any env vars.
    virtual Result<SandboxEnv> Prepare(const TaskContext&) = 0;
    virtual void Release(const TaskContext&) = 0;                   // unmount / cleanup
};

// Exposes another protocol or UI on top of the ClientApi (e.g. REAPI for recc/Bazel).
class IFrontend {
public:
    virtual ~IFrontend() = default;
    virtual std::string_view Name() const = 0;
    virtual bool Start(ClientApiHandle&, const PluginConfig&) = 0;
    virtual void Stop() = 0;
};

} // namespace zeromesh::plugin
```

**Loading** follows the Gazebo pattern:
- Each plugin shared library exports one C symbol, `zm_plugin_entry()`. It returns a struct holding `abi_version` and factory functions.
- The core refuses plugins with a different `abi_version` major.
- Search order: `$ZM_PLUGIN_PATH`, then `~/.local/lib/zeromesh/plugins`, then the install dir.
- Built-in plugins (`env-host` and the others in this repo) may also be linked statically and registered at startup, so a fresh install works with no plugin files at all.

`TaskContext`, `EnvParams`, `SandboxEnv` and `Availability` are defined in `core/plugin/` and are part of the plugin ABI. Keep them minimal.

## 7. Task lifecycle

1. **Submit:** the client uploads inputs (`UploadTree`), then calls `SubmitJob`. The daemon computes each `task_key`.
2. **Cache:** if the local ActionCache has `task_key` with exit code 0 and every output Object is present, finish right away as a cache hit. Team-wide cache lookups are v2.
3. **Place** (§8). Try `OfferTask` on candidates in ranked order until one accepts. If none accept and the policy allows local execution, run locally.
4. **Transfer:** push only the input digests the donor reported missing.
5. **Run:** the donor's env provider runs `Prepare`, then the sandbox runner executes it under `SCHED_IDLE`, Landlock and bwrap, with **no network by default**. The donor captures the outputs into its CAS and streams events.
6. **Collect:** the requester pulls the output digests and records the result in its ActionCache. Dependent tasks become runnable.
7. **Failure handling:**
   - Non-zero exit is a final result and isn't retried. That's the app's problem.
   - Donor drain, donor loss (missed heartbeats for 10 s or a broken stream), or a timeout re-queues the task to another node. Retries are capped by `policy.max_retries` (default 3).
   - Re-running is safe because tasks are hermetic.
   - Partial outputs from a failed attempt are discarded.

### 7.1 Sandbox layout (the same on every node)

- The task's `input_root` is materialized at **`/work`** inside the sandbox. The working directory is `/work/<working_dir>`. `argv` should use relative paths. Absolute paths into the requester's home don't exist on a donor, except in mirror mode.
- Each dependency's outputs are overlaid into the dependent task's `/work` at the path the job spec declares: `inputs_from: [{task: "render-001", path: "frames/"}]`.
- **`env-host`** (my call) bind-mounts the donor's `/usr`, `/bin`, `/lib*`, `/etc/alternatives`, `/etc/ld.so.cache` and `/etc/ssl` read-only, so the donor's installed software is visible. `Check()` passes only if `argv[0]` resolves on the donor's `PATH`, or is a path inside `input_root`. This deliberately relaxes the old ADR-002, which forbade host `/usr`, because hermetic toolchains are exactly what `env-oci` is for (v2).
- Outputs: declared paths under `/work` are captured into the CAS after exit. Undeclared files are discarded along with the scratch dir.

### 7.2 CLI semantics (`zm run`)

- `zm run [opts] -- <argv…>` uploads the **current directory** as `input_root`, runs the command (anywhere in the team, including locally), and writes the declared outputs back into the current directory.
- Options:
  - `-o PATH` (repeatable): declare outputs. If none are given, the default is **every file created or modified under `/work`**, compared against the input tree.
  - `--remote-only`: sets `may_run_local=false`.
  - `--local-only`
  - `--cores N`, `--mem SIZE`
  - `--env host|mirror`
  - `--each GLOB`: one task per match, with `{}` in `argv` replaced by the match's path relative to the current directory. All tasks share one uploaded `input_root`.
- Uploads honour a `.zmignore` file (gitignore syntax). By default the CLI also skips `.git/` and refuses an upload over **2 GiB** unless `--force-large` is given.
- stdout/stderr stream live and the exit code is the task's, so `zm run` feels like running the command locally.

## 8. Scheduling (v1: simple and honest)

The local node is always a candidate when `may_run_local` is true, judged on the same criteria (with zero missing bytes). Remote candidates are team peers that are `Active`, match the architecture, and have announced at least the requested free cores and RAM. Ranking:
1. Fewest **missing input bytes**. Ask via `OfferTask`, or estimate from a recently seen blob set.
2. Highest idle CPU.
3. On AC power before on battery.
4. Lowest latency.

A requester running `--each` with N tasks spreads them across nodes and doesn't pile onto the top-ranked one. Keep at most `cores_free / task.cores` tasks in flight per node. **No compiler- or domain-specific heuristics in core.** The linker-pinning logic in `scheduler/heuristics.cpp` moves into the REAPI front-end plugin, which sets `may_run_remote=false` on link actions.

## 9. Security

- **Team secret:** 128 random bits, shown as a short word phrase (`zm team create` prints it; `zm team join <phrase>`). It's shared out of band.
- **Node identity:** each node generates a key pair and self-signed certificate on first start.
- **Admission:** on first contact, two nodes run a challenge–response keyed by the team secret, then pin each other's certificate fingerprints. All later `NodeApi` traffic uses **mTLS with pinned certs**. Unknown or unpinned peers are rejected.
- **Local API:** unix socket, mode `0600`, owner only.
- **Sandbox:**
  - bwrap with `--unshare-all` (no network unless the task policy asks for it and the donor's policy allows it)
  - Landlock limiting writes to the task's scratch dir
  - `SCHED_IDLE`
  - a fresh scratch dir per task, deleted afterwards
- **Mirror mode** (when it lands, M5):
  - the requester's filesystem is exported **read-only**
  - a default deny list hides secrets: `~/.ssh`, `~/.gnupg`, `~/.config/zeromesh`, keyrings, `.env*`, browser profiles
  - users can extend the deny list
- **Leaving a team:** this wipes the secret and pinned peers. Rotating the team secret means everyone re-joins (v1 limitation).

The exact crypto construction (Noise vs TLS details, HMAC framing) is the builder's call, within these constraints:
- No plaintext task or blob traffic between nodes.
- No unauthenticated submit or run.
- Use libsodium or OpenSSL (both are installed). No custom crypto primitives.

## 10. Repository layout and fate of existing code

```
core/
  store/    ← src/cas/store.*, src/cas/merkle.*                          KEEP
  net/      ← src/discovery/mdns.*, peer_table.*                         KEEP + team scoping, HMAC, real telemetry
            + telemetry.* (/proc/stat, /proc/meminfo, /sys/class/power_supply)   NEW (replaces host_guard placeholders)
            + transport (mTLS, pinning, admission)                        NEW
  exec/     ← src/executor/runner.*, bwrap.*, landlock.*                 KEEP + adapt to Task/SandboxEnv
  sched/    ← src/scheduler/*                                            REWRITE (general, §8)
  node/     daemon, ClientApi + NodeApi services, job manager            NEW
  plugin/   loader, interfaces                                           NEW
  proto/    zeromesh/v1/*.proto (proto/remote_execution.proto moves to plugins/frontend-reapi/)
plugins/
  env-host/        run using the donor's installed software              NEW (M3)
  env-mirror/      requester's files over FUSE ← src/vfs/*               ADAPT (M5): swap the DHT for an IChunkSource backed by NodeApi.PullBlobs from the requester
  env-oci/         images (v2)
  frontend-reapi/  ← src/reapi/* (stubs) + linker heuristic              REWRITE (M6)
sdk/python/        zeromesh package (gRPC stubs + ergonomic wrapper)     NEW (M5)
apps/zm-render/    Blender frame rendering                               NEW (M5)
cli/               zm (thin ClientApi client) ← src/main.cpp, src/ui/tui.*   REWRITE main, ADAPT tui
frozen/            src/dsm, src/gpu, src/verify, src/dht, include/zmdsm.h, tests/examples/torrent_*
                   — kept in git, removed from the Meson build and CI, each with a README "frozen: why, what would unfreeze it"
tests/             unit (existing framework) + integration (multi-zmd)
```

**Dependencies:**
- **gRPC C++** becomes **required**, not optional. Arch: `grpc`; Ubuntu CI: `libgrpc++-dev protobuf-compiler-grpc`.
- Protobuf, fuse3, OpenSSL and libsodium are already present.
- The Python SDK uses `grpcio`.

## 11. Milestones and acceptance tests

Each milestone is done **only** when its acceptance test passes. No ticking boxes for code that doesn't run.

| # | Milestone | Acceptance test |
|---|---|---|
| **M0** | **Honesty pass.** Rewrite PLAN.md and README.md to match §3 and this design. Move frozen modules out of the build. Make CI install gRPC and run the tests. | CI green on a clean Ubuntu runner. PLAN.md has no `[x]` on unimplemented work. README describes only what works. |
| **M1** | **Contracts.** v1 protos, plugin interfaces, generated C++ and Python stubs. A fake `zmd` that implements `ClientApi` in-memory (runs tasks locally, no network), so SDK and app work can start. | A Python script submits a 3-task job to the fake daemon and receives results and events. |
| **M2** | **Core relocation.** Keep modules moved into `core/`. Real telemetry. Persistent node IDs. `zmd` runs as a long-lived daemon with a real `ClientApi` and local execution. | `zm run -- echo hi` prints `hi` with exit 0. Running it twice with different commands gives two real results, with no false cache hits. Killing `zmd` and restarting it keeps the node ID. |
| **M3** | **Remote execution + security.** Team create/join, admission, mTLS, `NodeApi`, blob transfer, `env-host` plugin, placement. | **Two machines:** on A, `zm run --remote-only -- sh -c 'hostname > out.txt'` with output `out.txt` produces a file containing **B's** hostname. A third machine not in the team never appears in `zm status` and can't call `NodeApi`. |
| **M4** | **Fan-out + resilience.** `zm run --each`, job DAGs, retries, drain/loss re-dispatch, donor policy file. `zm run --each 'cases/*' -- ./sim {}` with 20 CPU-bound cases (each ~30 s single-threaded, inputs under 10 MB) on 3 machines completes in under 45% of the single-machine time. Killing one donor's `zmd` mid-job still completes all 20 correctly. |
| **M5** | **SDK + first app + mirror mode.** Python SDK, `zm-render`, and the `env-mirror` plugin (so donors don't need Blender installed). | `zm-render shot.blend --frames 1-240` on 4 machines produces the same frames as a local render. A donor without Blender installed renders frames through mirror mode. |
| **M6** | **REAPI front-end plugin.** | A CMake project builds through `recc` against `zmd`, with compile actions observed running on a peer. |

Integration tests for M2–M4 run **multiple `zmd` instances on one machine** (separate state dirs, sockets and ports, joined to the same team over loopback multicast). Two-machine checks are a documented manual script, `tests/acceptance/`, run before a milestone is declared done.

## 12. Configuration and paths

- State: `~/.local/share/zeromesh/` (node ID, keys, team, CAS at `cas/`)
- Runtime: `$XDG_RUNTIME_DIR/zeromesh/` (`zmd.sock`)
- Optional donor policy: `~/.config/zeromesh/donor.toml`. The defaults are safe:
  - donate only on AC power, or on battery above 50%
  - at most `cores-1` cores
  - no network for tasks
  - stop accepting work and drain if battery drops below 20%
- Ports: multicast `239.255.42.99:8988/udp` (discovery), `8981/tcp` (`NodeApi`), `127.0.0.1:8980` (REAPI front-end, only when that plugin is enabled).

## 13. Decided vs left to the builder

**Decided (my calls in this design; flag any you disagree with):**
- gRPC for both APIs (unix socket locally, mTLS between nodes). It matches REAPI and gives the Python SDK for free.
- Python is the first SDK. The C ABI (`libzm`) is deferred until a non-Python app needs it.
- `env-host` is the first environment (M3), and mirror mode comes in M5. This gets remote execution working before the hardest environment work.
- v1 = Tasks + Objects + static DAGs. Services, Channels and dynamic tasks are v2.
- The team secret + pinned mTLS security model, and the safe donor defaults.
- The DHT is frozen. Discovery is multicast-only on a single subnet.
- The restructure happens on a branch, `restructure/core-plugin`, merged to `main` milestone by milestone, with CI green at each merge.

**Left to the builder:**
- Exact proto field layout and numbering.
- The threading model inside `zmd` (thread pool vs async gRPC).
- The exact crypto construction, within the constraints in §9.
- The word list and format for the team secret phrase.
- Internal module APIs within `core/`, beyond the plugin ABI and the protos.
- How the TUI is restyled.
- Test framework: keep the in-repo one or switch to GoogleTest/Catch2.

**Confirmed by the project owner (2026-10-26):**
- v1 donors are Linux-only.
- One builder implements all milestones, including the filesystem/`env-mirror` work previously claimed in `locks/shravan-a-y`. That lock is superseded. Once the contracts (M1) and core (M2–M3) exist, the builder produces a fresh task breakdown so teammates can pick up parallel work (apps, plugins, SDK features).

## 14. Open questions

None blocking v1. Revisit before v2: team-wide ActionCache, dynamic tasks, Services/Channels, relay across subnets, macOS donors, `env-oci`.
