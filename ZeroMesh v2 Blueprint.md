# ZeroMesh v2 Blueprint

Oct 29, 2026 · @lucifer

## Overview and thesis

ZeroMesh v2 is the compute pool built for laptops that leave: it moves each input once, stays out of the owner's way, and finishes jobs when machines disappear. HTCondor and BOINC were designed for desktops that stay put and owners judged by keyboard idle time; that assumption is the gap.

The pitch in one line: **lend your laptop without noticing, jobs finish even when people leave, and re-runs are free.** Each part is a claim we measure against HTCondor and BOINC on the same machines. None of the mechanisms is new on its own; the contribution is the combination for small laptop pools, with numbers.

This blueprint is version 2.0 and is expected to change. Rewrite any section once measurements disagree with it.

### Where v1 stands (audit of main, 29 Oct 2026)

| Area | Status | Where |
| --- | --- | --- |
| Content-addressed store, Merkle trees, action cache | Working | `core/store`, `core/node/task_key.cpp` |
| bwrap sandbox, `SCHED_IDLE`, per-task cgroup v2 (memory, cpu.max, pids) | Working | `core/exec/sandbox_runner.cpp` |
| Remote execution over mTLS (Offer, Push, Pull, Run, Cancel, Drain) | Working in automated tests on one host; two-machine acceptance pending | `core/node/node_service.cpp`, `tests/integration/test_remote_exec.py` |
| Dispatch policies, retries on node loss (3, keepalive 10 s) | Working | `core/sched/placer.cpp`, `dispatch_policy.cpp` |
| env-mirror (FUSE lazy fetch of requester binaries) | Working, skipped in CI without `/dev/fuse` | `plugins/env-mirror` |
| REAPI front-end | Partial (in-memory action cache, WaitExecution stub) | `plugins/frontend-reapi` |
| Drain on low battery | Kills tasks; no freeze, no partial results | `core/node/daemon.cpp` |
| Docs | README contradicts PLAN and code | `README.md`, `PLAN.md`, `docs/HANDOFF.md` |

What v1 lacks is not plumbing but proof: there is no committed measurement of speedup, bytes moved or owner impact.

## The 3-minute pitch and demo

We write the pitch and the results table first and build toward them; every cell below starts empty and is filled only by a measured run.

**Setup on stage:** 4 laptops on one Wi-Fi network, ZeroMesh and a tuned HTCondor pool installed on the same machines, a 200-case CPU-bound sweep and a 240-frame Blender render ready.

1. **Hook (0:00–0:30).** "Your club has 20 laptops idle all evening. HTCondor can pool them, but it was built for lab desktops that never leave. Laptops close their lids, drop off Wi-Fi and have owners who notice slowdowns."
2. **Wow (0:30–1:15).** Start the sweep from one laptop. The live monitor shows tasks spreading across all four. Point at the bytes counter: the requester sends each input once per donor, not once per job.
3. **Twist (1:15–2:15).** A teammate opens a browser benchmark on a donor: the governor steps donated work down and the owner's latency line stays flat. Then close another laptop's lid: its finished cases are already saved, only the unfinished one moves, and a backup copy covers the tail.
4. **Payoff (2:15–3:00).** Re-run the sweep with one input changed: only that case runs. Show the table below against HTCondor and BOINC.

**Fallback:** a recorded run of the same script with its logs and the filled table, played if the live network fails.

### Headline results (filled only from measured runs)

| Metric | HTCondor (default) | HTCondor (tuned) | BOINC-style | ZeroMesh v2 |
| --- | --- | --- | --- | --- |
| Requester upload for the sweep (GB) |  |  |  |  |
| Time to first result on a donor without the software (s) |  |  |  |  |
| Owner p99 latency increase while donating (%) |  |  |  |  |
| Lost CPU-seconds per laptop departure |  |  |  |  |
| p95 makespan at 30% churn per hour (min) |  |  |  |  |
| Second run with one changed input (s) |  |  |  |  |
| Wrong cache hits under toolchain drift (count) |  |  |  |  |

## Prior art and what we adapt

