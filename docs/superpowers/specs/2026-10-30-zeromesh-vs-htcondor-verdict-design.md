# ZeroMesh vs HTCondor Verdict Suite: Design

- **Date:** 2026-10-30
- **Status:** Draft
- **Builds on:** `scripts/zmfield.sh` (w1–w3 bench), `scripts/baselines/`, "Evaluation plan" in `ZeroMesh v2 Blueprint.md`

---

## 1. Intent

The team needs one command that runs on the real laptops and produces a **verdict that holds up**: is ZeroMesh
faster than HTCondor, and is it better? The verdict has to be one a sceptical reviewer can't wave away as
cherry-picked, a weak baseline, noise, or moving the goalposts after seeing the numbers.

"Once and for all" here means **once and for all for the conditions tested**. No benchmark proves a claim about
every pool, network and workload. The suite proves something narrower that can be defended: on these machines, this
network and these workloads, against HTCondor configured as well as we can configure it, the answer is X, with a
stated confidence. The report prints those conditions next to the verdict so nobody can quote it out of scope.

What makes it "proof" and not just a benchmark:

1. **Pre-registration.** Hypotheses, metrics, thresholds, sample size and the verdict rule live in a config file.
   Its hash is recorded *before* the first run. A report whose config hash differs from the registered one is
   marked `INVALID: config changed after registration`.
2. **A strong baseline.** ZeroMesh is compared per metric against the **best** HTCondor configuration on that metric,
   not against an out-of-the-box install.
3. **Paired, randomised, repeated runs.** Every block runs every system back to back, in random order, on the same
   machines. That cancels drift from thermals, Wi-Fi and background load.
4. **A correctness gate.** A fast wrong answer counts as a loss. Every output is checked against a reference.
5. **Statistics that can say "don't know".** Each metric ends up as BETTER, WORSE, EQUIVALENT or INCONCLUSIVE.
   INCONCLUSIVE is a valid result and is never rounded up to a win.
6. **Full disclosure.** Raw per-run data, logs, host info, git commit and config hash ship with every verdict.

## 2. Goals, non-goals, anti-goals

### Goals
- One entry point: `bash scripts/zmfield.sh verdict`. It runs every workload, every system and every block, then
  writes `verdict.json` and `report.md`.
- Two headline answers, each with a one-word result and a confidence statement:
  - **FASTER?** Covers time-based metrics only.
  - **BETTER?** Covers every scored metric: time, bytes, churn and owner impact. Correctness is a gate on both.
- A per-metric table showing the effect size, the 95% CI, the adjusted p-value and the per-metric verdict.
- It can be resumed. The full run takes hours (§8), so a crash or reboot loses at most the block in progress.
- A smoke mode (`--smoke`) that runs everything at n=3 to check the plumbing. It **never** emits a verdict.

### Non-goals
- Ablations: which ZeroMesh mechanism earned the gain. That is the blueprint's separate ablation-ladder work.
- BOINC comparison. It could be added later as another system in the same harness.
- Generalising beyond the tested hardware and network. The report states its scope; it does not extrapolate.
- Replacing `zmfield.sh bench`. That stays as the quick exploratory tool. `verdict` is the formal run.

### Anti-goals: failures even if the suite technically works
- **A strawman HTCondor.** Running N separate `condor_submit` calls, leaving default 60 s+ negotiation cycles as the
  only config, or skipping HTCondor features that would help it. Any of these makes the verdict worthless.
- **Hidden cache wins.** ZeroMesh's action cache turning a repeat run into a no-op, except in the one workload (W4)
  that exists to measure exactly that, and is labelled so.
- **Peeking.** Adding runs until p < 0.05, or editing thresholds after seeing data. Prevented mechanically by §1.1
  and §6.4.
- **One headline number.** Collapsing everything into a single "ZeroMesh is 3× better" score. The verdict is a
  rule over per-metric results, not a weighted average someone can tune.
- **Unscoped claims.** A verdict printed without the hardware, network and node count it applies to.

## 3. Systems under test

| ID | What it is | Notes |
|---|---|---|
| `zm` | ZeroMesh v2 as shipped on the tested commit, governor **on** | The product as an owner would run it. `ZMF_NO_GOVERNOR` is not set. |
| `condor-default` | HTCondor from the distro package or container, stock config plus only what's needed to form the pool | Shows what users get out of the box. |
| `condor-tuned` | HTCondor with the best config we can justify for small churny laptop pools (§3.1) | **The main baseline.** |
| `zm-local` | ZeroMesh with `--local-only` on the requester | Reference only: gives speedup over one machine. Not part of the verdict. |

