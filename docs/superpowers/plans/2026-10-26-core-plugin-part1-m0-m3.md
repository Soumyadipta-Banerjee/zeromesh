# ZeroMesh Core + Plugin, Part 1 (M0–M3) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Turn the repo into a small `zmd` core with plugin interfaces that can really run a sandboxed task on another team machine and bring the outputs back (milestones M0–M3 of the spec).

**Architecture:** One long-lived daemon `zmd` exposes `zeromesh.v1.ClientApi` on an owner-only unix socket and `zeromesh.v1.NodeApi` on TCP 8981 (mTLS, pinned team peers). Inside it:
- `JobManager` turns job specs into tasks.
- A `Placer` picks the local node or a peer.
- `SandboxRunner` executes the task in bwrap, using a `SandboxEnv` produced by an `IEnvProvider` plugin (`env-host` in this part).

The CLI `zm` is a thin ClientApi client.

**Tech Stack:** C++20, Meson/Ninja, gRPC C++ 1.84 + protobuf 36, OpenSSL 3 (certs, SHA-256), libsodium (randomness, HMAC-SHA256), bubblewrap, Python 3 + grpcio (integration tests only).

**Spec:** `docs/superpowers/specs/2026-10-26-core-plugin-architecture-design.md`

## Global Constraints

- Linux only. C++20 (`cpp_std=c++20`), `warning_level=2`, and the build must stay warning-clean for new code.
- gRPC C++ and protobuf are **required** Meson dependencies. Arch: `grpc python-grpcio python-grpcio-tools`. Ubuntu CI: `libgrpc++-dev protobuf-compiler-grpc libprotobuf-dev libsodium-dev libssl-dev bubblewrap python3-grpcio python3-grpc-tools`.
- Ports: discovery multicast `239.255.42.99:8988/udp`; NodeApi `8981/tcp`. Both are overridable for tests via `ZM_DISCOVERY_PORT` and `ZM_NODE_PORT`.
- Paths:
  - state: `~/.local/share/zeromesh/` (override `ZM_STATE_DIR`)
  - runtime: `$XDG_RUNTIME_DIR/zeromesh/zmd.sock` (override `ZM_RUNTIME_DIR`)
  - donor policy: `~/.config/zeromesh/donor.toml` (override `ZM_CONFIG_DIR`)