Every piece of v2 exists somewhere; we take one pattern from each and cite it up front. Nothing here is a claim of being first.

| System | What it does well | What it lacks for laptop pools | What we adapt |
| --- | --- | --- | --- |
| [HTCondor](https://htcondor.readthedocs.io/en/v9_0/admin-manual/policy-configuration.html) | General batch jobs, owner policies, ClassAd matchmaking, one-command installer | Re-sends identical inputs per job; desktop policy reacts only to keyboard use or more than 2 min of load; central manager must stay up | Owner-first framing; tuned config is our main baseline |
| [BOINC](https://arxiv.org/pdf/1903.01699) | Volunteer computing, adaptive replication for trust | Targets jobs of about an hour; static thresholds; needs a project server | Adaptive spot-checking idea |
| [icecream](https://www.mankier.com/7/icecream) | Zero-config LAN discovery, ships the compiler as a tarball | Compiles only | Environment shipping (our env-mirror) |
| [Flamenco](https://flamenco.blender.org/usage/job-types/builtin/) | Blender jobs as small scripts; chunk size is a static setting | Needs shared storage and one Manager | Adapter-per-domain model |
| [TaskVine](https://par.nsf.gov/servlets/purl/10567835) | Worker-to-worker transfers, placement on the worker holding most input bytes | Central manager, HPC focus | Peer seeding and data-aware placement |
| [BuildXL](https://github.com/microsoft/BuildXL/blob/main/Documentation/Wiki/Advanced-Features/Two-Phase-Cache-Lookup.md), [Riker](https://usenix.org/system/files/atc22-curtsinger.pdf) | Caches keyed on files a task actually read | Needs a build graph (BuildXL); local only (Riker) | Toolchain-aware cache key |
| IncrediBuild | Runs unmodified processes on helpers that lack the tools; build cache | Proprietary, build-focused | Proof the env-mirror idea is wanted |
| onepool, hearth, mesh-zero (2026) | Zero-config LAN pooling, alpha quality | No sandbox, store, cache or owner policy | Confirms demand; not a baseline |

From recent papers we take four patterns:

- **Recorded access-order prefetch** from [Starlight](https://usenix.org/system/files/nsdi22-paper-chen_jun_lin.pdf) (NSDI'22) and REAP (ASPLOS'21), which report 3.0× and 3.7× faster cold starts.
- **Graded pressure control** from [UFS](https://arxiv.org/abs/2605.02377) and SARA (2025–26 preprints). UFS found `SCHED_IDLE` protected the foreground no better than normal scheduling.
- **Per-node departure hazard** from [SkyNomad](https://arxiv.org/abs/2601.06520), plus liveput-style placement from Parcae (NSDI'24).
- **Finer-grained commit inside a grace window** from SpotServe.

We skip CRIU checkpointing because an [HTCondor pool study](https://arxiv.org/abs/2402.05244) found it fails inside container runtimes.

## v2 architecture

v2 keeps the v1 core and adds five modules inside `zmd`: three for owner comfort and churn, two for data movement and cache correctness. No module moves out of `core/`, and apps still reach the daemon only through the ClientApi.

&#91;embedded content: zmd v2 · 4 existing modules, 5 new\]

The governor and departure controller act on the donor through the kernel; the hazard model and seeding feed the placer's choice of donor.

## Claim 1: data moves once

The requester sends each input at most once per donor, against HTCondor's once per job. This is the safest claim and v1 already does most of it: the placer pushes only blobs a donor is missing (`core/sched/placer.cpp:510-573`).

HTCondor's [file-transfer docs](https://htcondor.readthedocs.io/en/24.0/users-manual/file-transfer.html) say input is copied from the submit machine for every job, even when identical. For N jobs sharing an input of size S on D donors, that is about N × S against our D × S.

### What we add

1. **Measure it now (week 1).** Count bytes per NodeApi call and report requester upload for the sweep. No new design needed.
2. **Peer seeding on wired links.** A donor fetches a missing blob from the least-busy peer that holds it, capped at 3 concurrent sends per source as TaskVine does, and from the requester only as a last resort. On shared Wi-Fi, cap LAN-wide transfers at 1–2: airtime is shared, so seeding mainly helps on wired networks.
3. **Recorded prefetch for env-mirror.** On the first run of an environment, the FUSE layer logs which chunks were read and in what order. The profile is stored in the store under the environment's root hash. Later donors stream those chunks in order while the task is starting, as Starlight and REAP do.
4. **Bulk-fetch crossover.** If a profile covers more than 80% of an environment, fetch it whole instead (the crossover point reported for SOCI, a 2026 preprint).
5. **Placement by bytes.** The placer's cost becomes missing bytes divided by the measured bandwidth to the best source, not missing bytes alone.

### Metrics

- Requester upload bytes for the 200-case sweep and the 240-frame render.
- Time to first result on a donor with no software installed, with and without the recorded profile.
- Bytes moved when one library in the environment changes.
- FUSE round trips per task.

## Claim 2: owner comfort governor

A governor in `zmd` watches the owner's resource pressure and steps donated work down in stages, so the owner's p99 latency stays within a stated bound. It replaces today's two settings: `SCHED_IDLE` always, and kill on drain.

`SCHED_IDLE` alone is weak: the [UFS preprint](https://arxiv.org/abs/2605.02377) found it did no better than plain EEVDF for foreground latency. HTCondor's [desktop policy](https://htcondor.readthedocs.io/en/v9_0/admin-manual/policy-configuration.html) suspends only on keyboard use within 1 minute or load of 0.5 or more for 2 minutes.

### The ladder

The governor reads PSI (`/proc/pressure` and the owner's slice: cpu, memory, io) with epoll triggers over 500 ms–1 s windows. On sustained pressure it moves one step down; after a hold time with no pressure it moves one step up.

1. Lower `cpu.weight` and set `cpu.idle` on the task cgroup.
2. Set `memory.high` and write one bounded `memory.reclaim`.
3. Lower `io.weight` (needs the io controller delegated).
4. Write `cgroup.freeze` to pause the task.
5. Only after a timeout: cancel and re-dispatch (claim 3 decides when).

Hold timers and a cooldown stop it oscillating, following AgentLens (2026 preprint). The current comfort level goes into the `Offer` reply, so the placer stops sending work to a squeezed donor.

### Where it goes

- **Per-task cgroups:** keep the handle beyond `Run()` and add the io controller (`core/exec/sandbox_runner.cpp`; systemd unit gets `Delegate=cpu memory pids io`).
- **Governor and PSI reader:** new `core/node/comfort_governor.cpp`, fed by the existing telemetry sampler.
- **Donor keys:** `comfort_target_ms`, `max_step`, `hold_s` in `donor.toml`.

### Metrics

- A probe program in the owner's cgroup wakes every 100 ms; report its p50 and p99 wake delay. This method comes from the PSI-reclaim preprint.
- Real foreground work: Speedometer 3 in a browser, typing latency, a foreground `ninja` build.
- Reaction time from the owner's activity starting to donated work stepping down.
- Donated throughput kept, as a fraction of the `SCHED_IDLE`-only setting.

PSI triggers faster than 2-second windows need `CAP_SYS_RESOURCE`, so `zmd` must run with it.

## Claim 3: churn-proof completion

When a laptop leaves, ZeroMesh loses only the unfinished part of its in-flight task, and backup copies cover the end of the job. Today a drain kills the task and the retry starts from zero (`core/node/daemon.cpp:460-508`).

### Freeze-first departure

1. **Signals:** AC unplug, battery below threshold, the owner's return (from claim 2), and lid or suspend from logind's `PrepareForSleep` with a delay inhibitor. v1 reads no lid, suspend or network signals.
2. **Finish what fits:** tasks whose expected remaining time, from action-cache runtime history, fits the grace window keep running.
3. **Freeze the rest** with `cgroup.freeze`, and keep them frozen for a time-to-live set by the donor's departure hazard.
4. **Kill and re-dispatch** only when the time-to-live expires.

### Partial commit

Apps stream each finished unit (a frame, a sweep case) into the store as soon as it completes, as a partial action-cache entry. A retry then runs only the missing units. This follows SpotServe's finer-grained commit. It needs an SDK call (`commit_unit`), so `zm-render` and `zm-sweep` adopt it first.

### Hazard-aware placement and tail hedging

- **Hazard model:** log each node's join and leave events and fit a Nelson-Aalen estimate, counting graceful drains as censored, as [SkyNomad](https://arxiv.org/abs/2601.06520) does. Pool data across nodes as a prior, since 5–20 laptops give few samples.
- **Placement:** rank donors by expected committed work, slots × E\[min(remaining life, task time)\], the liveput idea from [Parcae](https://arxiv.org/abs/2403.14097).
- **Hedging:** only near the end of a job, when slots are free. Copy a task to a second donor if its chance of surviving its runtime is below a threshold. Cap at 2 copies, and cancel the loser with the existing Cancel RPC. A simulated HTCondor study found 2–3 replicas help and 4 or more hurt ([McGough and Forshaw](https://arxiv.org/abs/2209.13531)).

### Metrics

- Lost CPU-seconds per departure.
- p95 and p99 makespan under injected churn at 10%, 30% and 50% per hour, on the 200-case sweep.
- Extra CPU-seconds spent on hedging; any claim reports this cost beside the gain.

CRIU checkpointing is out of scope (see Prior art).

## P0 fixes before any claim

These five fixes land in week 1, because each one can make a measured number wrong or unsafe.

- [ ] **Toolchain in the cache key.** `task_key.cpp:67-97` hashes argv, env and inputs but not the donor's `/usr` and `/etc`. A donor with a different gcc can return a wrong cache hit. Add a toolchain digest (hash of the resolved executable and its loaded libraries) to the key.
- [ ] **Pin env-mirror files of running tasks.** The 512 MB LRU in `mirror_fuse.cpp` can evict a file a running task still needs. Pin chunks mapped by live tasks; evict only unpinned ones.
- [ ] **bubblewrap version.** Require bubblewrap 0.12.0 or newer and refuse a setuid bubblewrap. Older versions have 2026 sandbox-escape advisories ([oss-sec](https://seclists.org/oss-sec/2026/q3/603)).
- [ ] **Docs match code.** README says remote execution, team security and FUSE do not work; all three are built and tested. Fix README, PLAN.md and `docs/HANDOFF.md`.
- [ ] **Two-machine acceptance.** Run `tests/acceptance/m3_two_machines.md` on real hardware and record the result. CI runs every integration test on one host.

Later, before the action cache is shared across the team, add trust: results land as unverified, a sampled fraction is re-run on another donor in the background, and an entry must be verified before its third reuse. That costs roughly BOINC's adaptive-replication budget of 5–10% instead of 2× replication.

## Data contract changes

All changes go into `zeromesh.v1` as new optional fields, so v1 nodes keep working. These contracts are agreed in week 1, before the streams split, as M1 was in v1.

| Message / file | New field or type | Used by |
| --- | --- | --- |
| `OfferReply` | `comfort_level` (0–4, ladder step), `hazard_per_hour` | Placer (claims 2, 3) |
| `OfferReply` | `held_bytes` for the task's input root, per source peer | Placer cost (claim 1) |
| `PullRequest` | `source_hint` (peer node id to fetch from) | Peer seeding (claim 1) |
| `EnvProfile` (new, stored in the store) | ordered list of (blob, chunk, offset) per environment root hash | Recorded prefetch (claim 1) |
| `RunTask` | `profile_id`, `grace_window_ms` | Prefetch; freeze-first drain |
| `TaskEvent` | `UNIT_COMMITTED` with unit id and output digest | Partial commit (claim 3) |
| `DrainNotice` | `reason` (unplug, battery, lid, owner), `ttl_ms` | Freeze-first drain (claim 3) |
| `task_key` | `toolchain_digest` added to the hashed fields | P0 cache fix |
| `donor.toml` | `comfort_target_ms`, `hold_s`, `max_copies` | Governor, hedging |
| `ActionResult` (later) | `receipt` (donor id, key, output digest, signature), `verified` flag | Shared-cache trust |

## Evaluation plan

Every claim is measured on the same 4 laptops against default HTCondor, tuned HTCondor and a BOINC-style policy, one mechanism at a time. Success thresholds are set below before any run.

### Harness and workloads

- **`zm-sweep`** (new app, about 500–800 lines of Python on the SDK): expands a grid or YAML into cases, aggregates results, and supports `commit_unit`. Its first run doubles as the pending M4 acceptance test (`tests/acceptance/m4_three_machines.md`).
- **Workloads:** a 200-case CPU-bound sweep (about 30 s per case), the 240-frame Blender render via `zm-render`, and a mid-size C project build.
- **Churn injector:** replays departure traces (Poisson at 10/30/50% per hour, or recorded lid and unplug logs) by suspending or draining donors.
- **Owner probe:** a 100 ms periodic wake-up program in the owner's cgroup, plus Speedometer 3 and a foreground `ninja` build.

### Baselines

- **HTCondor default** and **HTCondor tuned:** short negotiator interval, short job lease, `max_retries`, rank on `KeyboardIdle`. HTCondor reuses a match for later jobs, so the gap narrows after the first job.
- **BOINC-style:** suspend on input, quorum 1 and quorum 2.
- **ZeroMesh v1** as shipped today.

### Ablation ladders

Each rung adds one mechanism, so we know which part earned the gain:

- **Claim 1:** whole input per job (HTCondor-like) → CAS dedup (v1) → peer seeding → recorded prefetch.
- **Claim 2:** no limits → `SCHED_IDLE` (v1) → `cpu.idle` + weights → + memory and io limits → + freeze.
- **Claim 3:** kill and retry (v1) → freeze-first → + partial commit → + hazard placement → + tail hedging.

### Success thresholds (set before running)

| Claim | Go if | Otherwise |
| --- | --- | --- |
| 1 | Requester upload at most 1/5 of HTCondor's on the sweep | Report the measured ratio; drop peer seeding if it adds under 10% |
| 2 | Owner p99 increase under 5% while keeping at least 70% of `SCHED_IDLE` throughput | Publish as a measurement of how much `SCHED_IDLE` leaks |
| 3 | Lost CPU-seconds per departure down at least 50%, with hedging under 15% extra CPU | Keep freeze-first and partial commit; drop hedging |

These thresholds are our targets, not results from the papers.

### Statistics

- At least 20 runs per configuration, order randomised; laptops on AC and pinned to a fixed power profile unless the test is about battery.
- Report the median with bootstrap 95% confidence intervals and every per-run result.
- Paired comparisons use the Wilcoxon signed-rank test, with Holm correction across variants.

## Team split and roadmap

Four streams run in parallel for six weeks, and every stream faces the same go/no-go gate on 22 Nov. The dates are a proposal starting Mon 2 Nov 2026; shift them as a block if the start moves.

| Stream | Scope | Delivers |
| --- | --- | --- |
| A: harness | `zm-sweep`, HTCondor and BOINC-style baselines, churn injector, owner probe | Every number in the results table |
| B: data | Byte counters, peer seeding, recorded prefetch | Claim 1 |
| C: comfort | cgroup handle, PSI reader, governor ladder | Claim 2 |
| D: churn | Lid and suspend signals, freeze-first drain, partial commit, hazard model, hedging | Claim 3 |

Assign one person per stream. Branches follow `stream/<letter>-<topic>`, which CI already builds.

&#91;embedded content: v2 roadmap · 4 streams, 3 gates\]

Stream A's harness is on the critical path: no claim can pass a gate until its baseline runs exist.

### Gates

1. **Gate 1, 8 Nov:** protos and `donor.toml` keys frozen, P0 fixes merged, v1 bytes measured on 4 real machines.
2. **Gate 2, 22 Nov:** each claim's first rung beats v1 on its metric across at least 10 runs. A claim that does not is cut back to a measurement-only result.
3. **Gate 3, 13 Dec:** headline table filled from at least 20 runs per cell, pitch rehearsed 5 times, fallback recording made.

## Risks and what we will not claim

The biggest risk is that claims 2 and 3 come out small on 4 laptops; we publish whatever we measure, including a null result.

| Risk | Effect | Fallback |
| --- | --- | --- |
| Wi-Fi and thermal noise swamp the effect | Wide confidence intervals | More runs, AC power, fixed power profile; report variance |
| PSI misses cache and memory-bandwidth contention | Governor reacts late | Add foreground probe latency as a second signal |
| Too few departures to fit a per-node hazard | Hedging no better than random | Pooled prior; synthetic churn traces |
| `CAP_SYS_RESOURCE` or io delegation unavailable | Slow PSI triggers, no io step | Run the ladder without io; document the requirement |
| Hedging wastes CPU at high load | Lower total throughput | Hedge only in the tail phase, cap at 2 copies |
| Most supporting papers are 2025–26 preprints | Their numbers may not hold | Cite them as preprints; rely on our own measurements |

**We will not claim:**

- To be first at any single mechanism, or new versus HTCondor in kind.
- Support for every workload. The fit is headless Linux jobs that split into independent pieces. Single MPI runs, GPU work and GUI apps such as Photoshop are out.
- Zero-declaration tracing of arbitrary commands. BuildXL and IncrediBuild already do it, and it is 5–6 weeks of high-risk work.
- Gains quoted from other systems as ZeroMesh numbers.

## References

Preprints are marked; their numbers are not peer-reviewed.

**Baselines and tools**

- [HTCondor desktop policy configuration](https://htcondor.readthedocs.io/en/v9_0/admin-manual/policy-configuration.html)
- [HTCondor file transfer](https://htcondor.readthedocs.io/en/24.0/users-manual/file-transfer.html)
- [HTCondor negotiator configuration](https://htcondor.readthedocs.io/en/latest/admin-manual/configuration/negotiator.html)
- [get\_htcondor installer](https://www.mankier.com/1/get_htcondor)
- [BOINC platform paper](https://arxiv.org/pdf/1903.01699) and [adaptive replication](https://boinc.berkeley.edu/trac/wiki/AdaptiveReplication)
- [icecream](https://www.mankier.com/7/icecream)
- [Flamenco job types](https://flamenco.blender.org/usage/job-types/builtin/)
- [BuildXL two-phase cache lookup](https://github.com/microsoft/BuildXL/blob/main/Documentation/Wiki/Advanced-Features/Two-Phase-Cache-Lookup.md)

**Data movement**

- [TaskVine](https://par.nsf.gov/servlets/purl/10567835)
- [Starlight, NSDI'22](https://usenix.org/system/files/nsdi22-paper-chen_jun_lin.pdf)
- [REAP, ASPLOS'21](https://arxiv.org/abs/2101.09355)
- [SOCI lazy loading (preprint, 2026)](https://arxiv.org/abs/2607.06868)
- [Riker, ATC'22](https://usenix.org/system/files/atc22-curtsinger.pdf)

**Owner interference**

- [UFS, sched\_ext (preprint, 2026)](https://arxiv.org/abs/2605.02377)
- [SARA (preprint, 2025)](https://arxiv.org/abs/2511.19991)
- [PSI-triggered bounded reclaim (preprint, 2026)](https://arxiv.org/abs/2608.13689)
- [AgentLens (preprint, 2026)](https://arxiv.org/abs/2610.25231)
- [Linux PSI documentation](https://kernel.org/doc/html/v6.10/accounting/psi.html)

**Churn and scheduling**

- [Parcae, NSDI'24](https://arxiv.org/abs/2403.14097)
- [SkyNomad (preprint, 2026)](https://arxiv.org/abs/2601.06520)
- [SpotServe](https://arxiv.org/abs/2311.15566)
- [Replication on an HTCondor trace (McGough and Forshaw, 2022)](https://arxiv.org/abs/2209.13531)
- [CRIU on the OSPool (2024)](https://arxiv.org/abs/2402.05244)

**Security**

- [bubblewrap advisory, oss-sec 2026 Q3](https://seclists.org/oss-sec/2026/q3/603)