**Per-metric baseline = the better of `condor-default` and `condor-tuned` on that metric**, chosen per block pair
by median over all blocks. This stops a tuned config that helps one metric and hurts another from favouring
ZeroMesh.

### 3.1 HTCondor tuned: what it must include

Starting from `scripts/baselines/condor_tuned.conf`, extended. The builder must check every knob against the docs
**for the HTCondor version actually installed** (record it with `condor_version`), and drop or replace any knob
that doesn't exist there:

- Short scheduling cycles: `NEGOTIATOR_INTERVAL`, `NEGOTIATOR_CYCLE_DELAY`, `SCHEDD_INTERVAL`, `UPDATE_INTERVAL`
  (current values: 5/2/5/10).
- Claim reuse across jobs (`CLAIM_WORKLIFE` long enough to cover a workload) so HTCondor doesn't renegotiate per job.
- A short job lease (`job_lease_duration = 60`) and `max_retries` so jobs on a dead node come back.
- Applied on **every** pool node, not only the requester. The current bench tunes only the requester; that has to
  change for the verdict.
- One submit per workload: `queue ... from` or `queue N` in a single cluster, never N separate submits.
- **Input reuse, if the installed version has it.** Newer HTCondor releases may offer some form of shared or common
  input-file transfer, or file-transfer caching. If that exists and works rootless/containerised, `condor-tuned`
  uses it for W3 and W4. If it doesn't exist or can't be made to work, the report says so explicitly ("HTCondor
  X.Y: no input-reuse feature used, because ...").
- Owner policy at its best equivalent: `JOB_RENICE_INCREMENT` and suspend-on-load/keyboard settings, so W6 compares
  against an HTCondor that also tries to protect the owner.

The tuned config is committed under `scripts/baselines/` and its hash is part of the pre-registration.

## 4. Workloads and metrics

All workloads reuse zmfield's approach: the same payload for every system, a fresh copy of the inputs per run, and a
unique nonce argument per run so no system gets a cache hit it shouldn't. Every task writes a deterministic output
that the harness checks against a reference computed on the requester (§5).

Default execution is **remote-only** (the requester only submits) for W1–W4 and W6, so the comparison is about the
pool, not the requester's own CPU. W5 uses all nodes, as the current w3 does.

| ID | Workload | Primary metric | Secondary metrics | What it probes |
|---|---|---|---|---|
| W1 | **Short-task fan-out:** 200 tasks × ~1 s CPU | makespan (submit → last output on requester) | time to first result | Scheduling overhead. HTCondor's negotiation latency should hurt here. |
| W2 | **Long-task fan-out:** 24 tasks × ~60 s CPU on fewer slots than tasks | makespan | slot utilisation | Raw throughput when overhead is amortised. **A tie is the honest expectation.** |
| W3 | **Shared input:** 48 tasks × ~5 s reading the same 256 MB input | requester upload bytes | makespan | Claim 1 (data moves once). |
| W4 | **Incremental re-run:** W3, then change one task's small input file and rerun everything | makespan of the rerun | bytes, tasks actually executed | Re-runs are free. The only workload where cache reuse is allowed and expected. |
| W5 | **Churn:** 60 tasks × ~10 s; one donor is killed at ~30% of the no-kill makespan, then comes back | makespan | lost CPU-seconds, retries, all outputs correct | Claim 3. Kill mode is pre-registered (`kill -9` by default; `SIGTERM` as a second variant if registered). |
| W6 | **Owner impact:** a W2-style load on a donor while an owner probe runs on that donor | probe p99 wake delay increase vs idle | donated throughput (tasks/min) | Claim 2. Probe: wakes every 100 ms in the owner's cgroup and logs wake delay. Idle baseline measured in the same block. |

Task durations are calibrated once per session on the slowest donor, as `sim.sh` does now, and recorded.

**Measured but not scored** (no repeated sampling; reported in a separate "operational" table):
- Time from a clean machine to a working pool, for each system (`setup` + join).
- Commands and config lines needed to set up each system.
- Cold start of the first task on a donor that doesn't have the software (ZeroMesh env-mirror vs HTCondor
  transferring the executable). Scored only if the builder can make it repeatable within one block; otherwise reported.