- The ClientApi socket file mode is `0600`. Key files and the team file are mode `0600`.
- Digest = lowercase hex SHA-256 (64 chars) + `size_bytes`. This matches the existing `CasStore`. *(Deviation from the spec's `sha256:` prefix, which adds nothing.)*
- Sandbox:
  - bwrap with `--unshare-all --die-with-parent`
  - inputs at `/work`, cwd `/work/<working_dir>`
  - only `/work` and `/tmp` are writable
  - no network unless `policy.allow_network` and the donor policy both allow it
  - `SCHED_IDLE` on the child
  - env var `ZM_NODE_ID` = id of the executing node
- Landlock is **not** stacked with bwrap. Kernel docs (v7.3.0): *"Threads sandboxed with filesystem restrictions cannot modify filesystem topology, whether via mount(2) or pivot_root(2)."* bwrap's mount layout is the write boundary. `landlock.*` stays in the tree, unused.
- Nothing domain-specific (gcc, blender, linker) may appear under `core/`.
- Namespaces match directories: `zeromesh::store`, `zeromesh::net`, `zeromesh::exec`, `zeromesh::sched`, `zeromesh::node`, `zeromesh::plugin`, `zeromesh::cli`. Include root is the repo root plus `core/`, so includes look like `#include "store/store.hpp"`.
- No checkbox in PLAN.md is ticked unless its acceptance test passes.

## Review Focus

1. **Paths with spaces, unicode and symlinks** in uploaded trees and outputs. A symlink pointing outside the tree must be stored as a symlink, never followed, and an output symlink must never let a task write or read outside `/work`. Tests in Task 6 and Task 7.
2. **Huge or binary stdout** (MBs, NUL bytes). It must not deadlock or bloat memory: the full stream goes to CAS, and only a 64 KiB head is inline. Test in Task 7.
3. **Command not found on the executing node.** The task must be rejected cleanly (offer rejected, or exit 127 with a message), never hang. Tests in Task 10 and Task 15.
4. **`zm` invoked while `zmd` isn't running.** It must print `zmd is not running (start it with: zmd)` and exit 2. Test in Task 11.
5. **A peer that disappears mid-transfer or mid-run.** The task fails with a clear error within 10 s of missed heartbeats, and nothing hangs. (Retry and re-dispatch arrive in Part 2.) Test in Task 16.

---

## File Structure (end of Part 1)

```
proto/zeromesh/v1/common.proto   Digest, TaskSpec, EnvSpec, Resources, Policy, JobSpec, TaskResult, JobEvent, PeerInfo
proto/zeromesh/v1/client.proto   service ClientApi
proto/zeromesh/v1/node.proto     service NodeApi
core/store/   store.{hpp,cpp} merkle.{hpp,cpp} ignore.{hpp,cpp}
core/exec/    sandbox_runner.{hpp,cpp} bwrap.{hpp,cpp} landlock.{hpp,cpp} priority.{hpp,cpp}
core/net/     discovery.{hpp,cpp} peer_table.{hpp,cpp} telemetry.{hpp,cpp} team.{hpp,cpp} identity.{hpp,cpp} tls.{hpp,cpp}
core/plugin/  plugin.hpp loader.{hpp,cpp}
core/node/    state.{hpp,cpp} task_key.{hpp,cpp} action_cache.{hpp,cpp} executor.hpp local_executor.{hpp,cpp}
              job_manager.{hpp,cpp} client_service.{hpp,cpp} node_service.{hpp,cpp} node_client.{hpp,cpp}
              donor_policy.{hpp,cpp} admission.{hpp,cpp} daemon.{hpp,cpp}
core/sched/   placer.{hpp,cpp} rank.{hpp,cpp}
plugins/env-host/  env_host.{hpp,cpp}
zmd/main.cpp
cli/          main.cpp client.{hpp,cpp} run_cmd.{hpp,cpp} status_cmd.{hpp,cpp} team_cmd.{hpp,cpp} tui.{hpp,cpp}
tests/unit/   test_*.cpp (+ test_framework.hpp, test_main.cpp), tests/plugins/fake_env.cpp
tests/integration/  zmtest.py (harness), test_local_daemon.py, test_remote_exec.py
tests/acceptance/   m3_two_machines.md
frozen/       dsm/ gpu/ verify/ dht/ vfs/ reapi/ scheduler/ examples/ include/ + README.md each
```

---

## Milestone M0: Honesty pass

### Task 1: Freeze dead modules and make the build and CI honest

**Files:**
- Move (git mv):
  - `src/dsm`, `src/gpu`, `src/verify`, `src/dht`, `src/vfs`, `src/reapi`, `src/scheduler`, `src/cas/p2p_syncer.*` → `frozen/<same name>/`
  - `include/zmdsm.h` → `frozen/include/`
  - `tests/examples/` → `frozen/examples/`
  - `tests/test_{dht,dsm,verify,vfs,torrent_swarm,p2p_syncer,reapi,scheduler,host_guard}.cpp` → beside their module in `frozen/`
- Create: `frozen/README.md` (one table: module, why frozen, what would unfreeze it, which spec milestone revives it). `vfs` → "revived as `plugins/env-mirror` in M5". `reapi` + `scheduler/heuristics` → "revived as `plugins/frontend-reapi` in M6".
- Modify:
  - `meson.build`: only remaining sources; gRPC/protobuf/libsodium now `required: true`; drop the GTK target.
  - `Makefile`: `test` → `meson test -C build --print-errorlogs`.
  - `.github/workflows/ci.yml`: install the Ubuntu packages from Global Constraints, then run `make build` and `make test`.
  - `src/main.cpp`: keep only `version` and `status`. Remove `up/run/mount/publish`, which were broken or fake.
  - `src/ui/tui.*`: drop the `Scheduler` dependency. The constructor takes only the `PeerTable`.

- [ ] **Step 1:** Make the moves and edits above. `host_guard.hpp` is still used by `runner.cpp` and `mdns.cpp` until Tasks 7–8, so keep it in place for now.
- [ ] **Step 2:** Run `rm -rf build && meson setup build && ninja -C build && meson test -C build --print-errorlogs`. Expected: builds with no reference to frozen code, and `all_unit_tests` passes.
- [ ] **Step 3:** Run `./build/zm run x`. Expected: usage text and exit 1. The fake pipeline is gone.
- [ ] **Step 4:** Commit: `chore: freeze unimplemented modules, make gRPC required, honest CI`.

### Task 2: Rewrite PLAN.md and README.md to match reality

**Files:** Modify `PLAN.md` and `README.md`.

- [ ] **Step 1:** Rewrite `PLAN.md`:
  - §1: the spec's architecture diagram, plus a link to the spec.
  - §2: the module fate table (spec §10).
  - §3: milestones M0–M6 with the spec's acceptance tests verbatim. Only M0 is `[x]`, and only after Task 1 passes CI.
  - Keep ADR-001…010 under a heading "Historical ADRs (superseded by the 2026-10-26 spec)".
  - Add ADR-011 "Core + plugin architecture" (link the spec).
  - Add ADR-012 "bwrap mount layout instead of stacked Landlock", with the kernel-doc quote from Global Constraints.
- [ ] **Step 2:** Rewrite `README.md`: what ZeroMesh is (spec §1, 3 sentences), current status (what works today), build instructions with the new dependencies, and a "Roadmap" link to PLAN.md. Remove the claims about DSM, FUSE, REAPI, recc and Bazel.
- [ ] **Step 3:** Verify: `grep -nE '\[x\]' PLAN.md` shows only M0 lines.
- [ ] **Step 4:** Commit: `docs: rewrite PLAN and README to match the core+plugin design`.

---

## Milestone M1: Contracts

### Task 3: v1 protos and code generation

**Files:**
- Create: `proto/zeromesh/v1/{common,client,node}.proto`, `tests/unit/test_proto.cpp`
- Modify: `meson.build`. Add a `custom_target` per proto using `protoc` + `grpc_cpp_plugin` (C++), and a second target with `python3 -m grpc_tools.protoc` writing to `build/py/`. Add a `zm_proto` static library.

**Interfaces (produces; package `zeromesh.v1`; field numbers are the builder's):**

`common.proto`:
- `Digest{hash, size_bytes}`
- `EnvSpec{name, map<string,string> params}`
- `Resources{cores, mem_bytes, arch}`
- `Policy{may_run_local=true, may_run_remote=true, max_retries=3, timeout_s=3600, allow_network=false, preemptible=true}`. Proto3 has no defaults; the defaults are applied by `node::Normalize()` in Task 5.
- `InputFrom{task, path}`
- `TaskSpec{name, argv[], map env, Digest input_root, working_dir, outputs[], capture_changed, EnvSpec env_spec, Resources resources, Policy policy, depends_on[], InputFrom inputs_from[]}`
- `JobSpec{name, TaskSpec tasks[]}`
- `OutputEntry{path, Digest digest, is_dir, is_executable}`
- `TaskResult{task_key, exit_code, timed_out, Digest stdout, Digest stderr, bytes stdout_head, bytes stderr_head, OutputEntry outputs[], executed_by, duration_ms, cache_hit, error}`
- `TaskState{QUEUED, PLACED, RUNNING, SUCCEEDED, FAILED, CANCELLED}`
- `JobEvent{job_id, task_name, TaskState state, string detail, bytes log_chunk, int32 log_fd, TaskResult result}`
- `JobStatus{job_id, name, map<string,TaskState> tasks, map<string,TaskResult> results, bool done}`
- `PeerInfo{node_id, hostname, address, node_port, arch, cores_total, cores_free, mem_available, cpu_load_pct, battery_pct, on_ac, state, is_self}`
- Reserved message names for v2: `ServiceSpec`, `ChannelSpec` (empty messages with a comment).

`client.proto` (`service ClientApi`):
- `PutObjects(stream Chunk) returns (PutObjectsReply)`, where `Chunk{bytes data, bool last}` delimits one object per run of chunks
- `GetObjects(Digests) returns (stream Chunk)`
- `FindMissing(Digests) returns (Digests)`
- `UploadTree(UploadTreeRequest{path, force_large}) returns (UploadTreeReply{Digest root, int64 total_bytes})`
- `SubmitJob(JobSpec) returns (SubmitReply{job_id})`
- `WatchJob(JobRef) returns (stream JobEvent)`
- `GetJob(JobRef) returns (JobStatus)`
- `CancelJob(JobRef) returns (Empty)`
- `FetchOutputs(FetchRequest{job_id, task_name, dest_path, only_changed}) returns (FetchReply{written[]})`
- `ListPeers(Empty) returns (PeerList)`
- `TeamCreate(Empty) returns (TeamSecretReply{phrase})`
- `TeamJoin(TeamJoinRequest{phrase}) returns (Empty)`
- `TeamLeave(Empty) returns (Empty)`
- `TeamInfo(Empty) returns (TeamInfoReply{in_team, team_id, peers_pinned})`

`node.proto` (`service NodeApi`):
- `Admit(AdmitHello{node_id, nonce}) returns (AdmitChallenge{node_id, nonce, mac})`
- `AdmitConfirm(AdmitProof{mac}) returns (Empty)`
- `Ping(Empty) returns (PeerInfo)`
- `OfferTask(Offer{task_id, TaskSpec spec, Digest needed[]}) returns (OfferReply{accepted, reason, Digest missing[]})`
- `PushBlobs(stream Chunk) returns (Empty)`
- `PullBlobs(Digests) returns (stream Chunk)`
- `RunTask(RunRequest{task_id}) returns (stream JobEvent)`
- `CancelTask(TaskRef) returns (Empty)`
- `Drain(DrainNotice{node_id, reason}) returns (Empty)`

- [ ] **Step 1: Write the failing test.** In `test_proto.cpp`, `TEST(ProtoTaskSpecRoundTrip)` builds a `TaskSpec` with argv `{"sh","-c","echo hi"}`, `env_spec.name="host"` and `outputs={"out.txt"}`, serializes it, parses it back, and `ASSERT_EQ`s each field.
- [ ] **Step 2:** `ninja -C build`. Expected: FAIL, because the generated headers don't exist yet.
- [ ] **Step 3:** Write the three protos and the Meson codegen.
- [ ] **Step 4:** `ninja -C build && meson test -C build`. Expected: PASS. `python3 -c "import sys; sys.path.insert(0,'build/py'); from zeromesh.v1 import client_pb2_grpc"` exits 0.
- [ ] **Step 5:** Commit: `feat(proto): zeromesh.v1 ClientApi/NodeApi contracts with C++ and Python codegen`.

### Task 4: Plugin ABI and loader

**Files:**
- Create: `core/plugin/plugin.hpp`, `core/plugin/loader.{hpp,cpp}`, `tests/plugins/fake_env.cpp` (built as `shared_module('zm_fake_env', …)`), `tests/unit/test_plugin.cpp`

**Interfaces (produces):**
```cpp
namespace zeromesh::plugin {
inline constexpr uint32_t kAbiMajor = 1;
struct Bind { std::string host; std::string sandbox; bool read_only{true}; };
struct Symlink { std::string target; std::string link; };          // bwrap --symlink target link
struct SandboxEnv { std::vector<Bind> binds; std::vector<Symlink> symlinks;
                    std::vector<std::pair<std::string,std::string>> env; };
struct EnvParams { std::map<std::string,std::string> params; std::vector<std::string> argv; };
struct Availability { bool ok{false}; std::string reason; };
struct TaskContext { std::string task_id; EnvParams env; std::filesystem::path scratch_dir; };
template <class T> struct Result { std::optional<T> value; std::string error; };
class IEnvProvider { /* Name, Check, Prepare, Release exactly as spec §6.2 */ };
class ClientApiHandle;                       // opaque; defined by node in Part 3
struct PluginConfig { std::map<std::string,std::string> values; };
class IFrontend { /* Name, Start(ClientApiHandle&, const PluginConfig&), Stop: spec §6.2 */ };
struct PluginEntry { uint32_t abi_major; const char* name;
                     IEnvProvider* (*make_env)();   // may be null
                     IFrontend* (*make_frontend)(); // may be null
};
}
extern "C" const zeromesh::plugin::PluginEntry* zm_plugin_entry();
namespace zeromesh::plugin {
class Registry {
public:
  void RegisterEnv(std::unique_ptr<IEnvProvider>);            // static built-ins
  bool LoadFile(const std::filesystem::path& so, std::string* error);   // dlopen + abi check
  size_t LoadSearchPath();   // $ZM_PLUGIN_PATH (':'-separated), ~/.local/lib/zeromesh/plugins, <prefix>/lib/zeromesh/plugins
  IEnvProvider* Env(std::string_view name) const;             // nullptr if absent
  std::vector<std::string> EnvNames() const;
};
}
```

- [ ] **Step 1: Write the failing tests:**
  - `TEST(PluginLoadsSharedModule)`: `LoadFile(<build>/tests/plugins/libzm_fake_env.so)` is true, `Env("fake")` is non-null, and `Env("fake")->Check({}).ok` is true. The path is passed via the `ZM_TEST_PLUGIN_DIR` env set in meson `test()`.
  - `TEST(PluginRejectsAbiMismatch)`: the fake module built with `-DFAKE_ABI=99` is a second module, `zm_fake_env_badabi`. `LoadFile` is false and the error contains `"abi"`.
  - `TEST(PluginStaticRegistration)`: `RegisterEnv` a local class named `"static"`, and `Env("static")` is non-null.
- [ ] **Step 2:** Run. Expected: FAIL to compile.
- [ ] **Step 3:** Implement. `dlopen(RTLD_NOW|RTLD_LOCAL)`; keep handles alive for the registry's lifetime; on duplicate names, the first registration wins.
- [ ] **Step 4:** Run. Expected: PASS.
- [ ] **Step 5:** Commit: `feat(plugin): IEnvProvider/IFrontend ABI and registry with dlopen loader`.

### Task 5: Task identity and spec normalization

**Files:**
- Create: `core/node/task_key.{hpp,cpp}`, `tests/unit/test_task_key.cpp`

**Interfaces (produces):**
- `void node::Normalize(v1::TaskSpec&)` applies the Policy defaults from Task 3 when fields are zero/unset. Use `has_policy()` to detect "unset" and fill all defaults. `max_retries` is 3, `timeout_s` is 3600, and `working_dir` defaults to `"."`.
- `std::string node::TaskKey(const v1::TaskSpec&)` returns hex SHA-256 over a canonical encoding of argv, sorted env, input_root, working_dir, sorted outputs, capture_changed, env_spec (sorted params), resources.arch and policy.allow_network. **Excluded:** name, depends_on, inputs_from (already reflected in input_root after overlay, Task 9), cores/mem, retries, timeout, may_run_*.

- [ ] **Step 1: Write the failing tests:**
  - `TEST(TaskKeyStable)`: two identical specs give equal keys of length 64.
  - `TEST(TaskKeyIgnoresName)`: `name` "a" vs "b" gives equal keys.
  - `TEST(TaskKeyArgvMatters)`: argv `echo hi` vs `echo ho` gives different keys.
  - `TEST(TaskKeyEnvOrderIrrelevant)`: env inserted in different orders gives equal keys.
  - `TEST(NormalizeDefaults)`: an empty spec after `Normalize` has `max_retries==3`, `timeout_s==3600`, `may_run_local` and `may_run_remote` true, and `working_dir=="."`.
- [ ] **Step 2:** Run. Expected: FAIL.
- [ ] **Step 3:** Implement. Use a length-prefixed field encoding, so there are no delimiter collisions.
- [ ] **Step 4:** Run. Expected: PASS.
- [ ] **Step 5:** Commit: `feat(node): canonical task key and spec normalization`.

---

## Milestone M2: Core relocation and a real local daemon

*(The spec's M1 "fake in-memory zmd" is dropped. It existed so a parallel team could start on the SDK, and with one builder the real local daemon arrives in M2 at the same cost. The M1 Python acceptance check runs against the real daemon in Task 11.)*

### Task 6: `core/store`: move CAS + Merkle, add overlay and ignore rules

**Files:**
- Move: `src/cas/{store,merkle}.*` → `core/store/`, namespace `zeromesh::cas` → `zeromesh::store`. Update `tests/test_cas.cpp` and `tests/test_merkle.cpp`, and move them to `tests/unit/`.
- Create: `core/store/ignore.{hpp,cpp}`, `tests/unit/test_ignore.cpp`
- Modify: `core/store/merkle.{hpp,cpp}`

**Interfaces (produces):**
- Everything existing in `CasStore` and `MerkleBuilder` keeps its signature.
- Add `MerkleBuilder::BuildTree(const std::filesystem::path& root, const IgnoreRules& rules, uint64_t* total_bytes)`.
- Add `Digest MerkleBuilder::Overlay(const Digest& base_tree, const std::string& at_path, const Digest& item, bool item_is_tree)`. It returns a new root with `item` placed at `at_path`, creating intermediate dirs. If `item` is a tree, its entries merge into that directory, and the overlay wins on name clashes.
- `class store::IgnoreRules { static IgnoreRules ForRoot(const std::filesystem::path& root); bool Ignored(std::string_view rel_path, bool is_dir) const; }`. It always ignores `.git/` and reads `<root>/.zmignore`. Supported syntax: comments, blank lines, `!negation`, trailing `/` (directory only), leading `/` (anchored), `*`, `?`, `**`. The last matching rule wins.
- Symlinks are stored as `SymlinkNode` and never followed, on both build and materialize. `MaterializeTree` refuses entries whose name contains `/`, `..`, or is empty, and returns false.

- [ ] **Step 1: Write the failing tests:**
  - `TEST(IgnoreGitAlways)`: `.git/config` is ignored.
  - `TEST(IgnoreDirOnly)`: rule `build/` ignores dir `build` but not file `build`.
  - `TEST(IgnoreNegation)`: rules `*.log` + `!keep.log` ignore `a.log` and not `keep.log`.
  - `TEST(IgnoreDoubleStar)`: `**/tmp` ignores `a/b/tmp`.
  - `TEST(IgnoreAnchored)`: `/out` ignores `out` but not `x/out`.
  - `TEST(MerkleOverlayFile)`: base tree `{a.txt}`, overlay file at `frames/f1.png` → materialized dir has both `a.txt` and `frames/f1.png`.
  - `TEST(MerkleSymlinkNotFollowed)`: tree with `link -> /etc/passwd` → manifest has a symlink node, and the materialized `link` is a symlink with the same target, not a copy.
  - `TEST(MerkleRejectsTraversal)`: hand-crafted manifest with file name `../evil` → `MaterializeTree` returns false and creates nothing outside the destination.
  - `TEST(MerkleUnicodeSpaces)`: file `dir with space/ünï.txt` round-trips.
- [ ] **Step 2:** Run. Expected: FAIL.
- [ ] **Step 3:** Implement the moves, the namespace rename, the overlay and the ignore rules.
- [ ] **Step 4:** Run the full suite. Expected: PASS.
- [ ] **Step 5:** Commit: `refactor(store): move CAS to core/store; add tree overlay, .zmignore rules, traversal guards`.

### Task 7: `core/exec`: bwrap-based `SandboxRunner`

**Files:**
- Move: `src/executor/{bwrap,landlock}.*` → `core/exec/` (namespace `zeromesh::exec`)
- Create: `core/exec/priority.{hpp,cpp}` (`bool exec::EnterIdlePriority()`: `sched_setscheduler(0, SCHED_IDLE)`, falling back to `setpriority(19)`), `core/exec/sandbox_runner.{hpp,cpp}`, `tests/unit/test_sandbox_runner.cpp`
- Delete: `src/executor/runner.*`, `src/executor/host_guard.hpp`. Update `mdns.cpp` to stop using `host_guard`; Task 8 replaces it with telemetry, so pass zeros for now.
- Modify: `core/exec/bwrap.{hpp,cpp}`. Add `AddReadOnlyBind`, `AddSymlink`, `ShareNetwork(bool)` (adds `--share-net` after `--unshare-all`), and `SetHostname`. Remove the CAS-root concept.

**Interfaces (produces):**
```cpp
namespace zeromesh::exec {
struct RunSpec { std::vector<std::string> argv; std::vector<std::pair<std::string,std::string>> env;
  store::Digest input_root; std::string working_dir{"."}; std::vector<std::string> outputs;
  bool capture_changed{false}; std::chrono::seconds timeout{3600}; bool allow_network{false};
  plugin::SandboxEnv sandbox_env; std::string node_id; };
struct OutputEntry { std::string path; store::Digest digest; bool is_dir{false}; bool is_executable{false}; };
struct RunResult { int exit_code{-1}; bool timed_out{false}; bool cancelled{false};
  store::Digest stdout_digest, stderr_digest; std::string stdout_head, stderr_head;   // head ≤ 64 KiB
  std::vector<OutputEntry> outputs; std::chrono::milliseconds duration{0}; std::string error; };
using LogSink = std::function<void(int fd, std::string_view chunk)>;   // fd 1|2; may be empty
class SandboxRunner {
public:
  SandboxRunner(std::shared_ptr<store::CasStore>, std::shared_ptr<store::MerkleBuilder>, std::filesystem::path scratch_base);
  RunResult Run(const RunSpec&, const LogSink&, std::stop_token);
};
}
```

**Behaviour the tests pin:**
- Scratch dir is `<scratch_base>/<random>`, and the input tree is materialized at `<scratch>/work` and bind-mounted rw at `/work`. Always add `--tmpfs /tmp --dev /dev --proc /proc --chdir /work/<working_dir> --setenv ZM_NODE_ID <id>`, plus `sandbox_env` binds and symlinks.
- stdout/stderr are streamed to the `LogSink` and to temp files. After exit they're `PutBlobFile`d into CAS, with heads ≤ 64 KiB.
- Timeout or cancel sends `SIGKILL` to the bwrap process group (`setpgid` in the child).
- Outputs: declared paths relative to `/work`. Files become blobs, dirs become trees via `BuildTree`, and symlinks are stored as symlink entries and never followed.
- `capture_changed`: compare a pre-run snapshot (path → size+mtime_ns+digest) and capture new or modified regular files.
- The scratch dir is always removed.

- [ ] **Step 1: Write the failing tests.** Each test uses a `SandboxEnv` that binds host `/usr` read-only and mirrors `/bin`, `/lib` and `/lib64` as symlinks when they are symlinks on the host, otherwise as ro binds. This is the same logic as Task 10; a small test helper is acceptable.
  - `TEST(RunnerEchoStdout)`: `sh -c 'echo hi'` → `exit_code==0`, `stdout_head=="hi\n"`.
  - `TEST(RunnerExitCode)`: `sh -c 'exit 7'` → 7.
  - `TEST(RunnerDeclaredOutput)`: `sh -c 'echo x > out.txt'` with outputs `{"out.txt"}` → one entry, and the blob content is `"x\n"`.
  - `TEST(RunnerCaptureChanged)`: input tree has `in.txt`; command `sh -c 'echo y > new.txt'` with `capture_changed` → outputs contain `new.txt` and not `in.txt`.
  - `TEST(RunnerTimeout)`: `sleep 30` with timeout 1 s → `timed_out` true, `duration < 5 s`.
  - `TEST(RunnerCancel)`: `sleep 30`, request stop after 200 ms → `cancelled` true.
  - `TEST(RunnerNoNetwork)`: `sh -c 'wc -l < /proc/net/dev'` → `stdout_head=="3\n"` (header lines + lo only).
  - `TEST(RunnerNodeIdEnv)`: `sh -c 'printf %s "$ZM_NODE_ID"'` with node_id `"abc"` → `"abc"`.
  - `TEST(RunnerHugeBinaryStdout)`: `head -c 5000000 /dev/urandom` → `stdout_digest.size_bytes==5000000`, `stdout_head.size()==65536`.
  - `TEST(RunnerCannotWriteOutsideWork)`: `sh -c 'echo z > /usr/zm_test || echo denied'` → stdout contains `denied`.
  - `TEST(RunnerOutputSymlinkNotFollowed)`: `sh -c 'ln -s /etc/hostname out'` with outputs `{"out"}` → the entry is a symlink, and its digest is not the content of `/etc/hostname`.
  - `TEST(RunnerCommandNotFound)`: argv `{"definitely-not-a-binary-xyz"}` → `exit_code==127`, and `stderr_head` is non-empty.
  - `TEST(RunnerIdlePriority)`: `cut -d' ' -f41 /proc/self/stat` → `"5\n"` (SCHED_IDLE). The child calls `exec::EnterIdlePriority()` after `fork` and before `exec` of bwrap.
- [ ] **Step 2:** Run. Expected: FAIL.
- [ ] **Step 3:** Implement.
- [ ] **Step 4:** Run. Expected: PASS. Port the old `test_executor.cpp` and `test_bwrap.cpp` cases that still apply, and delete the rest.
- [ ] **Step 5:** Commit: `feat(exec): bwrap SandboxRunner with /work layout, streaming logs, timeouts, output capture`.

### Task 8: `core/net`: relocation, real telemetry, persistent node identity

**Files:**
- Move: `src/discovery/{mdns,peer_table}.*` → `core/net/{discovery,peer_table}.*`. Namespace `zeromesh::net`; rename `MdnsEngine` → `Discovery` and `MdnsConfig` → `DiscoveryConfig`.
- Create: `core/net/telemetry.{hpp,cpp}`, `core/node/state.{hpp,cpp}`, `tests/unit/test_telemetry.cpp`, `tests/unit/test_state.cpp`

**Interfaces (produces):**
- `struct net::Telemetry { int cores_total; int cores_free; uint64_t mem_total, mem_available; double cpu_load_pct; int battery_pct /*-1 none*/; bool on_ac; std::string arch; }`
- `class net::TelemetrySampler { explicit TelemetrySampler(std::filesystem::path sysroot = "/"); Telemetry Sample(); }`
  - CPU is the delta between consecutive `/proc/stat` samples; the first call returns 0.
  - `cores_free = round(cores_total * (1 - load))`
  - memory comes from `/proc/meminfo` `MemAvailable`
  - battery: the first `/sys/class/power_supply/*` with `type==Battery` gives `capacity`. `on_ac` is any `type==Mains` with `online==1`, or no battery at all.
  - arch comes from `uname().machine`
- `class node::NodeState { static NodeState LoadOrCreate(const std::filesystem::path& state_dir); const std::string& node_id() const; const std::filesystem::path& dir() const; }`. The node_id is 32 lowercase hex chars from `randombytes_buf`, stored in `<dir>/node_id` (mode 0600).
- `PeerTable::Peer` gains `arch`, `node_port`, `cores_free` and `mem_available`. `SelectBestWorker` is removed (ranking moves to `sched`).

- [ ] **Step 1: Write the failing tests:**
  - `TEST(TelemetryFakeSysroot)`: a temp sysroot with `proc/stat` (two samples written between `Sample()` calls: idle-only, then 50% busy), `proc/meminfo` (`MemTotal: 16000000 kB`, `MemAvailable: 8000000 kB`) and `sys/class/power_supply/BAT0/{type=Battery,capacity=42}`, `AC/{type=Mains,online=0}` → `cpu_load_pct≈50` (±1), `mem_available==8192000000`, `battery_pct==42`, `on_ac==false`.
  - `TEST(TelemetryNoBatteryMeansAc)`.
  - `TEST(StatePersistsId)`: `LoadOrCreate(tmp)` twice gives equal ids of length 32.
  - `TEST(StateDistinctDirs)`: two dirs give different ids.
  - Update `test_mdns.cpp` → `test_discovery.cpp` and `test_peer_table.cpp`.
- [ ] **Step 2:** Run. Expected: FAIL.
- [ ] **Step 3:** Implement. `Discovery` takes a `std::function<net::Telemetry()>` for announcements.
- [ ] **Step 4:** Run. Expected: PASS.
- [ ] **Step 5:** Commit: `feat(net): real telemetry, persistent node identity, discovery relocation`.

### Task 9: ActionCache, Executor interface, LocalExecutor, JobManager

**Files:**
- Create: `core/node/{action_cache,local_executor,job_manager}.{hpp,cpp}`, `core/node/executor.hpp`, `tests/unit/test_job_manager.cpp`

**Interfaces (produces):**
```cpp
namespace zeromesh::node {
class ActionCache {   // persists v1::TaskResult (serialized proto) in CasStore's action-result area, keyed by task_key
public: explicit ActionCache(std::shared_ptr<store::CasStore>);
  std::optional<v1::TaskResult> Get(const std::string& task_key) const;   // only if exit 0 AND every output digest present
  void Put(const std::string& task_key, const v1::TaskResult&); };
struct TaskCallbacks { std::function<void(v1::TaskState, std::string detail)> on_state;
  std::function<void(int fd, std::string_view)> on_log; std::function<void(v1::TaskResult)> on_done; };
class Executor { public: virtual ~Executor() = default;
  virtual void Execute(const std::string& task_id, const v1::TaskSpec& spec, TaskCallbacks cb) = 0;  // async; on_done exactly once
  virtual void Cancel(const std::string& task_id) = 0; };
class LocalExecutor final : public Executor {   // thread pool sized to cores; uses Registry env provider + SandboxRunner
public: LocalExecutor(std::shared_ptr<exec::SandboxRunner>, std::shared_ptr<plugin::Registry>, std::string node_id, int slots); };
class EventStream { public: std::optional<v1::JobEvent> Next(std::chrono::milliseconds timeout); void Close(); };
class JobManager {
public: JobManager(std::shared_ptr<store::CasStore>, std::shared_ptr<store::MerkleBuilder>,
                   std::shared_ptr<ActionCache>, std::shared_ptr<Executor>);
  std::string Submit(v1::JobSpec);          // throws std::invalid_argument: duplicate/unknown task names, cycles, unknown env
  void Cancel(const std::string& job_id);
  std::optional<v1::JobStatus> Get(const std::string& job_id) const;
  std::shared_ptr<EventStream> Watch(const std::string& job_id);   // replays past events, then live; closes when job done
};
}
```

**Behaviour:**
- `Submit` normalizes every task. When a task's dependencies have succeeded, it builds the task's effective `input_root` by `Overlay`ing each `inputs_from` output (a dir output → tree, a file output → file) onto the task's own `input_root`. It then computes `TaskKey`, checks the `ActionCache` (a hit yields a `SUCCEEDED` event with `cache_hit=true`), and otherwise calls `Executor::Execute`.
- A failed dependency marks its dependents `FAILED` with detail `"dependency <name> failed"`.
- The job is done when every task is terminal.
- `job_id` = 16 random hex chars.
- LocalExecutor rejects an unknown `env_spec.name` with `FAILED` and error `"no env provider '<name>'"`.

- [ ] **Step 1: Write the failing tests.** Use a `FakeExecutor` that records calls and completes them synchronously with configurable results, plus one real-runner test.
  - `TEST(JobRejectsCycle)`: a→b→a throws.
  - `TEST(JobRejectsUnknownDep)`.
  - `TEST(JobRunsDagInOrder)`: c depends on a and b; FakeExecutor observes c last.
  - `TEST(JobOverlayInputsFrom)`: a's output `frames/` (tree) → c's effective input_root contains `frames/f1`. Assert via the FakeExecutor-captured spec, by materializing its input_root.
  - `TEST(JobCacheHitSkipsExecutor)`: submit the same single task twice → the second run has a `SUCCEEDED` event with `cache_hit`, and the executor was called once.
  - `TEST(JobNoFalseCacheHit)`: `echo a` then `echo b` → both executed.
  - `TEST(JobDependencyFailurePropagates)`.
  - `TEST(JobWatchReplaysHistory)`: Watch after the job is done still yields all events, then end.
  - `TEST(JobRealLocalExecutor)`: LocalExecutor with a test env provider (Task 7's helper) runs `sh -c 'echo hi'` → result `stdout_head=="hi\n"`.
- [ ] **Step 2:** Run. Expected: FAIL.
- [ ] **Step 3:** Implement.
- [ ] **Step 4:** Run. Expected: PASS.
- [ ] **Step 5:** Commit: `feat(node): job manager with DAG, overlay inputs, action cache, local executor`.

### Task 10: `env-host` plugin

**Files:**
- Create: `plugins/env-host/env_host.{hpp,cpp}` (compiled into `zmd` and registered statically as `"host"`; also built as `shared_module` for loader testing), `tests/unit/test_env_host.cpp`

**Interfaces (produces):** `std::unique_ptr<plugin::IEnvProvider> envhost::Make(std::string path_env = getenv("PATH"))`

**Behaviour:**
- `Check`: ok when `argv[0]` contains `/` and is relative (it lives in the input tree), or when it resolves to an executable regular file on `path_env`. Otherwise the reason is `"command '<x>' not found on this node"`.
- `Prepare`:
  - ro-bind `/usr`
  - for each of `/bin`, `/sbin`, `/lib`, `/lib32`, `/lib64`: if it's a symlink on the host, emit the same `Symlink`; if it's a dir, emit a ro `Bind`; if absent, skip it
  - ro-bind `/etc/alternatives`, `/etc/ld.so.cache` and `/etc/ssl` when they exist
  - env `PATH=/usr/local/bin:/usr/bin:/bin`

- [ ] **Step 1: Write the failing tests:**
  - `TEST(EnvHostFindsSh)`: `Check({argv={"sh"}}).ok`.
  - `TEST(EnvHostMissingCommand)`: `Check({argv={"definitely-not-a-binary-xyz"}})` is not ok, and the reason contains `not found on this node`.
  - `TEST(EnvHostRelativeScriptOk)`: `./sim` is ok.
  - `TEST(EnvHostMirrorsUsrMerge)`: for each of `/bin` and `/lib`, the Prepare output has a Symlink if `std::filesystem::is_symlink(p)` on the host, otherwise a Bind.
  - Re-run `TEST(RunnerEchoStdout)` using `envhost::Make()->Prepare(...)` instead of the test helper, then delete the helper.
- [ ] **Step 2:** Run. Expected: FAIL.
- [ ] **Step 3:** Implement.
- [ ] **Step 4:** Run. Expected: PASS.
- [ ] **Step 5:** Commit: `feat(plugins): env-host environment provider`.

### Task 11: `zmd` daemon with ClientApi, plus the `zm` CLI (local)

**Files:**
- Create: `core/node/client_service.{hpp,cpp}`, `core/node/daemon.{hpp,cpp}`, `zmd/main.cpp`, `cli/{main,client,run_cmd,status_cmd}.{hpp,cpp}`, `cli/tui.{hpp,cpp}` (moved from `src/ui`, now fed by `ListPeers`), `tests/integration/zmtest.py`, `tests/integration/test_local_daemon.py`
- Delete: `src/main.cpp`, `src/ui/`. The `zm` binary now builds from `cli/`.
- Modify: `meson.build` (targets `zmd`, `zm`; `test('integration_local', python3, args: [...], env: {ZM_BUILD_DIR, PYTHONPATH=build/py})`)

**Interfaces (produces):**
- `class node::Daemon { explicit Daemon(DaemonConfig); int Run(); void Shutdown(); }`. `DaemonConfig{state_dir, runtime_dir, config_dir, node_port, discovery_port}` is resolved from the env overrides in Global Constraints. `Run()`:
  - creates `runtime_dir` (0700) and the socket (0600)
  - starts the gRPC server
  - blocks until SIGINT/SIGTERM, then shuts down cleanly and removes the socket
  - if the socket path is held by a live daemon, exits 1 with `zmd is already running`
- `ClientService` implements all ClientApi RPCs except `Team*`, which return `UNIMPLEMENTED` until Task 17.
  - `UploadTree` uses `IgnoreRules::ForRoot` and refuses > 2 GiB (`RESOURCE_EXHAUSTED`, message contains `--force-large`) unless `force_large`.
  - `FetchOutputs` materializes the task's outputs into `dest_path`. It refuses a destination outside an absolute path the client sent, and never writes through symlinks.
  - `ListPeers` returns self (`is_self=true`) and peer-table entries.
- `zm` CLI:
  - `zm run [-o PATH]... [--remote-only|--local-only] [--cores N] [--mem SIZE] [--env NAME] [--force-large] -- argv...`
    1. UploadTree(cwd)
    2. SubmitJob with one task: `capture_changed = (no -o)`
    3. stream `log_chunk`s to the matching stdout/stderr
    4. FetchOutputs into cwd
    5. exit with the task's exit code (or 1 plus a message if `error` is set)
  - `zm status` prints a peer table.
  - `zm monitor` runs the TUI.
  - `zm version`.
  - Global flag `--socket PATH` (default resolved like the daemon).
  - Connection failure prints `zmd is not running (start it with: zmd)` and exits 2.

- [ ] **Step 1: Write the failing integration tests** in `test_local_daemon.py`. `zmtest.py` provides `class Zmd(ctx)`: it starts `build/zmd` with temp `ZM_STATE_DIR`/`ZM_RUNTIME_DIR`/`ZM_CONFIG_DIR`, waits for the socket (≤ 5 s), and offers `.zm(*args, cwd)` → `CompletedProcess` and `.stop()`.
  - `test_run_echo`: `zm run -- echo hi` → stdout `hi\n`, rc 0.
  - `test_no_false_cache_hit`: `zm run -- echo a`, then `zm run -- echo b` → `b\n`.
  - `test_exit_code`: `zm run -- sh -c 'exit 3'` → rc 3.
  - `test_outputs_written_back`: in a temp cwd, `zm run -- sh -c 'echo x > out.txt'` → `cwd/out.txt == "x\n"`.
  - `test_declared_output_only`: `zm run -o a.txt -- sh -c 'echo 1 > a.txt; echo 2 > b.txt'` → `a.txt` exists, `b.txt` doesn't.
  - `test_node_id_persists`: `zm status` shows the self id; restart zmd with the same state dir → same id.
  - `test_cli_without_daemon`: `zm --socket /nonexistent run -- true` → rc 2, stderr contains `zmd is not running (start it with: zmd)`.
  - `test_python_three_task_job` (the M1 acceptance): via generated stubs over `unix:<sock>`, submit a JobSpec where a and b each write a file and c depends on both, using `inputs_from` with `cat`. Watch until done; c's stdout is the concatenation.
  - `test_upload_limit`: monkeypatch via env `ZM_UPLOAD_LIMIT_BYTES=1000` (test-only override, documented in the code) with a 2 KB file → rc ≠ 0 and the message mentions `--force-large`.
  - `test_second_daemon_refused`: start a second zmd on the same runtime dir → exit 1, stderr `zmd is already running`.
- [ ] **Step 2:** `meson test -C build integration_local`. Expected: FAIL.
- [ ] **Step 3:** Implement the daemon, ClientService and CLI.
- [ ] **Step 4:** `meson test -C build --print-errorlogs`. Expected: all PASS.
- [ ] **Step 5:** Mark **M2** `[x]` in PLAN.md (and M1, satisfied by `test_python_three_task_job`).
- [ ] **Step 6:** Commit: `feat: zmd daemon with ClientApi and zm CLI; local execution end to end`.

---

## Milestone M3: Remote execution with security

### Task 12: Team secret, team id and HMAC

**Files:**
- Create: `core/net/team.{hpp,cpp}`, `tests/unit/test_team.cpp`

**Interfaces (produces):**
```cpp
namespace zeromesh::net {
struct TeamKey { std::array<uint8_t,16> secret; };
TeamKey GenerateTeamKey();                                    // randombytes_buf
std::string FormatPhrase(const TeamKey&);                     // "ZM-XXXX-XXXX-XXXX-XXXX-XXXX-XXXX-XXC" Crockford base32 of 128 bits (26 chars) + 1 check char (mod-37 Crockford checksum)
std::optional<TeamKey> ParsePhrase(std::string_view);         // case-insensitive, ignores '-' and spaces; nullopt on bad checksum/length
std::string TeamId(const TeamKey&);                           // hex of first 16 bytes of SHA-256("zm-team-id" || secret)
std::array<uint8_t,32> Mac(const TeamKey&, std::string_view label, std::string_view data);   // HMAC-SHA256 key=secret, msg=label||0x00||data
bool MacEqual(const std::array<uint8_t,32>&, const std::array<uint8_t,32>&);                 // sodium_memcmp
struct TeamFile { static std::optional<TeamKey> Load(const std::filesystem::path& state_dir);
                  static void Save(const std::filesystem::path& state_dir, const TeamKey&);    // <dir>/team, mode 0600
                  static void Remove(const std::filesystem::path& state_dir); };
}
```

- [ ] **Step 1: Write the failing tests:**
  - `TEST(TeamPhraseRoundTrip)`: Format then Parse returns an equal secret.
  - `TEST(TeamPhraseTypoRejected)`: change one char → nullopt.
  - `TEST(TeamPhraseCaseAndSpaces)`: lowercase with spaces parses.
  - `TEST(TeamIdStableAndPublic)`: the same key gives the same 32-hex id, and different keys give different ids.
  - `TEST(TeamMacDiffersByLabel)`.
  - `TEST(TeamFileMode0600)`: after `Save`, `stat` mode `& 0777 == 0600`.
- [ ] **Step 2:** Run. Expected: FAIL. **Step 3:** Implement. **Step 4:** Run. Expected: PASS.
- [ ] **Step 5:** Commit: `feat(net): team secret phrase, team id, HMAC helpers`.

### Task 13: Team-scoped, authenticated discovery

**Files:**
- Modify: `core/net/discovery.{hpp,cpp}`
- Create/modify: `tests/unit/test_discovery.cpp`

**Interfaces (produces):**
- `std::string net::FormatAnnouncement(const AnnounceFields&, const TeamKey&)` and `std::optional<Peer> net::ParseAnnouncement(std::string_view, const TeamKey&, std::string_view self_node_id)`.
- `AnnounceFields{node_id, hostname, node_port, arch, Telemetry}`.
- Wire format: `ZM2\t<team_id>\t<node_id>\t<hostname>\t<node_port>\t<arch>\t<cores_total>\t<cores_free>\t<mem_available>\t<cpu_load_pct>\t<battery_pct>\t<on_ac>\t<state>\t<hex mac>`, where mac = `Mac(key, "zm-announce", everything before the last tab)`.
- Drain: `ZM2D\t<team_id>\t<node_id>\t<reason>\t<hex mac>`.
- `DiscoveryConfig` gains `std::optional<TeamKey> team`, `node_port`, `std::function<Telemetry()> telemetry` and `discovery_port`.
- With no team, the node neither announces nor accepts announcements.
- Sockets use `SO_REUSEADDR`+`SO_REUSEPORT` and `IP_MULTICAST_LOOP=1`.
- The announce interval is 2 s. A peer is stale after 10 s and pruned after 30 s.

- [ ] **Step 1: Write the failing tests:**
  - `TEST(AnnounceRoundTrip)`: Format → Parse gives fields equal to the input.
  - `TEST(AnnounceOtherTeamIgnored)`: parse with a different key → nullopt.
  - `TEST(AnnounceTamperedRejected)`: flip a digit in node_port → nullopt.
  - `TEST(AnnounceSelfIgnored)`: self_node_id equal to the sender → nullopt.
  - `TEST(DiscoveryLoopbackSameTeam)`: two `Discovery` instances on a random port, same key → each sees the other within 5 s.
  - `TEST(DiscoveryLoopbackOtherTeam)`: different keys → neither sees the other after 5 s.
- [ ] **Step 2:** Run. Expected: FAIL. **Step 3:** Implement. **Step 4:** Run. Expected: PASS.
- [ ] **Step 5:** Commit: `feat(net): team-scoped HMAC-authenticated discovery`.

### Task 14: Node identity certs, mTLS with pinning, admission

**Files:**
- Create: `core/net/identity.{hpp,cpp}`, `core/net/tls.{hpp,cpp}`, `core/node/admission.{hpp,cpp}`, `tests/unit/test_admission.cpp`

**Interfaces (produces):**
- `struct net::Identity { std::string cert_pem, key_pem, fingerprint; static Identity LoadOrCreate(const std::filesystem::path& state_dir); }`
  - ECDSA P-256 self-signed, CN = node_id, 10-year validity, via OpenSSL EVP
  - `fingerprint` = hex SHA-256 of the DER cert
  - files `<dir>/node.crt`, `<dir>/node.key` (0600)
- `class net::PinStore { explicit PinStore(std::filesystem::path state_dir); bool IsPinned(std::string_view fp) const; std::optional<std::string> NodeFor(std::string_view fp) const; void Pin(std::string node_id, std::string fp); void Clear(); }`. File `<dir>/peers`, one `node_id fingerprint` per line, 0600.
- `std::shared_ptr<grpc::ServerCredentials> net::ServerCreds(const Identity&)`: TLS with `GRPC_SSL_REQUEST_AND_REQUIRE_CLIENT_CERTIFICATE_BUT_DONT_VERIFY`.
- `std::shared_ptr<grpc::ChannelCredentials> net::ClientCreds(const Identity&, std::function<bool(std::string_view server_fp)> accept)`:
  - `grpc::experimental::TlsChannelCredentialsOptions` with the identity as key/cert
  - `set_verify_server_certs(false)`, `set_check_call_host(false)`
  - an `ExternalCertificateVerifier` that computes the server leaf fingerprint and calls `accept`
- `std::string net::PeerFingerprint(const grpc::ServerContext&)`: from `auth_context()->FindPropertyValues("x509_pem_cert")`.
- Admission (`node::Admission`, used by NodeService and NodeClient):
  1. Client calls `Admit{client_node_id, nonce_c(32B)}` over a channel whose verifier accepts any server fp and records it.
  2. Server responds `{server_node_id, nonce_s, mac_s = Mac(key,"zm-admit-s", nonce_c||fp_server||fp_client)}`. The server reads fp_client from the connection.
  3. Client verifies mac_s using the fp_server it recorded, then sends `AdmitConfirm{mac_c = Mac(key,"zm-admit-c", nonce_s||fp_client||fp_server)}`.
  4. Server verifies and pins (client_node_id, fp_client). Client pins (server_node_id, fp_server).
  5. The server keeps pending admissions keyed by fp_client for 30 s.
- Every NodeApi RPC except `Admit`/`AdmitConfirm` returns `PERMISSION_DENIED` unless `PeerFingerprint` is pinned. After admission, client channels use a verifier that accepts **only** the pinned fp for that node.

- [ ] **Step 1: Write the failing tests.** These run in-process: two `NodeService` instances (Task 15's service class, with only the Admit/Ping RPCs needed here; the stub service grows in Task 15) on 127.0.0.1 random ports, each with its own temp state dir.
  - `TEST(AdmissionSameTeamSucceeds)`: A admits to B → both PinStores contain each other, and A→B `Ping` succeeds.
  - `TEST(AdmissionWrongTeamFails)`: different team keys → admission fails, nothing is pinned, and `Ping` returns `PERMISSION_DENIED`.
  - `TEST(UnpinnedPeerDenied)`: without admission, `Ping` returns `PERMISSION_DENIED`.
  - `TEST(PinnedChannelRejectsImpostor)`: after admission, restart B with a *new* identity on the same port → A's pinned channel fails to connect.
  - `TEST(IdentityPersists)`: LoadOrCreate twice gives the same fingerprint.
- [ ] **Step 2:** Run. Expected: FAIL. **Step 3:** Implement. **Step 4:** Run. Expected: PASS.
- [ ] **Step 5:** Commit: `feat(net): node certificates, pinned mTLS, HMAC admission handshake`.

### Task 15: NodeApi service + client: blobs, offers, remote runs, donor policy

**Files:**
- Create: `core/node/node_service.{hpp,cpp}`, `core/node/node_client.{hpp,cpp}`, `core/node/donor_policy.{hpp,cpp}`, `tests/unit/test_node_api.cpp`, `tests/unit/test_donor_policy.cpp`

**Interfaces (produces):**
- `struct node::DonorPolicy { int allow_on_battery_above{50}; int max_cores{-1 /*= cores_total-1*/}; bool allow_network{false}; int drain_below_battery{20};
  static DonorPolicy Load(const std::filesystem::path& config_dir); /* donor.toml: `key = value` lines, '#' comments; unknown keys warn on stderr */
  std::optional<std::string> Refuses(const net::Telemetry&, const v1::TaskSpec&, int cores_in_use) const; /* nullopt = acceptable, else reason */ }`
- `class node::NodeService final : public v1::NodeApi::Service`. Construction takes the store, Registry, LocalExecutor, PinStore, TeamKey provider, identity, telemetry sampler and donor policy.
  - `OfferTask`: env `Check`, then `DonorPolicy::Refuses`, then `FindMissing(needed)`. On accept, it reserves the slot for 30 s, keyed by task_id.
  - `PushBlobs`/`PullBlobs` stream in 1 MiB chunks and verify digests on put.
  - `RunTask`: runs the reserved spec through LocalExecutor (`ZM_NODE_ID`=donor id) and streams `JobEvent`s, ending with one carrying `result`.
  - `CancelTask`.
  - `Ping` returns `PeerInfo`.
  - `Drain` marks the sender `Draining` in the PeerTable.
- `class node::NodeClient`, one per pinned peer:
  - `Admit(const net::TeamKey&)`
  - `Ping()`
  - `Offer(task_id, spec, needed) → v1::OfferReply`
  - `Push(digests)`, `Pull(digests)`
  - `Run(task_id, std::function<void(const v1::JobEvent&)>) → v1::TaskResult` (blocking, cancellable via `Cancel()`)
  - `Drain(reason)`
  - Every RPC except Run has a 10 s deadline. Run uses keepalive pings every 5 s, with a 10 s timeout.

- [ ] **Step 1: Write the failing tests:**
  - `TEST(DonorPolicyDefaults)`: battery 30% and not on AC → refuses with a reason containing `battery`. On AC → accepts.
  - `TEST(DonorPolicyNetwork)`: a task with `allow_network` → refused by default.
  - `TEST(DonorPolicyParse)`: a file with `allow_on_battery_above = 80` and `# comment` → 80.
  - `TEST(NodeApiRemoteRun)`: two admitted in-process nodes. A uploads a tree containing `in.txt`, offers `sh -c 'cat in.txt; printf %s "$ZM_NODE_ID" > who'` with outputs `{"who"}` → `missing` includes the tree blobs. After Push and Run, the result exit is 0, stdout is in.txt's content, and pulling `who` gives **B's node_id**.
  - `TEST(NodeApiOfferRejectsMissingCommand)`: argv `definitely-not-a-binary-xyz` → `accepted=false`, reason contains `not found on this node`.
  - `TEST(NodeApiBlobDigestVerified)`: pushing bytes whose declared digest doesn't match → `INVALID_ARGUMENT`.
  - `TEST(NodeApiCancel)`: run `sleep 30`, cancel after 200 ms → result `cancelled`/`FAILED` within 2 s.
- [ ] **Step 2:** Run. Expected: FAIL. **Step 3:** Implement. **Step 4:** Run. Expected: PASS.
- [ ] **Step 5:** Commit: `feat(node): NodeApi blob transfer, offers, remote runs, donor policy`.

### Task 16: Placer (local or remote)

**Files:**
- Create: `core/sched/rank.{hpp,cpp}`, `core/sched/placer.{hpp,cpp}`, `tests/unit/test_rank.cpp`, `tests/unit/test_placer.cpp`

**Interfaces (produces):**
- `struct sched::Candidate { std::string node_id; bool is_local; int cores_free; uint64_t mem_available; double cpu_load_pct; bool on_ac; std::string arch; std::chrono::milliseconds latency; int64_t missing_bytes /* -1 unknown */; int in_flight; }`
- `std::vector<Candidate> sched::Rank(const v1::TaskSpec&, std::vector<Candidate>)`:
  - filter: arch must match (empty `resources.arch` means "same as the submitting node"); `cores_free - in_flight*cores >= spec cores` (cores default 1); `mem_available >= mem_bytes`; respect `may_run_local` and `may_run_remote`
  - order (spec §8): missing_bytes ascending (unknown sorts after known), cpu idle descending, on_ac first, latency ascending, then node_id for determinism
- `class sched::Placer final : public node::Executor`. Built from LocalExecutor, the PeerTable, a NodeClient factory, the CasStore and the local telemetry.
  - `Execute`: rank, then try candidates in order.
  - Local → LocalExecutor.
  - Remote: Offer → on reject, try the next candidate → Push missing → Run, forwarding events → Pull the output digests (and stdout/stderr blobs) into the local CAS → `on_done`.
  - If no candidate accepts → `FAILED`, with error `"no node can run this task: <last reason>"`.
  - If a remote run's stream breaks or the keepalive times out → `FAILED`, with error `"node <id> lost during run"`. No retry in Part 1.

- [ ] **Step 1: Write the failing tests:**
  - `TEST(RankPrefersFewerMissingBytes)`.
  - `TEST(RankFiltersArch)`.
  - `TEST(RankRemoteOnlyExcludesLocal)`.
  - `TEST(RankDeterministicTieBreak)`.
  - `TEST(PlacerRemoteOnlyRunsOnPeer)`: in-process A+B admitted; A's JobManager uses a Placer; the task has `may_run_local=false` → `executed_by == B`, and the output is in A's CAS.
  - `TEST(PlacerNoCandidate)`: remote-only with no peers → `FAILED`, error contains `no node can run this task`.
  - `TEST(PlacerPeerLostFailsFast)`: B runs `sleep 60`; shut B's server down mid-run → A's task is `FAILED` with `lost during run` within 15 s.
- [ ] **Step 2:** Run. Expected: FAIL. **Step 3:** Implement. **Step 4:** Run. Expected: PASS.
- [ ] **Step 5:** Commit: `feat(sched): ranking and placer for local/remote execution`.

### Task 17: Wire it up: team commands, discovery, NodeApi in `zmd`; multi-node integration; acceptance

**Files:**
- Modify: `core/node/daemon.cpp`, `core/node/client_service.cpp` (Team RPCs), `cli/main.cpp`
- Create: `cli/team_cmd.{hpp,cpp}`, `tests/integration/test_remote_exec.py`, `tests/acceptance/m3_two_machines.md`

**Behaviour:**
- `Daemon::Run` starts the NodeApi server on `node_port` with `ServerCreds`, and starts Discovery when a team exists. On each newly discovered unpinned peer of the same team it runs admission once, with a 30 s backoff on failure. It uses `Placer` as the JobManager's executor.
- `TeamCreate`: generates a key, saves it, starts discovery and returns the phrase.
- `TeamJoin`: parses the phrase (`INVALID_ARGUMENT` with "invalid team phrase" on a bad checksum), saves it and starts discovery.
- `TeamLeave`: stops discovery, removes the team file and clears pins.
- `zm team create`, `zm team join <phrase>`, `zm team leave`, `zm team info`.
- `zm status` lists peers: id (first 8 chars), host, address, cores free/total, load, battery/AC, state.
- On SIGTERM, `zmd` sends `Drain` to peers with tasks in flight, then exits.

- [ ] **Step 1: Write the failing integration tests** in `test_remote_exec.py`. Three zmd processes on one machine, each with its own state/runtime dirs and `ZM_NODE_PORT` (random free ports) and a shared random `ZM_DISCOVERY_PORT`. A runs `team create`; B joins with A's phrase; C creates its own team.
  - `test_peers_discovered`: within 15 s, A's `zm status` shows B and not C.
  - `test_remote_only_runs_on_b`: `zm run --remote-only -- sh -c 'printf %s "$ZM_NODE_ID" > out.txt'` on A → `cwd/out.txt` equals B's node id.
  - `test_remote_logs_stream`: `zm run --remote-only -- sh -c 'echo remote-hello'` → stdout `remote-hello\n`.
  - `test_other_team_cannot_call`: a Python NodeApi client using C's identity calls `Ping` on B → `PERMISSION_DENIED`.
  - `test_bad_phrase`: `zm team join ZM-0000` → rc ≠ 0, stderr contains `invalid team phrase`.
  - `test_missing_command_remote`: `zm run --remote-only -- definitely-not-a-binary-xyz` → rc ≠ 0, stderr contains `no node can run this task`.
- [ ] **Step 2:** `meson test -C build integration_remote`. Expected: FAIL.
- [ ] **Step 3:** Implement.
- [ ] **Step 4:** `meson test -C build --print-errorlogs`. Expected: all PASS.
- [ ] **Step 5:** Write `tests/acceptance/m3_two_machines.md`: install steps on two Linux machines, `zmd` on both, `zm team create` / `zm team join`, run the spec's M3 command `zm run --remote-only -o out.txt -- sh -c 'hostname > out.txt'`, and confirm out.txt holds machine B's hostname. A third machine not in the team must not appear in `zm status`. Include the firewall note: open 8981/tcp and 8988/udp.
- [ ] **Step 6:** Mark **M3** `[x]` in PLAN.md **only after** the two-machine acceptance has been run by a person. Until then, mark it "automated tests pass; two-machine acceptance pending".
- [ ] **Step 7:** Commit: `feat: team pairing, authenticated discovery and remote execution in zmd (M3)`.

---

## Next plans (not in this document)

- **Part 2 (M4):** `zm run --each`, retries and re-dispatch on drain/loss, multi-task spread, donor drain on battery drop.
- **Part 3 (M5):** Python SDK package, `zm-render`, `env-mirror` (revive `frozen/vfs` with an `IChunkSource` over `NodeApi.PullBlobs`), and the new team task breakdown.
- **Part 4 (M6):** `plugins/frontend-reapi` (revive `frozen/reapi` + linker heuristic) for recc/Bazel.
