# ZeroMesh: team work breakdown (after M3)

- **Date:** 2026-10-26
- **Design:** [`docs/superpowers/specs/2026-10-26-core-plugin-architecture-design.md`](../superpowers/specs/2026-10-26-core-plugin-architecture-design.md)
- **Part 1 plan (M0–M3):** [`docs/superpowers/plans/2026-10-26-core-plugin-part1-m0-m3.md`](../superpowers/plans/2026-10-26-core-plugin-part1-m0-m3.md)
- **Milestone status:** [`PLAN.md`](../../PLAN.md)

This file splits the work after M3 into six streams that can run in parallel. Each stream
has an owner, a set of folders it owns, and the contracts it builds against. Pick a stream,
put your name next to it in the table below (via a PR), and start.

| Stream | Owner | Size |
|---|---|---|
| [M4: fan-out + resilience](#m4-fan-out--resilience-core) | core maintainer | L |
| [M5a: Python SDK](#m5a-python-sdk-sdkpython) | _open_ | M |
| [M5b: zm-render](#m5b-zm-render-appszm-render) | _open_ | M |
| [M5c: env-mirror](#m5c-env-mirror-pluginsenv-mirror) | Shravan (suggested) | L |
| [M6: frontend-reapi](#m6-frontend-reapi-pluginsfrontend-reapi) | in review (`stream/m6-reapi`) | L |
| [Ops: CI, packaging, acceptance, docs](#ops-ci-packaging-acceptance-docs) | _open_ | M |

Sizes are rough: S is a few days, M is one to two weeks, L is two weeks or more for one person.

## How we work

1. One branch per stream, cut from `main` (`stream/m4-fanout`, `stream/m5a-sdk`, ...). Rebase on `main` often.
2. Everything lands through a PR to `main` with CI green. Keep PRs small enough to review in one sitting.
3. Never edit another stream's folders without asking its owner first. Ask in the PR or the team chat.
4. The contracts, `proto/zeromesh/v1/*.proto` and `core/plugin/plugin.hpp`, change only by agreement with the core owner, in their own PR.
5. Tests are required: unit tests for logic, an integration test against real `zmd` processes for anything user-visible.
6. A box in `PLAN.md` is ticked only when that milestone's acceptance test has passed, on real machines where it says so.

**Shared files.** `meson.build` (top level), `Makefile` and `README.md` belong to nobody in
particular. Add only the lines your stream needs (a `subdir()`, a `test()`, a build target),
and expect the core owner to review them.

**Files marked "lands with M3"** below are named in the Part 1 plan (Tasks 15–17) and are
not on `main` yet. Their names may shift slightly when M3 merges; check the tree before you
start.

## Ordering and dependencies

```
               ┌──► M4 (core owner) ──────────────────┐
               │                                      │
 M3 merges ────┼──► M5a SDK ──► M5b zm-render ────────┼──► M5 acceptance (4 machines)
               │                                      │
               ├──► M5c env-mirror ───────────────────┘   (needs a plugin.hpp change first)
               │
               ├──► M6 frontend-reapi                     (needs ClientApiHandle first)
               │
               └──► Ops                                   (can start today)
```

- **Can start right after M3:** M4, M5a, M5c, M6 and Ops.
  - M5a only needs `ClientApi`, which already works locally (M2). It can start before M3 merges.
  - Ops can start today. CI hardening and packaging don't depend on M3.
  - M5c and M6 each need one small contract change from the core owner first (see their
    "contract gaps"). The core owner should land those two changes **before** going deep
    into M4, because they unblock two other people.
- **Waits on the SDK:** M5b (`zm-render`). It needs the SDK's first cut (`connect`,
  `upload`, `map`, `wait`, `fetch`). Agree the SDK's API in the first few days so M5b can
  write code against it while M5a implements it.
- **M5 acceptance** needs M4 (spreading 240 frames over 4 machines, and surviving a lost
  donor), M5a, M5b and M5c. The `PLAN.md` M5 box covers all three M5 streams and is ticked
  once, when the whole M5 acceptance passes.
- **M6** does not depend on the SDK or on M4.

---

## M4: Fan-out + resilience (core)

**Owner:** core maintainer. **Size:** L. **Branch:** `stream/m4-fanout`.

**Goal.** Make one command spread many tasks across the team and finish even when donors
drain or disappear. This covers `zm run --each`, retries with re-dispatch on drain or loss,
sticky drain on battery and user activity, multi-task spread, and donor policy refinements.

**Why it matters for the pitch.** "Pool your idle machines" only means something if 20 jobs
finish in less than half the time on 3 laptops, and still finish when someone closes a lid
halfway through. This is the demo that turns remote execution into a speed-up.

**Owns:**
- `core/sched/` (`placer.*`, `rank.*`, lands with M3)
- `core/node/job_manager.*`, `core/node/donor_policy.*` (lands with M3), `core/node/node_client.*`, `core/node/node_service.*`
- `core/net/telemetry.*` (adding a user-activity signal), `core/net/discovery.*` (drain announcements)
- `cli/run_cmd.*` (`--each`)
- new: `tests/integration/test_fanout.py`, `tests/acceptance/m4_three_machines.md`

**Builds against:**
- C++: `node::Executor` / `node::TaskCallbacks` (`core/node/executor.hpp`), `node::JobManager`,
  `sched::Placer`, `sched::Rank`, `sched::Candidate` and `node::DonorPolicy::Refuses` (M3),
  `net::PeerTable` / `net::PeerState::Draining`, `net::Discovery::BroadcastNodeDraining`,
  `net::TelemetrySampler`.
- Proto: `NodeApi.OfferTask`, `RunTask`, `CancelTask`, `Drain`, `Ping`; `Policy.max_retries`,
  `Policy.preemptible`, `Policy.timeout_s`; `JobEvent` with `TaskState` `QUEUED`/`PLACED`/`RUNNING`.

**Work items:**
- `zm run --each GLOB -- argv…` (spec §7.2): one task per match, `{}` in argv replaced by the
  match path relative to the current directory, one shared `input_root`, one job.
  Each task writes its outputs back. The exit code is non-zero if any task failed, and the
  failures are listed.
- Spread (spec §8): at most `cores_free / task.cores` tasks in flight per node. Track
  in-flight counts in `Candidate::in_flight` so N tasks don't pile onto the top-ranked node.
- Retries: on donor drain, donor loss (missed heartbeats for 10 s or a broken stream) or a
  timeout, re-queue the task to another node, up to `policy.max_retries` (default 3).
  Discard partial outputs. A non-zero exit is a final result and is never retried. Report a
  retry as a `JobEvent` with state `QUEUED` and detail `retry <n>/<max>: <reason>`. A new
  `TaskState` value would be a contract change, so tell the SDK owner first if you want one.
- Sticky drain on the donor: when battery drops below `drain_below_battery` or the user
  becomes active, announce `Draining`, send `NodeApi.Drain` to requesters with tasks in
  flight, and cancel preemptible tasks. Stay drained until the condition has been clear for
  a hold period (for example 5 minutes), so a laptop doesn't flap on and off.
  The user-activity source is your choice (for example logind's `IdleHint`, or input-device
  interrupt counts in `/proc/interrupts`). Document it and make it testable with a fake sysroot,
  the way `TelemetrySampler` already is.
- Donor policy refinements in `donor.toml`: enforce `max_cores` across concurrent tasks, and
  add `drain_on_user_activity` and `drain_hold_s` keys. Unknown keys keep warning on stderr.

**Must not touch:** `sdk/`, `apps/`, `plugins/`. No domain names (gcc, blender, linker) in
`core/`. If a proto change is needed, announce it to the M5a owner before merging, because
the SDK regenerates its stubs from it.

**Acceptance test:**
1. Automated: `meson test -C build integration_fanout --print-errorlogs`. Three `zmd`
   processes on one machine in one team (harness: `tests/integration/zmtest.py`). On A, run
   `zm run --remote-only --each 'cases/*' -- sh -c 'sleep 3; sha256sum {} > {}.out'` over 20
   case files. After about 4 s, SIGKILL one donor's `zmd`. All 20 `.out` files must exist with
   the correct hashes. `zm` must exit 0. At least one task's event log must show
   `retry 1/3`.
2. Automated: unit tests for drain hysteresis (a battery that crosses the threshold and
   recovers within the hold period stays drained) and for the per-node in-flight cap.
3. Manual, three machines (`tests/acceptance/m4_three_machines.md`): the spec's M4 command,
   `zm run --each 'cases/*' -- ./sim {}`, with 20 CPU-bound cases of about 30 s each, finishes
   in under 45% of the single-machine time. Kill one donor's `zmd` mid-job and confirm all 20
   finish correctly. Record the times and hostnames in the file.

**Suggested skills:** C++20 concurrency, gRPC C++ streaming and deadlines, Linux
procfs/sysfs, failure testing.

---

## M5a: Python SDK (`sdk/python`)

**Owner:** _open_. **Size:** M. **Branch:** `stream/m5a-sdk`.

**Goal.** Ship a `zeromesh` Python package that lets anyone write an app in a few lines:
connect, upload, submit or map, wait, fetch. It talks to the local `zmd` over `ClientApi`
only.

**Why it matters for the pitch.** The pitch is "write the domain part, we did the
plumbing". The SDK is how a third party proves that: a render farm or a parameter sweep
should be a short script, not a fork of the daemon.

**Owns:**
- new: `sdk/python/` (`pyproject.toml`, `zeromesh/`, `tests/`, `examples/`)
- one `test('sdk_python', …)` entry in the top-level `meson.build`

**Builds against (proto only):**
- `ClientApi.UploadTree`, `PutObjects`, `GetObjects`, `FindMissing`, `SubmitJob`, `WatchJob`,
  `GetJob`, `CancelJob`, `FetchOutputs`, `ListPeers`, `TeamInfo`.
- Messages: `JobSpec`, `TaskSpec`, `InputFrom`, `EnvSpec`, `Resources`, `Policy`, `JobEvent`,
  `JobStatus`, `TaskResult`, `PeerInfo` (`proto/zeromesh/v1/common.proto`, `client.proto`).
- Socket path, the same rules as `node::DaemonConfig::FromEnv`: `$ZM_RUNTIME_DIR/zmd.sock`,
  else `$XDG_RUNTIME_DIR/zeromesh/zmd.sock`, else `/tmp/zeromesh-<uid>/zmd.sock`.

**Work items:**
- Generate the Python stubs from `proto/zeromesh/v1/` into `zeromesh/v1/` inside the package
  at build/install time with `grpc_tools.protoc` (the same command as
  `proto/zeromesh/v1/meson.build`). The generated modules import as `zeromesh.v1.*_pb2`, so they
  must live inside this package, not next to it. `proto/` stays the only source of truth.
- A small API, roughly as below. Agree the names with the M5b owner in the first days,
  then keep them stable:
  ```python
  import zeromesh
  zm = zeromesh.connect()                      # raises zeromesh.DaemonNotRunning
  root = zm.upload("scene/")                   # UploadTree, returns a Digest
  job = zm.map(["blender", "-b", "shot.blend", "-f", "{}"], items=range(1, 241),
               input_root=root, outputs=["out/"], env="host", cores=1)
  for ev in job.events(): ...                  # WatchJob
  job.wait()                                   # raises on failed tasks unless check=False
  job.fetch(task, "renders/")                  # FetchOutputs
  zm.peers()                                   # ListPeers
  ```
  Also a lower-level `zm.submit(JobSpec)` for DAGs (`depends_on`, `inputs_from`).
- Errors: `DaemonNotRunning` (same message as `zm`: `zmd is not running (start it with: zmd)`),
  `TaskFailed` with exit code and stderr head, and cancellation on `KeyboardInterrupt`.
- Type hints and docstrings. A one-page `sdk/python/README.md` with a runnable example.

**Must not touch:** `core/`, `cli/`, `plugins/`, `apps/`. Never import `NodeApi` or read the
daemon's state directory. If `ClientApi` is missing something, ask the core owner for a
proto change.

**Acceptance test:**
1. `pip install ./sdk/python` in a fresh venv, then `python -c "import zeromesh"` works.
2. `meson test -C build sdk_python --print-errorlogs` starts a real `zmd` with
   `tests/integration/zmtest.py` and runs `sdk/python/tests/`, which must cover:
   - upload a directory of 3 files, `map` `sh -c 'wc -c < {} > {}.len'` over them, `wait`,
     `fetch` and check the 3 results;
   - a task that exits 3 raises `TaskFailed` with `exit_code == 3`;
   - `cancel()` on a `sleep 30` task finishes within 2 s;
   - with no `zmd` running, `connect()` raises `DaemonNotRunning`.
3. The same tests pass against a 2-node team (A submits with `may_run_local=False`), once M3
   has merged.

**Suggested skills:** Python packaging (`pyproject.toml`), `grpcio`, API design, pytest.

---

## M5b: zm-render (`apps/zm-render`)

**Owner:** _open_. **Size:** M. **Branch:** `stream/m5b-render`.

**Goal.** Build `zm-render`, a Blender frame-rendering app on the Python SDK. It splits a
frame range into tasks, renders them across the team and collects the frames into one
directory. It is the flagship demo.

**Why it matters for the pitch.** This is the demo people remember: one command, a
240-frame animation renders on four machines, and the output matches a local render. It also
proves the SDK is enough to build a real app without touching the core.

**Owns:** new: `apps/zm-render/` (package, `zm-render` console script, `tests/`, a small
test scene or a script that generates one with `blender -b --python`), and
`tests/acceptance/m5_render.md`.

**Builds against:** the `zeromesh` SDK only (`connect`, `upload`, `map`, `events`, `wait`,
`fetch`, `peers`). Env names `host` (the donor has Blender installed) and `mirror` (M5c).
Blender's command line (`-b`, `-o`, `-F`, `-s`/`-e`/`-a` or `-f`).

**Work items:**
- `zm-render SCENE.blend --frames 1-240 [--chunk N] [-o DIR] [--env host|mirror]`.
- Upload the scene's directory. Warn when the `.blend` references absolute paths outside it
  (those won't exist on a donor except in mirror mode). Suggest packing or relative paths.
- One task per chunk of frames, for example
  `blender -b shot.blend -o //out/frame_#### -F PNG -s A -e B -a`, with declared output `out/`.
- Progress: frames done / total and which node rendered each chunk (`TaskResult.executed_by`).
- Merge: fetch every chunk into `DIR`, check every frame exists, and exit non-zero with the
  missing frame numbers if any are missing. Optional `--encode out.mp4` runs `ffmpeg` locally.

**Must not touch:** `core/`, `plugins/`, `sdk/` (ask the M5a owner for SDK changes), and no
direct gRPC calls that bypass the SDK.

**Acceptance test:**
1. CI, no Blender needed: `meson test -C build app_zm_render`, or `pytest apps/zm-render/tests`,
   runs `zm-render` against a local `zmd` with a fake `blender` script in the uploaded
   directory that writes one PNG per frame. It checks the split (chunk boundaries), that all
   frames are collected, and that a missing frame fails loudly.
2. Local, with Blender installed: `zm-render apps/zm-render/tests/data/cube.blend --frames 1-8`
   produces the same frames as `blender -b cube.blend -o //ref/frame_#### -F PNG -s 1 -e 8 -a`.
   Compare decoded pixels, not file bytes. Use Cycles on CPU with a fixed seed and the same
   Blender version on every node.
3. Manual, the M5 acceptance (`tests/acceptance/m5_render.md`):
   `zm-render shot.blend --frames 1-240` on 4 machines produces the same frames as a local
   render, and with `--env mirror` a donor without Blender renders frames.

**Suggested skills:** Python, Blender command line and Python API, image comparison
(Pillow or OpenImageIO), CLI UX.

---

## M5c: env-mirror (`plugins/env-mirror`)

**Owner:** Shravan (suggested; filesystem background). **Size:** L. **Branch:** `stream/m5c-mirror`.

**Goal.** Revive the FUSE code in `frozen/vfs` as an `IEnvProvider` named `mirror`. It shows
the requester's filesystem to the task read-only, with secrets hidden by a deny list, and
fetches file chunks on demand from the requester via `NodeApi.PullBlobs`.

**Why it matters for the pitch.** "Zero setup" has to include software. With mirror mode a
donor doesn't need Blender, a compiler or a Python environment installed. It uses the
requester's, which is the difference between "install the same stack on every laptop" and
"just join the team".

**Owns:**
- new: `plugins/env-mirror/` (code moved from `frozen/vfs/fuse_fs.*` and `frozen/vfs/chunk_fetcher.*`,
  plus `entry.cpp` exporting `zm_plugin_entry()`, as in `plugins/env-host/entry.cpp`)
- new: `tests/unit/test_env_mirror.cpp`, `tests/integration/test_mirror.py`
- removing `vfs/` from `frozen/` and updating its row in `frozen/README.md`

**Builds against:**
- C++: `plugin::IEnvProvider` (`Name()` = `"mirror"`, `Check`, `Prepare`, `Release`),
  `plugin::SandboxEnv` / `plugin::Bind` / `plugin::Symlink`, `plugin::TaskContext`,
  `plugin::PluginEntry`, `plugin::Registry::LoadFile` (`core/plugin/`).
  `store::CasStore` and `store::MerkleBuilder` for manifests and the local chunk cache.
- Proto: `NodeApi.PullBlobs(Digests) → stream Chunk`; `EnvSpec{name: "mirror", params}`.
  `zm run --env mirror` already parses (`cli::RunOptions::env`).
- The `IChunkSource` named in the spec does not exist yet. Define it inside the plugin; it
  replaces `dht::KademliaDht` in `chunk_fetcher.hpp` and `fuse_fs.hpp`.

**Contract gaps (agree with the core owner before coding):**
1. `plugin::TaskContext` has no requester node id and no way to fetch blobs from it. The
   plugin needs one, for example a `requester_node_id` field plus a small blob-source
   interface the core hands to `Prepare`, backed by the core's `NodeClient`. This is a
   `core/plugin/plugin.hpp` change, so the core owner makes it.
2. How the requester publishes what it exports. The suggested approach needs no proto
   change: the requester builds a Merkle manifest of the exported roots with `MerkleBuilder`,
   leaving out denied paths, and passes the root digest in `EnvSpec.params`. The donor
   mounts that manifest with FUSE and pulls file blobs lazily by digest. Hashing all of `/usr`
   is slow, so cache manifests keyed by path, size and mtime. The alternative, a path-based
   read RPC on `NodeApi`, is a proto change. Write the choice down in the first PR.

**Work items:**
- Remove the DHT dependency from the revived code. Keep the LRU disk chunk cache.
- `Prepare` mounts the FUSE tree under the task's scratch dir and returns read-only `Bind`s
  for it (for example the requester's `/usr`, `/opt` and the job's paths under `$HOME`).
  `Release` unmounts it, including after crashes and cancels. No stale mounts.
- `Check` returns `ok=false` with a clear reason when FUSE is unavailable or the requester
  is not reachable.
- Read-only: every write op returns `EROFS`, and binds are `read_only=true`.
- Deny list, enforced on the **requester** side (denied paths never enter the manifest, so
  the donor never sees them): `~/.ssh`, `~/.gnupg`, `~/.config/zeromesh`, keyrings
  (`~/.local/share/keyrings`), `.env*` anywhere, and browser profiles (`~/.mozilla`,
  `~/.config/google-chrome`, `~/.config/chromium`). Users extend it in a file under
  `~/.config/zeromesh/` (one glob per line).

**Must not touch:** `frozen/dht/` (it stays frozen), `core/` (contract changes go through
the core owner), `NodeApi` server code, other plugins.

**Acceptance test:**
1. Unit (`meson test -C build all_unit_tests`): a manifest built from a fake home with
   `.ssh/id_ed25519`, `project/.env` and `project/main.c` contains only `main.c`. A write
   through the mount fails with `EROFS`. `Release` leaves no mount behind (check `/proc/self/mounts`).
2. Integration (`meson test -C build integration_mirror`): two `zmd` processes in one team.
   A creates a script at a path that exists only on A (for example `$TMP/a-only/bin/hello`)
   and runs `zm run --remote-only --env mirror -o out.txt -- $TMP/a-only/bin/hello`. `out.txt`
   is produced, `executed_by` is B, and B never had that file on its own disk.
3. Manual, part of the M5 acceptance: a donor without Blender renders frames for
   `zm-render --env mirror`.

**Suggested skills:** FUSE 3 (libfuse low-level API), Linux VFS and mount namespaces,
bubblewrap, C++20, careful security thinking about path handling and symlinks.

---

## M6: frontend-reapi (`plugins/frontend-reapi`)

**Owner:** in review. **Size:** L. **Branch:** `stream/m6-reapi`.

**Goal.** Build a REAPI v2 front-end as an `IFrontend` plugin, so `recc`, Bazel and CMake
builds can send compile actions to the team through `zmd`. Revive what is useful in
`frozen/reapi`, and move the linker heuristic from `frozen/scheduler` into the plugin.

**Why it matters for the pitch.** It shows the core is general: the old "compile mesh" idea
becomes one plugin on top of the same plumbing instead of the centre of the project. C++
developers can then use ZeroMesh without changing their build.

**Owns:**
- new: `plugins/frontend-reapi/` (services revived from `frozen/reapi/*`, and the
  `IsCriticalPath` logic from `frozen/scheduler/heuristics.*`)
- moving `proto/remote_execution.proto` into `plugins/frontend-reapi/proto/`, with the Google
  protos it imports
- new: `tests/integration/test_reapi.py`, `tests/data/reapi-hello/` (a small CMake project)
- removing `reapi/` and `scheduler/` from `frozen/` and updating `frozen/README.md`

**Builds against:**
- C++: `plugin::IFrontend` (`Name`, `Start(ClientApiHandle&, const PluginConfig&)`, `Stop`),
  `plugin::PluginEntry::make_frontend`, `plugin::PluginConfig`.
- Proto: `ClientApi` (`FindMissing`, `PutObjects`, `GetObjects`, `SubmitJob`, `WatchJob`,
  `GetJob`) and `Policy.may_run_remote`.
- REAPI v2 services served on `127.0.0.1:8980`: `Capabilities.GetCapabilities`,
  `ContentAddressableStorage.FindMissingBlobs` / `BatchUpdateBlobs` / `BatchReadBlobs` / `GetTree`,
  `ByteStream.Read` / `Write`, `ActionCache.GetActionResult` / `UpdateActionResult`,
  `Execution.Execute` / `WaitExecution`.

**Contract (done on this branch):** `plugin::ClientApiHandle` is defined in
`core/plugin/client_api_handle.hpp` (unix socket + `ClientApi` stub). `Registry::Frontends()`
exposes loaded front-ends; `zmd` calls `Start`/`Stop` after the ClientApi server is up.

**Work items:**
- [x] Map a REAPI `Action` + `Command` to a `TaskSpec`. File blob digests are the same (SHA-256 of
  the bytes), but REAPI `Directory` messages and ZeroMesh Merkle manifests are different
  formats. Translate the input root, and translate outputs back into an `ActionResult`.
- [x] Pin link and archive actions locally: when `IsCriticalPath(argv)` is true, set
  `may_run_remote=false`. This heuristic lives only in the plugin, never in `core/sched`.
- [x] Disabled by default. Enabled by `ZM_ENABLE_REAPI=1` / PluginConfig, listening on loopback only.
- [x] Update `scripts/install_recc.sh` with apt, dnf, brew and Arch package-manager guidance.
- [x] Remove the revived `frozen/reapi/` and `frozen/scheduler/` directories and document
  their migration in `frozen/README.md`.

**Must not touch:** `core/sched/` and the rest of `core/` (no compiler logic in core),
`proto/zeromesh/v1/` without agreement, other plugins.

**Acceptance test:**
1. [ ] Automated integration (`meson test -C build integration_reapi`, skipped with a clear message when
   `recc` isn't installed): two `zmd` processes in one team, with the plugin loaded on A.
   Build `tests/data/reapi-hello` (4 or more `.cpp` files) with
   `RECC_SERVER=127.0.0.1:8980 cmake -DCMAKE_CXX_COMPILER_LAUNCHER=recc … && cmake --build`.
   The CMake binary runs, at least one compile action has `executed_by` = B, and a direct link
   action submitted through `recc` is pinned to A. `RECC_LINK` is scoped to that action because
   this `recc` release leaves CMake's normal final link local.
2. [x] Unit tests cover compiler-driver links, linker aliases, archives, and compile-only actions.
3. [ ] Two-node manual acceptance is covered once `integration_reapi` is green with `recc` installed.

**Suggested skills:** REAPI v2 and the Bazel remote execution model, gRPC C++ servers,
protobuf, CMake and recc.

---

## Ops: CI, packaging, acceptance, docs

**Owner:** _open_. **Size:** M. **Branch:** `stream/ops`.

**Goal.** Make ZeroMesh easy to install and hard to break: stricter CI, packages with a
systemd user unit, and the multi-machine acceptance runs done and recorded. Keep the public
docs in line with what actually works.

**Why it matters for the pitch.** "Install one daemon, join a team, done" is a claim about
packaging as much as code. A demo that needs a 10-step build from source isn't zero setup.
Recorded acceptance runs are also our evidence that the milestones are real.

**Owns:**
- `.github/workflows/`
- new: `packaging/` (`systemd/zmd.service` user unit, `arch/PKGBUILD`, `debian/`)
- new: `tests/acceptance/README.md` (how to run the manual acceptance scripts and record
  results). Each stream writes its own `tests/acceptance/m*_*.md`. Ops runs them with a person
  and records the date, machines and outcome.
- install-related lines in `meson.build` and `Makefile` (reviewed by the core owner)
- a docs site, if we want one (for example GitHub Pages built from `docs/`)

**Builds against:** the `make build` / `make test` targets, `meson install` (`zm` and `zmd`
are already `install: true`), and the paths and environment overrides in
`node::DaemonConfig::FromEnv` (`ZM_STATE_DIR`, `ZM_RUNTIME_DIR`, `ZM_CONFIG_DIR`,
`ZM_NODE_PORT`, `ZM_DISCOVERY_PORT`).

**Work items:**
- CI: a GCC and Clang matrix; an ASan + UBSan job (`meson setup -Db_sanitize=address,undefined`);
  an Arch container job next to Ubuntu; a job for `sdk/python` once it exists; confirm the
  integration tests' loopback multicast works on GitHub runners, and fix or document it if not.
- systemd user unit: `systemctl --user enable --now zmd`, `Restart=on-failure`. Document
  `loginctl enable-linger` for donating while logged out.
- Packages: an AUR `PKGBUILD` and a `.deb` that install `zm`, `zmd` and the user unit. The
  firewall note (8981/tcp and 8988/udp) goes in the package docs.
- Acceptance runs: M3 on two machines first (`tests/acceptance/m3_two_machines.md`, lands
  with M3), then M4, M5 and M6 as their streams finish.
- M0's `PLAN.md` box is still waiting for CI on GitHub. Get it green and tell the core owner.

**Must not touch:** code under `core/`, `cli/`, `zmd/`, `plugins/`, `sdk/` or `apps/`. If
packaging needs a code change (for example a `--version` flag on `zmd`), ask the owning
stream.

**Acceptance test:**
1. A PR to `main` runs the full matrix (GCC, Clang, sanitizers, Ubuntu, Arch) and it passes.
2. On a clean Arch machine or VM: `makepkg -si` in `packaging/arch/`, then
   `systemctl --user start zmd && zm status` shows this node. The same on Ubuntu 24.04 with
   `sudo dpkg -i zeromesh_*.deb`.
3. `tests/acceptance/m3_two_machines.md` has a filled-in results section (date, both
   hostnames, the `out.txt` content, and the third-machine check) from a real two-machine run.

**Suggested skills:** GitHub Actions, Meson, systemd user services, Arch and Debian
packaging, technical writing.