**Not measured, stated in the report:** real lid close, suspend or unplug departures (W5 only kills processes),
Wi-Fi vs wired unless both are run, and more than the tested number of nodes.

## 5. Correctness gate

- Every task's output is compared byte for byte with a reference computed on the requester for the same nonce.
- A run with **any** missing or wrong output gets `ok=false`. For that block, that system loses every metric of the
  workload: its value is set to +∞ for "lower is better" metrics. A fast wrong answer can't win.
- W4 additionally checks that the changed task really re-executed and produced the new output. A stale cache hit
  counts as a wrong output.
- If any system fails correctness in more than 10% of blocks of any workload, the headline verdict for that system is
  `DISQUALIFIED on <workload>` and the per-metric table is still printed for transparency.

## 6. Statistics and the verdict rule

### 6.1 Design
- **Block** = one run of every system on every workload, in an order randomised per block (seeded; the seed is
  recorded). The pool is left to settle for a fixed interval between runs (pre-registered, default 15 s).
- **n = 20 blocks** per verdict (the blueprint's minimum).
- Before the first block: one discarded warm-up block, so first-run effects (image pulls, page cache, JIT) don't land
  in the data.
- Fixed conditions, checked and logged at the start of every block: AC power on every node, the same power profile
  or governor (`powerprofilesctl`/`cpupower` where available), no other user load (load average logged), and the
  same network. A block that fails a pre-check is retried once, then recorded as excluded with a reason. Excluded
  blocks are listed in the report.

### 6.2 Per-metric test
For each metric *m* and workload, per block *b*: `r_b = zm_b / baseline_b` (for "higher is better" metrics, invert
so that r < 1 always means ZeroMesh is better).

- Effect = median of `r_b`, with a bootstrap 95% CI (10,000 resamples of blocks, BCa or percentile; the builder
  picks one and records it).
- p-value: two-sided Wilcoxon signed-rank on `log r_b`.
- Holm correction across **all scored metrics** together.

### 6.3 Per-metric verdict (margins pre-registered; defaults shown)

| Verdict | Rule |
|---|---|
| **BETTER** | Holm-adjusted p < 0.05 **and** CI upper bound < 1.0 **and** median r ≤ 0.95 (at least a 5% practical gain) |
| **WORSE** | Holm-adjusted p < 0.05 **and** CI lower bound > 1.0 **and** median r ≥ 1.05 |
| **EQUIVALENT** | The whole 95% CI lies inside [0.95, 1.05] (the TOST-style equivalence margin) |
| **INCONCLUSIVE** | Anything else |

The W6 owner-impact metric uses an absolute margin instead of a ratio, because the baseline can be near zero:
BETTER if ZeroMesh's p99 increase is lower by at least 2 ms with the CI excluding 0, and EQUIVALENT if the CI of the
difference is inside ±2 ms. The builder may revise the 2 ms after measuring the probe's idle noise in the warm-up
block, but must do so **before** registration.

### 6.4 One allowed extension
If any metric is INCONCLUSIVE at n=20, the config may pre-declare **one** extension to n=40 (on by default). It is
applied to every metric, not just the inconclusive ones, and the Holm/CI analysis is rerun on all 40. No further
extensions. The report says whether the extension was used.

### 6.5 Headline verdict

**FASTER?** over the time metrics (W1, W2, W4 rerun and W5 makespan; W3 makespan as secondary, not scored here):
- `YES`: no correctness disqualification, at least one time metric BETTER, and none WORSE.
- `NO`: at least one time metric WORSE and none BETTER.
- `MIXED`: some BETTER, some WORSE. The report lists which, e.g. "faster on short tasks and churn, slower on long tasks".
- `TIE`: all EQUIVALENT.
- `INCONCLUSIVE`: otherwise.

**BETTER?** over all scored metrics (the time metrics, W3 bytes, W5 lost CPU-seconds, W6 owner impact):
- `YES`: no disqualification, BETTER on at least half of the scored metrics, and WORSE on none.
- Otherwise the same `NO` / `MIXED` / `TIE` / `INCONCLUSIVE` logic as above.

Each headline is printed with its scope line, e.g.
`FASTER? YES — 4 laptops (1 requester, 3 donors), 5 GHz Wi-Fi, HTCondor 24.x tuned, zeromesh@<sha>, n=20, config <hash>`.

## 7. Pre-registration mechanics

- `scripts/verdict/verdict.toml` holds the workloads, task counts and durations, n, the extension flag, margins,
  α, the kill mode, the settle time, the randomisation seed policy and the condor-tuned config path.
- `verdict register` hashes `verdict.toml` + `condor_tuned.conf` + the harness scripts and writes
  `registration.json` (hash, git commit, UTC time). The intent is that this file is **committed and pushed before
  the run**, which gives a public timestamp. The command prints the commit command; it doesn't push by itself.
- `verdict run` refuses to start if the current hash doesn't match a `registration.json`, unless given `--smoke`.
- `verdict report` recomputes the hash from the archived copies and prints `VALID` or `INVALID` next to the verdict.

## 8. Run budget and resumability

Rough estimate at the defaults: one block ≈ 3 systems × (W1 ~1–4 min + W2 ~4–8 min + W3 ~2–4 min + W4 ~2–4 min +
W5 ~3–6 min + W6 ~4 min) plus settle time ≈ 50–90 min. **20 blocks ≈ 17–30 hours**, 40 blocks double that. This is
an overnight-to-weekend job. The builder may shrink task counts and durations to bring a block under ~45 min, as
long as W1 stays short-task-dominated and W2 stays long-task-dominated, and the final numbers are registered before
running.

- Results append per run to `results.csv` together with the block id and order. `verdict run --resume DIR` skips
  completed (block, system, workload) cells.
- An interrupted run is discarded and repeated, never partially used.

## 9. Outputs

`~/.zmfield/results/verdict-<UTC>/`:
- `verdict.json`: headline answers, per-metric verdicts, effects, CIs, p-values, scope and validity.
- `report.md`: headline first, then the per-metric table, operational table, excluded blocks, disqualifications,
  the "not measured" list, and HTCondor features used or unavailable.
- `results.csv`: one row per (block, system, workload, step), with every raw metric and `ok`.
- `registration.json`, archived `verdict.toml` and `condor_tuned.conf`, `hostinfo.txt` per node
  (CPU, RAM, kernel, distro, power profile, NIC, link speed), `logs/`.
- Plots (per-metric distributions of r with the margin band drawn) are nice-to-have, not required.

## 10. How it gets built (decided)

These are my calls, made because the questions were skipped. Each is easy to reverse.

- **Extend `zmfield.sh`; don't start a new harness.** It already does setup, pool formation for both systems, the
  payload, cache-busting nonces, byte counting and kill-chaos. New subcommands: `verdict register|run|report`.
- **Statistics in Python** in the existing zmfield venv (`numpy`, `scipy`), in `scripts/verdict/analyze.py`.
  Bash stays the orchestrator.
- **The analysis gets known-answer unit tests** on synthetic CSVs: identical distributions → EQUIVALENT or
  INCONCLUSIVE, never BETTER; an injected 30% ZeroMesh speedup → BETTER; an injected wrong output → that system loses
  the block; a config edit after registration → INVALID. These run in `meson test` or as a pytest beside it.
- **Hardware:** the verdict is valid on 2+ machines, but the report labels the node count. 4 laptops (1 requester,
  3 donors), as in the blueprint, is the target configuration. With only 1 donor, W5 must run all-nodes (as w3 does
  now) and the report flags that.
- **The owner probe** is a small C program (or Python with `clock_nanosleep`, if its own jitter is measured and
  shown to be small enough) started in the donor owner's user slice.

## 11. Left to the builder

- Exact task counts and durations within the shapes in §4, fitted to the run budget, then registered.
- Bootstrap method (BCa or percentile).
- How to kill an HTCondor donor reliably in each install tier (package vs container) so W5 is a fair kill for both.
- Which HTCondor input-reuse features exist in the installed version and whether they work in this setup (§3.1).
- Whether cold start (§4) can be made repeatable enough to score.

## 12. Threats to validity (printed in the report)

- **Node count and network.** Results on 2–4 laptops on one Wi-Fi network don't automatically carry over to 20 laptops
  or wired LANs.
- **Workload choice.** Synthetic CPU loops, not real renders or builds. A follow-up run can add `zm-render` or
  `zm-cfd` as W7 under the same rules.
- **Configuration skill.** We know ZeroMesh better than HTCondor. Mitigated by §3.1 and by publishing the tuned config
  so HTCondor users can say what we missed. If someone shows a better config, the verdict is rerun with it.
- **Synthetic churn.** Process kills, not real departures.
- **Same-author bias.** Mitigated by pre-registration and raw-data publication, not eliminated.
