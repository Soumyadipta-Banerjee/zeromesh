# ZeroMesh Adapter Platform — Design

- **Date:** 2026-11-02
- **Status:** Draft for team review
- **Extends:** [`architecture.md`](architecture.md). The Task/Job/Object model, the security model, the
  plugin ABI and the v2 blueprint mechanisms all stay. This document adds the layer above them that
  adapter developers use, and changes what the SDK is.
- **Supersedes:** the adapter-side plumbing in `apps/zm-render`, `apps/zm-cfd` and `apps/zm-cc`, and the
  "tree versus distcc" split in `zm-cc`.

---

## 1. Intent

Adapters are the product. ZeroMesh's own value is the plumbing, and the plumbing is only worth
anything if a person who knows one tool (Blender, OpenFOAM, gcc, ffmpeg, pytest, a Monte Carlo script)
can put it on the pool without becoming a distributed-systems engineer.

The test for every decision in this document is one question:

> **How much of my adapter is my domain, and how much is your plumbing?**

[`architecture.md`](architecture.md) §1 already states the goal: an app writes only *how to split work,
what to run per piece, and how to merge results*. The code does not deliver it. The ClientApi's unit is
the **Task** (one sandbox run), while an adapter's unit is a **frame, a translation unit, a case**.
Every adapter bridges that gap by hand, and that is where their size and difficulty come from.

### 1.1 What adapters rebuild today

Measured on the three adapters in `apps/` (branch `zm_perf_cc`, 2026-11-02):

| Plumbing written by hand | zm-render (744 lines) | zm-cfd (267) | zm-cc (2 350) |
|---|---|---|---|
| Split work into tasks, guess the pool size | `split_chunks`, `--chunk`, `--cores 8` | one task per case | `make_tasks`, `_wave_task_count` (hard-coded "2 nodes × 2 slots") |
| Build protos | `build_job_spec` | `build_job_spec` | `build_job_spec` |
| Event loop: started/done sets, node id → hostname, "who did what" | ~110 lines | ~20 | `_watch`, `_collect`, `peer_names`, `who` |
| Verify and place outputs | `_flatten_frames`, `collect_frames` | `job.fetch` per case | `_find`, `_move`, `_fresh` |
| Explain failures from `stderr_head`/`stdout_head` | parses Blender stdout for `Error` | none | `task_failure`, `explain` |
| Memory per task | none (the memory gate cannot help it) | none | `_MEM_BASE`, `_MEM_PER_INPUT_BYTE` |
| Decide what to upload | whole scene directory | whole cases directory | `_SKIP_TREE_DIRS`, `_SKIP_TREE_SUFFIXES` |
| Build the donor environment | `--mirror PATH` by hand | host only | `mkroot.py` (279 lines) |
| Retry locally | `may_run_local` | `may_run_local` | `retry_failed_local` |
| The same CLI flags | `--env --mirror --cores --remote-only --socket` | same | same + `--keep-going --dry-run -v` |

Roughly 560 of zm-render's 744 lines and 1 400 of zm-cc's 2 350 are this plumbing. The domain parts
(Blender argv, `.blend` path scanning, frame naming, ffmpeg; flag classification, depfiles, linking) are
small and readable on their own.

Core features that **no adapter can reach**: partial commit (`commit_unit`, `UnitCommitted`: zero
callers), tail hedging and peer seeding (internal to the placer), task DAGs (`depends_on`,
`inputs_from`), `.zmignore` (only `zm run` honours it), and env-mirror's access recorder (used for
prefetch only).

So the core is a good task executor and a thin adapter platform. The fix is not a list of features; it
is to **move the API boundary up to the item and make the core own everything below it.**

## 2. Who the adapter developer is

Someone who knows one domain tool and wants it on the LAN pool. They expect:

- to install one package and have the first run work on one laptop with no mesh;
- to write *split, run, merge* and nothing else;
- the platform to handle environments, data movement, placement, retries, churn, caching, progress,
  reports and failure explanation, with sensible defaults when they say nothing;
- to test their adapter without a mesh, and ship it with `pip install`;
- to promise their own users what the sandbox guarantees without reading core code.

Appendix A lists every expectation we found, by stage of that journey, with its status today.

## 3. Model and names

| Term | Meaning | Replaces |
|---|---|---|
| **Workspace** | A content-addressed snapshot of the adapter's input directory, taken **incrementally** (a stat cache: files whose size and mtime are unchanged are not rehashed). Honours `.zmignore` and a built-in deny list. May include **SharedPath** entries: paths that every node already has (NFS) and that are never uploaded. | `UploadTree` + per-adapter skip lists |
| **Environment** | A content-addressed tree plus a mount recipe and a toolchain identity. Three sources: *host* (the donor's own install), *captured* (`zm env capture -- CMD`, built once on the requester, fetched by digest), *image* (an OCI image imported into the store; later). Mounted read-only. | `--env host` byte-identity prayer, `mkroot.py`, `--mirror PATH` |
| **Requirement** | What an item needs: programs with version constraints, an environment digest, GPU, architecture, libc. | toolchain digest only |
| **Capability** | What a donor advertises: programs and versions on its PATH, environment digests it holds, GPU, arch, libc. Matched against Requirements at Offer time. | `argv[0]` resolves on PATH |
| **Item** | The domain unit: `id`, `cwd`, `argv`, `env`, inputs subset, declared outputs with destinations, `kind`, cost hint, priority, `after`, footprint source. | the adapter's chunk |
| **Stage** | Items that share requirements and resources (GPU render, then CPU composite). | — |
| **Run** | A set of items from one workspace with one policy. Owned by the daemon, persisted in a run journal, survives the adapter process. | `Job` at the adapter layer |
| **Session** | A live Run that accepts items while open and yields results as they finish. Dynamic adapters (search, adaptive sweeps) and streaming submission use it. `attach(id)` reconnects. | several Jobs submitted in waves |
| **Batch** | A core-formed group of items that share one sandbox invocation. Sized from live slots and learned cost. Visible only in the report. | `--chunk` |
| **Attempt** | One execution of a batch on one node. Retries and hedges are attempts. | implicit |
| **Footprint** | The files an item actually read. Sources, in order of precision: a tool depfile the item declares, the env-mirror recorder, the batch runner's trace (opt-in). Absent any source, the key degrades to the whole workspace and the report says so. | nothing |
| **Result** | Per item: outputs, exit, log tails, node, wall and CPU time, peak memory, bytes in and out, cache status, a structured failure kind. | `TaskResult` + adapter parsing |
| **Report** | One schema for every adapter: items, attempts, nodes, bytes, cache hits and misses with reasons, failures, timeline. | per-adapter prints and JSON |
| **Warm dir** | A directory on the donor that persists between items of one (environment, kind). Opt-in per kind, because it breaks hermeticity. | — |

**Task** and **Job** remain the low-level contract, unchanged. `zm run`, the REAPI front-end and the Run
engine itself use them. Adapter developers never see them.

## 4. Layers

```
 ┌─ Adapters (out of process, any language) ──────────────────────────────┐
 │  Python SDK: Adapter base · FakeMesh · shared CLI · standard report      │
 │  run.json manifest for adapters in other languages                       │
 └───────────────────────────────┬─────────────────────────────────────────┘
                                 │  RunApi: Snapshot · Env · Run/Session · Watch · Report · Doctor
 ┌─ Run engine (new, inside zmd) ─▼────────────────────────────────────────┐
 │  items → batches (cost model, live slots)     per-item commit and retry │
 │  output verification and placement            item-level events         │
 │  structured failures                          resource learning by kind │
 │  footprint cache, team-wide                   requirement ↔ capability  │
 │  sessions, run journal, fair-share                                      │
 └───────────────────────────────┬─────────────────────────────────────────┘
                                 │  Job / Task (existing ClientApi contract, unchanged)
 ┌─ Task core (existing) ────────▼─────────────────────────────────────────┐
 │  placer · hedging · seeding · hazard · comfort governor · freeze-first  │
 │  action cache · sandbox runner · env providers (plugins)                │
 └───────────────────────────────┬─────────────────────────────────────────┘
 ┌─ Substrate (existing) ────────▼─────────────────────────────────────────┐
 │  CAS/Merkle · bwrap/cgroups · mTLS NodeApi · discovery · team           │
 └─────────────────────────────────────────────────────────────────────────┘
```

The Run engine is a layer **on top of** Job/Task, not a rewrite. Everything built for v2 (hedging,
seeding, hazard model, governor, freeze-first) keeps working at batch granularity; partial commit gives
item granularity inside a batch. That is what makes this additive.

## 5. The adapter contract

### 5.1 What an adapter writes (Python)

```python
from zeromesh.adapter import Adapter, Item, Out, Env, Res, Policy, main

class MyTool(Adapter):
    name = "zm-mytool"
    kind = "mytool"                       # opaque label; resource learning and warm dirs key on it
    env  = Env.host("mytool", version=">=2")   # or Env.capture([...]) / Env.ref("sha256:...")
    res  = Res(cores=1, mem="512M")       # starting point only; the team learns real numbers
    policy = Policy()                     # retries, timeout, keep_going, local_fallback, priority

    def add_args(self, p): ...            # domain flags only; shared flags are added for you
    def check(self, a): ...               # optional: fail early (external paths, missing files)
    def workspace(self, a): ...           # a directory, or Workspace(dir, shared=[...], extra=[...])
    def items(self, a): ...               # yield Item(...)
    def combine(self, a, items): ...      # optional: one process for several contiguous items
    def finish(self, a, run): ...         # optional: local merge step with every result in hand

main(MyTool)
```

`Item` fields: `id` (stable across runs), `cwd`, `argv`, `env`, `inputs` (subset of the workspace;
default: all), `outputs: [Out(path, dest=..., required=True)]` or `Out.changed(dest=...)`, `kind`,
`cost` (relative hint), `priority`, `after: [item ids]`, `footprint: Depfile("{out}.d") | Recorded |
Trace | None`, `stage`.

### 5.2 Adapters in other languages

A `run.json` manifest with the same fields (`workspace`, `env`, `items`, `policy`) driven by
`zm run --from run.json`. A shell script, a Rust tool or a Makefile is an adapter without the SDK.
The gRPC `RunApi` is public for anyone who wants a native client.

### 5.3 `combine()`

Some tools pay a large startup cost per process (Blender loads the scene; a solver reads the mesh;
an inference script loads a model). `combine(items)` lets the adapter say "these contiguous items can
be one process". The batch runner still commits **per item** by watching each item's declared output
files, so per-item retry, hedging, streaming and caching are kept. Adapters without this hook get one
process per item.

## 6. The in-sandbox contract

The same on every node, so an adapter can document it to its users:

```
/work        the workspace snapshot, read-only; declared output paths are writable
/env         the environment, read-only (absent for env host)
/scratch     writable, discarded after the attempt
/warm        writable, persists per (environment, kind); only when the kind opts in
/zm/run      the batch runner (a static binary shipped with zmd)

ZM_ITEM_ID   ZM_CORES   ZM_MEM_BYTES   ZM_GPU   ZM_UNITS_DIR   ZM_CHECKPOINT_DIR
```

**Preemption:** a donor leaving may freeze the cgroup (invisible to the item) or send `SIGTERM`, wait a
grace period, then `SIGKILL`. A checkpoint written to `ZM_CHECKPOINT_DIR` before exit is restored on the
next attempt.

**The batch runner** reads the batch manifest, runs items in order (or the `combine()` command),
watches declared output paths with inotify and commits each item's outputs atomically to the units
directory the moment the file is closed, records the footprint when asked, and writes per-item exit
code, wall time, CPU time and peak RSS (`wait4` rusage). It must be a static binary: it runs inside any
environment, including captured ones with a different libc.

**Inputs never share writable inodes with the store.** Materialization uses read-only bind mounts or
copy-on-write clones; hard links into the CAS from a writable tree (the current `merkle.cpp` path) are
not allowed.

## 7. How a run executes

1. **Check.** `check()` runs; the daemon runs its doctor checks (bwrap version, AppArmor userns,
   multicast, team joined) and fails fast with the fix.
2. **Snapshot.** Only changed files are rehashed. `--show-upload` lists what goes and how big.
3. **Resolve the environment.** The Requirement is matched against each donor's Capability. A host
   environment with a version constraint excludes donors that lack it; a captured environment is
   fetched by digest and counted as missing bytes in placement. The report lists excluded donors and
   why.
4. **Cache, per item, team-wide.** Key = argv + environment identity + footprint hashes + the name
   listings of searched directories (so a new file that shadows an old one invalidates). Hits are
   placed to their destinations at once; no sandbox runs.
5. **Batch.** Remaining items are packed by learned cost against live slots, longest first. Items with
   different `cwd`, `stage` or unmet `after` never share a batch.
6. **Run.** The batch runner executes; each item's outputs are committed as they appear.
7. **Stream.** Each committed item is pulled, verified against its `Out` declarations, written to
   `dest`, and an item event is emitted. Outputs appear on the requester while the run continues.
8. **Churn.** A donor leaving loses at most the item in flight. The retry batch holds only unfinished
   items. Tail hedging copies the unfinished tail, not the whole batch.
9. **Failure.** Every failed item carries a kind (§8) and log tails. Policy decides fail-fast or
   keep-going, exit retries, local fallback.
10. **Finish.** `finish()` runs locally with every result in hand. The report is written and the run
    is kept in history.

## 8. Promises to adapters

What an adapter author may rely on and repeat to their users:

- An item's outputs, once committed, are never lost or recomputed in this run.
- An unchanged item in an unchanged environment is a cache hit anywhere on the team.
- A donor without the software can still run the item when the environment is captured.
- A failing item is reported with a kind: `exit`, `missing-output`, `oom`, `timeout`, `hung`,
  `env-unavailable`, `toolchain-mismatch`, `sandbox`, `donor-left`, `cancelled`. Adapters never parse
  stderr to learn what happened.
- Ctrl-C keeps finished items; `--resume ID` continues a run.
- The same adapter runs unchanged against `FakeMesh` in tests, with simulated nodes, departures, OOM
  kills, cache state and latency.
- Items run sandboxed, with no network, at low priority, and the donor's policy wins. Environments are
  data, never code the donor must trust.

## 9. Defaults: when the adapter says nothing

- **Workspace:** the directory given; `.zmignore` plus a built-in deny list (`.git`, `.env*`, `.ssh`,
  `.gnupg`, keyrings, browser profiles); warn above 500 MB, refuse above 2 GB without `--force-large`.
- **Environment:** host, with a requirement inferred from `argv[0]` (present on PATH; version-checked
  only when declared).
- **Resources:** learned per kind. Before any history: 1 core, memory from the last similar item on
  the team or 512 MB, 1 h timeout, no GPU, no network.
- **Batching:** automatic by cost against live slots.
- **Policy:** 3 infra retries, 0 exit retries, fail-fast, local fallback on, hedge the tail,
  freeze-first on departure.
- **Outputs:** what changed under `cwd`, placed in `zm-out/<item>/` beside the workspace.
- **Observability:** the standard progress line, item view in `zm monitor`, `zm-out/report.json`,
  run kept 30 days.
- **Cache:** on, team-wide, footprint-keyed when a source exists, whole-workspace otherwise (and the
  report says which).

## 10. Daemon services and API additions

All additions are new messages and a new service; `ClientApi` and `NodeApi` keep their shape.

**`zeromesh.v1.RunApi`** (unix socket, owner-only, beside `ClientApi`):

| RPC | Purpose |
|---|---|
| `Snapshot(path, options) → WorkspaceRef` | incremental snapshot; returns size, file count, what was skipped |
| `CaptureEnv(argv, probes) → EnvRef`, `ResolveEnv(spec) → EnvRef` | build or look up an environment |
| `SubmitRun(RunSpec) → run_id`, `OpenSession(RunSpec) → run_id`, `AddItems(run_id, items)`, `CloseSession(run_id)` | static and dynamic runs |
| `WatchRun(run_id) → stream RunEvent` | item progress, batch placement, traffic, failures, cache status |
| `GetRun`, `CancelRun`, `ListRuns`, `GetReport` | status, cancel, history, the standard report |
| `CacheForget(selector)`, `CacheExplain(item)` | poisoned-cache escape, "why did it miss" |
| `Doctor() → findings` | the checks behind `zm doctor` |

**Capability** is added to discovery announcements and `Ping`: programs with versions, environment
digests held, GPU, arch, libc. `OfferTask` carries the Requirement and the donor answers with the
specific miss.

**Services inside `zmd`:** Snapshot, Environment, Run engine (batching, sessions, journal, fair-share by
per-requester round-robin at each donor's Offer), Result (verify, stream, place), Cache (footprint,
team-wide lookup through `NodeApi`, inspect/forget), Resource learning (cgroup `memory.peak`, duration,
per kind and input size), Observability (item events, report, history, timeline export), Doctor.

**Anti-goals hold.** Nothing in `core/` is named after a domain tool. `kind` is an opaque string.
Adapters never link against internals.

## 11. SDK, CLI and report

```
zeromesh.adapter   Adapter, Item, Out, Env, Res, Policy, Stage, Workspace, main()
zeromesh.session   Session: add(), as_completed(), attach(id), close()
zeromesh.testing   FakeMesh(nodes=, slots=, latency=, churn=, oom=, cache=); mesh.run(AdapterClass, argv)
zeromesh.cli       shared flags (below)
zeromesh.report    the standard schema and readers
zeromesh.client    the low-level Task/Job client (kept)
```

Shared flags every adapter gets: `--env host|capture|sha256:…`, `--cores`, `--mem`, `--gpu`,
`--remote-only`, `--local-only`, `--keep-going`, `--dry-run`, `--batch N` (override), `--show-upload`,
`--budget`, `--deadline`, `--resume ID`, `--socket`, `-v`, `-o/--out`.

Report (`zm-out/report.json`): `run` (id, adapter, started, finished, policy), `nodes` (hostname, slots,
excluded and why), `items[]` (id, kind, status, attempts[] with node, wall, cpu, peak_mem, bytes_in,
bytes_out; cache: hit|miss with reason; failure: kind, exit, log tails; outputs placed), `totals`
(bytes pushed, pulled, seeded, cache hits, donor-seconds), `timeline` (Perfetto-compatible events).

## 12. Extension points and trust

Ranked by how often an adapter developer touches them:

1. **Adapters** (always): out of process, any language, untrusted.
2. **Environment recipes** (sometimes): data (trees and mount recipes), authored with `zm env capture`
   or imported from an image. A donor mounts them read-only inside the sandbox, so an adapter author
   shipping one does not widen what the donor already agrees to.
3. **`run.json`** (other languages).
4. **Core C++ plugins** (never, for adapter developers): `IEnvProvider` for FUSE mirror and future
   `env-oci`, `IFrontend` for REAPI. Trusted, installed by the machine owner, as today.

## 13. Archetypes

| Archetype | Examples | Needs beyond the common set |
|---|---|---|
| Map | render, sweep, compile | items, batching, outputs with destinations, footprint cache |
| Pipeline | preprocess → N → reduce | `after`, stages, `finish()` or reduce items |
| Dynamic | hyperparameter search, adaptive refinement, probe-then-expand | Session, results feedback loop, streaming submission |
| Iterative / long-running | simulations with checkpoints, training | checkpoint contract, warm dirs, partial commit |
| Interactive dev loop | pytest shards, incremental compile | low dispatch latency, incremental snapshots, footprint cache |
| GPU | Cycles, inference | `gpu` resource, device and driver mounts |
| Big outputs | video encode, image processing | per-item streaming, parallel pull, no permanent pinning |
| Services (v2 reserved) | a model server for a test suite | Services/Channels, network allowlist |

## 14. Worked example: `zm-render`

### Before

744 lines; about 180 are Blender (argv and null-audio env, `.blend` absolute-path scan, frame naming,
ffmpeg) and about 560 are plumbing: chunking, proto building, a 110-line event loop, a 45-line result
loop that parses Blender's stderr and stdout, fetch into staging directories and flatten, a frame
existence check, five result/exception types, and a CLI with seven generic flags and six `except`
branches.

### After

```python
from pathlib import Path
from zeromesh.adapter import Adapter, Item, Out, Env, Res, main
from zm_render.frames import parse_frames, frame_filename      # unchanged
from zm_render.blend_paths import warn_external_paths           # unchanged

BLENDER_ENV = {"ALSOFT_DRIVERS": "null", "SDL_AUDIODRIVER": "dummy",
               "XDG_RUNTIME_DIR": "/tmp/xdg-runtime", "PIPEWIRE_RUNTIME_DIR": "/tmp/xdg-runtime"}

class BlenderRender(Adapter):
    name = "zm-render"
    kind = "blender"
    env  = Env.host("blender", version=">=4.0")
    res  = Res(cores=4, mem="4G")

    def add_args(self, p):
        p.add_argument("scene", type=Path)
        p.add_argument("--frames", required=True)
        p.add_argument("--engine", metavar="NAME")
        p.add_argument("--blender", default="blender")
        p.add_argument("--encode", type=Path)

    def check(self, a):
        warn_external_paths(a.scene, file=self.log)

    def workspace(self, a):
        return a.scene.parent

    def items(self, a):
        for f in parse_frames(a.frames).frame_numbers():
            yield Item(id=f"frame_{f:04d}", argv=self.blender(a, a.scene.name, f, f), env=BLENDER_ENV,
                       outputs=[Out(f"out/{frame_filename(f)}", dest=a.out_dir / frame_filename(f))])

    def combine(self, a, items):
        return self.blender(a, a.scene.name, items[0].frame, items[-1].frame)

    def blender(self, a, blend, start, end):
        argv = [a.blender, "-b", blend, "-noaudio", "-o", "//out/frame_####", "-F", "PNG",
                "-s", str(start), "-e", str(end), "-a"]
        if a.engine:
            argv[4:4] = ["-E", a.engine]
        return argv

    def finish(self, a, run):
        if a.encode and run.ok:
            encode_video(a.out_dir, parse_frames(a.frames), a.encode)

main(BlenderRender)
```

### What a run does, step by step

| Step | Platform | Today |
|---|---|---|
| Environment | `Env.host("blender", ">=4.0")` excludes donors without it, and the report says so; `--env capture` ships the requester's Blender by digest | host: the donor refuses with a toolchain-digest message; mirror: `--mirror /opt/blender` by hand |
| Cache | frames rendered yesterday by anyone on the team are hits, keyed on the files Blender read | none unless the whole scene dir is byte-identical, and only on the same machine |
| Batch | 240 items, 10 live slots, learned 20 s/frame → batches of ~8 contiguous frames; `combine()` makes each batch one Blender process | `--chunk 1` default, `--cores 8` default |
| Run | the batch runner commits each frame when Blender closes the file | outputs captured at task exit |
| Stream | each frame lands in `renders/` as it finishes | fetched into `.chunk_a_b/`, walked, moved, verified at the end |
| Churn | a lid closed at frame 37 of batch 33-40 keeps 33-36; the retry is 37-40 | the whole chunk re-ran |
| Failure | `kind=exit, exit=1, stderr: "Error: File format is not supported"`; an OOM is `kind=oom`, memory for `blender` is bumped, the node is skipped for that kind for a while | 45 lines parsing stderr, then stdout for lines starting with `Error` |

### Tests

Today's tests mock `common_pb2` and compare argv strings; churn, missing frames and cache hits are
untestable because they live in `run_render` against a real daemon. After:

```python
def test_render_survives_departure(tmp_path):
    mesh = FakeMesh(nodes=3, slots=2)
    mesh.leave("node-1", after_items=5)
    run = mesh.run(BlenderRender, [str(SCENE), "--frames", "1-20", "-o", str(tmp_path)])
    assert run.ok
    assert run.report.items["frame_0006"].attempts == 2     # frames 1-5 kept, 6 retried
```

### Where the current code goes

| Current | Fate |
|---|---|
| `blender_argv`, `_BLENDER_ENV`, `blend_paths.py`, `parse_frames`, `frame_filename`, `encode_video` | stay |
| `Chunk`, `split_chunks`, `--chunk`, `--cores 8` | deleted; `--batch` is an optional override |
| `build_job_spec`, event loop, `_peer_names`, `_who`, `_print_event`, result parsing, fetch and flatten, `collect_frames`, the five result/exception types, generic CLI flags | deleted; platform |
| `--env mirror --mirror PATH` | `--env capture` or `--env sha256:…`; kept as a deprecated alias for one release |

`zm-cc` follows the same moves: `flags.py` and depfile parsing stay; the two modes, wave submission,
watch/collect, memory constants and `mkroot.py` go. One path remains: upload the referenced sources and
include directories, one item per translation unit, full remote compile, `footprint=Depfile`. Local
preprocessing survives only as the fallback for a donor with no captured environment.

## 15. Milestones

Each is done only when its acceptance test passes, as in `PLAN.md`.

- **M7: Items and outputs.** Run engine over Job/Task: items, batching by cost, the batch runner with
  per-item commit, outputs with destinations streamed per item, structured failure kinds, run policy,
  item-level events, the standard report, `zm doctor`.
  *Acceptance:* `zm-cfd` ported under 60 lines of adapter code; on three machines, killing a donor
  mid-batch loses at most one case and the report shows every other case's outputs committed before
  the loss.
- **M8: SDK.** `Adapter` base, shared CLI, `FakeMesh`, `run.json`. `zm-render` and `zm-cc` ported;
  `zm-cc` has one mode.
  *Acceptance:* each adapter's churn, OOM and missing-output behaviour is covered by a test against
  `FakeMesh` with no daemon; `zm-render` is under 150 lines of adapter code; the 240-frame four-machine
  render (M5) passes through the new adapter.
- **M9: Environments and requirements.** Capability in discovery, Requirement matching at Offer,
  `zm env capture`, fetch by digest. `mkroot.py` deleted.
  *Acceptance:* a donor without Blender renders through a captured environment; a donor with the
  wrong Blender version is excluded and named in the report.
- **M10: Incremental and shared.** Incremental snapshots, footprints, team-wide cache, cache
  explain/forget, run journal and `--resume`, Sessions.
  *Acceptance:* a second `zm-cc` run after a one-header edit recompiles only the units that include
  it, with hits served by another team member's node; a `zm-render` run resumed after the adapter
  process is killed finishes without re-rendering committed frames.
- **M11: Learning and archetypes.** Resource learning from cgroups, GPU resources and mounts, warm
  dirs, the checkpoint contract, SharedPath, fair-share.
  *Acceptance:* with no declared resources, the third run of each adapter places tasks without an
  OOM on a 4 GB donor; two requesters submitting at once each get at least 40% of the pool.

Benchmark, run for every adapter from M8 on and committed with the results: clean run, one-input edit,
one-item edit, donor leaves mid-run, each against plain local execution.

## 16. Limits and open decisions

- GPU and OCI import need network access on donors (driver libraries, registries) and per-distro
  driver layouts. They are real work, not a flag.
- Footprint tracing without a depfile or FUSE costs overhead (ptrace) and stays opt-in. Without a
  source, the cache degrades to whole-workspace keys and the report says so.
- Fair-share in a system with no head node is per-requester round-robin at each donor's Offer; no
  global quotas.
- Warm dirs trade hermeticity for speed and default to off.
- Runs owned by the daemon change what a daemon restart means; the run journal is small and on disk,
  and a restart resumes watching, not executing.
- Services and Channels stay reserved; nothing here precludes them.

---

## Appendix A: the full catalog of adapter-developer expectations

Status on branch `zm_perf_cc`, 2026-11-02. ✅ exists · ◐ partial or adapter-side · ✗ missing.
Numbers are referenced from the milestones and the archetype table.

### A.1 Getting started

| # | Expectation | Today |
|---|---|---|
| 1 | `pip install zeromesh` and a distro package for the daemon | ◐ SDK from repo path; .deb and PKGBUILD exist |
| 2 | `zm adapter new NAME` scaffolds package, CLI, tests, README | ✗ |
| 3 | Runs with no mesh: a lone daemon executes items locally in the same sandbox | ✅ `may_run_local` |
| 4 | `zm doctor`: bwrap version, AppArmor userns, `/dev/fuse`, multicast, clock skew, team joined, why a donor rejects | ✗ (README prose) |
| 5 | Concepts in ten minutes, one example per archetype, API reference, cookbook | ◐ app READMEs |
| 6 | SDK ↔ daemon version check with a clear message and a compatibility policy | ✗ |
| 7 | Typed, documented SDK with completion | ◐ |

### A.2 Describing work

| # | Expectation | Today |
|---|---|---|
| 8 | The unit I describe is my unit (Item), not a sandbox run | ✗ |
| 9 | Per item: id, cwd, argv, env, inputs subset, outputs, kind, cost, priority | ◐ per task |
| 10 | Stable ids so a re-run resumes | ✗ |
| 11 | Dependencies between items, fan-in reduce items | ◐ task-level `depends_on`/`inputs_from`, unused |
| 12 | Dynamic items added while the run is live | ✗ |
| 13 | Streaming submission with bounded in-flight | ✗ (zm-cc submits waves itself) |
| 14 | Stages with different requirements | ✗ |
| 15 | Deadlines and budgets | ✗ |
| 16 | Dry-run that shows what would run where | ◐ zm-cc only |
| 17 | A run outlives the adapter process; `attach` | ✗ |

### A.3 Data in

| # | Expectation | Today |
|---|---|---|
| 18 | Snapshot with `.zmignore`, size warnings, symlink policy | ✅ (`UploadTree`; only `zm run` applies it fully) |
| 19 | Incremental snapshots | ✗ |
| 20 | Per-item input subsets | ◐ by hand |
| 21 | Dedup, chunked transfer, resume, peer seeding | ✅ seeding, ◐ resume |
| 22 | Help with external absolute paths referenced by inputs | ◐ zm-render scans the `.blend` itself |
| 23 | Extra roots mounted at the same absolute path on the donor | ◐ mirror `--mirror` |
| 24 | SharedPath inputs (NFS): never uploaded | ✗ |
| 25 | Inline per-item parameters | ✗ |
| 26 | Secrets deny list in every mode | ◐ mirror only |
| 27 | Show what will be uploaded and how big | ◐ size cap only |

### A.4 Environments

| # | Expectation | Today |
|---|---|---|
| 28 | Requirements with version constraints matched to donor capabilities; the report names excluded donors | ✗ (host = byte-identical toolchain digest) |
| 29 | Capture my environment once, ship by digest | ◐ `mkroot.py` is adapter code |
| 30 | Container images | ✗ (`env-oci` listed as v2) |
| 31 | Ship a venv/conda/nix tree | ◐ as 29 |
| 32 | GPU devices and driver libraries in the sandbox; VRAM as a resource | ✗ (old GPU code frozen) |
| 33 | Per-item env vars, HOME, TMPDIR, locale, writable scratch | ◐ |
| 34 | Warm dir per (environment, kind) | ✗ (`input-cache` is for inputs) |
| 35 | Network allowlist per item | ◐ bool |
| 36 | Architecture and libc compatibility checked before placement | ✅ arch, ✗ libc |

### A.5 Resources and scheduling

| # | Expectation | Today |
|---|---|---|
| 37 | Declare cores, memory, GPU, scratch disk, wall time per kind | ✅ cores/mem/timeout, ✗ gpu/disk |
| 38 | Learned resources per kind when undeclared | ✗ |
| 39 | Multi-threaded and whole-node items | ◐ cores |
| 40 | Batching by cost against live slots; no `--chunk` guess | ✗ |
| 41 | Fairness between requesters; priority | ✗ |
| 42 | Pin to, exclude, or co-locate with a node | ✗ |
| 43 | Capacity view including who has my environment | ◐ `peers()` |
| 44 | Preemption semantics stated; checkpoint hook | ◐ freeze exists; no signal contract |
| 45 | Data-aware placement | ✅ |
| 46 | Low dispatch latency for small items | ◐ (`input-cache` on this branch) |

### A.6 Results

| # | Expectation | Today |
|---|---|---|
| 47 | Declared outputs with destinations; globs; directories; "what changed" | ◐ fetch into a dir; adapters move files |
| 48 | Outputs stream back per item | ◐ `UnitCommitted` exists, unused |
| 49 | Verification and output-name collision detection | ◐ zm-cc only |
| 50 | Per-item logs, live, retained, capped | ✅ |
| 51 | Per-item wall, CPU, peak memory, bytes, node, cache status | ◐ wall/node/cache |
| 52 | Local `finish()` or remote reduce | ◐ adapter-side |
| 53 | Large outputs pulled in parallel, not pinned forever | ◐ |

### A.7 Failure and correctness

| # | Expectation | Today |
|---|---|---|
| 54 | Structured failure kinds with log tails | ✗ |
| 55 | Infra retries always; exit retries by policy; local fallback last | ◐ |
| 56 | Keep-going vs fail-fast as run policy | ◐ per adapter |
| 57 | Cancel keeps finished items; resume later | ◐ |
| 58 | A departing donor loses at most the item in flight | ◐ mechanisms exist, not wired to items |
| 59 | OOM or sandbox failures reported and the node avoided for that kind | ◐ |
| 60 | Hung-item detection | ✗ |
| 61 | Hermeticity check in dev mode | ✗ |
| 62 | `zm cache forget`, cache explain | ✗ |

### A.8 Caching and re-runs

| # | Expectation | Today |
|---|---|---|
| 63 | Re-run with one input changed runs only affected items | ◐ whole-root key per task |
| 64 | Footprint-keyed cache | ✗ |
| 65 | Team-wide cache | ✗ (open question in `architecture.md` §14) |
| 66 | Hit/miss with reason in the report | ✗ |
| 67 | Cache limits, GC, pinning during a run | ✅ |

### A.9 Observability

| # | Expectation | Today |
|---|---|---|
| 68 | Standard progress display for every adapter | ✗ (three hand-written loops) |
| 69 | `zm monitor` shows items | ◐ tasks |
| 70 | Run history | ✗ |
| 71 | One report schema | ✗ |
| 72 | Timeline export | ✗ |
| 73 | Events API for dashboards | ✅ `WatchJob` |
| 74 | Donor owner view: what ran here, by whom | ◐ `ListActivity` |

### A.10 Testing and CI

| # | Expectation | Today |
|---|---|---|
| 75 | `FakeMesh` | ✗ (adapters mock `common_pb2`) |
| 76 | `zm dev mesh --nodes N` on one machine | ◐ test fixtures |
| 77 | Conformance checks | ✗ |
| 78 | CI recipe | ✗ |

### A.11 Shipping

| # | Expectation | Today |
|---|---|---|
| 79 | `pip install zm-render` alone, pinned to an SDK version | ◐ |
| 80 | `zm apps`; `zm render …` dispatch via entry points | ✗ |
| 81 | `run.json`; gRPC examples in other languages | ✗ |
| 82 | Proto evolution rules; mixed daemon versions negotiate at Admit | ✗ |

### A.12 Trust

| # | Expectation | Today |
|---|---|---|
| 83 | Stated isolation guarantees | ✅ |
| 84 | Donor rejections explained in plain words | ◐ |
| 85 | Audit on the donor | ◐ |
| 86 | Environments are data, never code the donor must trust | ✅ by construction |
