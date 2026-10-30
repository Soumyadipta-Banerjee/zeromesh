# ZeroMesh vs HTCondor Verdict Suite Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** One command, `bash scripts/zmfield.sh verdict`, that runs pre-registered, paired, randomised, correctness-gated workloads W1-W6 for `zm` vs `condor-default`/`condor-tuned` on real laptops and emits `verdict.json` + `report.md`.

**Architecture:** Bash orchestration (`scripts/verdict/verdict.sh`, sourced by `zmfield.sh`) drives the existing `exec_system` machinery and appends a richer CSV; Python (`config.py`, `register.py`, `stats.py`, `analyze.py`) handles config, pre-registration hashing and statistics, covered by pytest known-answer tests. A fake backend (`ZMV_FAKE=1`) makes orchestration testable without a pool.

**Tech Stack:** bash >= 4, Python >= 3.11 (`tomllib`), numpy, scipy, pytest (installed into the zmfield venv `$ZMF_HOME/venv`), static C (`burn`, `probe`), meson `test()`.

**Spec:** `docs/superpowers/specs/2026-10-30-zeromesh-vs-htcondor-verdict-design.md`

**Execution notes:** Tasks run as Sonnet subagents (subagent-driven-development). Tasks 1-4 have no pool dependency. Tasks 5, 7, 8, 9 rely on `ZMV_FAKE=1` for tests. Python commands below use `python3`; use the zmfield venv's python if system python lacks numpy/scipy/pytest. Run pytest from the repo root. Commit messages carry NO `Co-Authored-By` trailer. Never run parallel ninja/meson builds.

## Global Constraints

- n = 20 blocks; one optional extension to n = 40 (on by default, applied to all metrics, once); 1 discarded warm-up block (block id -1); settle 15 s between runs.
- alpha = 0.05, Holm across all scored metrics; bootstrap 10,000 resamples, percentile method (decided), fixed seed recorded (`seed = 20261030`).
- Ratio verdicts: BETTER needs p_adj < 0.05 AND CI upper < 1.0 AND median r <= 0.95; WORSE needs p_adj < 0.05 AND CI lower > 1.0 AND median r >= 1.05; EQUIVALENT if CI inside [0.95, 1.05]; else INCONCLUSIVE. r < 1 always means ZeroMesh better.
- W6 absolute margin 2 ms: BETTER = zm lower by >= 2 ms median and CI of difference excludes 0 on the good side; EQUIVALENT = CI inside +/-2 ms.
- Disqualify a system on a workload if correctness fails in > 10% of blocks. A wrong/missing output makes that system lose every metric of that block's workload.
- Smoke mode: n = 3, never emits a verdict, prints `SMOKE — no verdict`.
- Remote-only default for W1-W4 and W6; W5 all-nodes. Per-metric baseline = better (lower median) of `condor-default` / `condor-tuned`.
- Unique nonce per (block, workload, system) so no cross-block cache hit; W4 is the only workload where cache reuse is allowed.
- Report prints scope next to every verdict: `<N> laptops (1 requester, <D> donors), <network>, HTCondor <ver> tuned, zeromesh@<sha>, n=<n>, config <hash8>`.
- Tuned HTCondor knobs must be checked against the docs of the installed version (`condor_version`); unknown knobs deleted.
- `zmfield.sh bench` / `report` must keep working unchanged (11-column CSV).

## Review Focus

1. Only two laptops (one donor): W5 must run all-nodes and the report must flag it (Task 8, `test_single_donor_flagged`).
2. Donor does not come back after a kill: the rest of that block's W5 cells are excluded, never silently counted as a fast/slow run (Task 8, `test_donor_not_back_excludes_rest_of_block`).
3. Resume after a config edit: refuse with the registration error even with `--resume` (Task 5, `test_resume_refuses_after_edit`).
4. Both systems fail correctness in the same block: ratio 1.0, but both count toward disqualification (Task 3, `test_both_fail_counts_for_both`).
5. Stale ZeroMesh cache hit across blocks: nonce includes block id; in W4 a stale hit on the changed case is a correctness failure (Task 5 `test_nonce_unique_per_block_and_system`, Task 7 `test_w4_changed_case_has_new_digest_reference`).

## File Structure

| Path | Responsibility |
|---|---|
| `scripts/verdict/verdict.toml` | Pre-registered config |
| `scripts/verdict/config.py` | Load toml, shell vars, seeded run order |
| `scripts/verdict/register.py` | Hash tracked files, `registration.json`, check |
| `scripts/verdict/stats.py` | Pure stats primitives |
| `scripts/verdict/analyze.py` | CSV to `verdict.json` + `report.md` |
| `scripts/verdict/verdict.sh` | Orchestration (sourced by zmfield.sh) |
| `scripts/verdict/probe.c` | Owner wake-latency probe |
| `scripts/verdict/tests/` | pytest: `conftest.py`, `test_*.py` |
| `scripts/zmfield.sh` | Modified: `verdict`, `condor-mode`, `probe`, `hostinfo` subcommands; burn v2; `emit_row`; `count_ok` |
| `scripts/baselines/condor_tuned.conf` | Modified: extra knobs + version header |

---

### Task 1: Config and pre-registration

**Files:**
- Create: `scripts/verdict/verdict.toml`, `scripts/verdict/config.py`, `scripts/verdict/register.py`
- Create: `scripts/verdict/tests/conftest.py` (inserts `scripts/verdict` on `sys.path`; fixture `repo` returning the repo root `Path`), `tests/test_config.py`, `tests/test_register.py` (all under `scripts/verdict/tests/`)
- Modify: `meson.build` (append test entry after `integration_cfd`), `scripts/zmfield.sh` (in `build_zm`, after the `if [ ! -d "$VENV" ]` block: `run "$VENV/bin/pip" install --quiet numpy scipy pytest`)

**Interfaces:**
- Produces:
  - `config.load(path: str|Path) -> dict`
  - `config.shell_vars(cfg: dict) -> str` (lines `VD_<SECTION>_<KEY>=<value>`, upper-case, strings single-quoted)
  - `config.run_order(seed: int, block: int, systems: list[str]) -> list[str]` (`random.Random(seed*1000+block).shuffle` on a copy)
  - CLI `python3 config.py shell FILE`, `python3 config.py order SEED BLOCK sys1 sys2 ...` (space-separated)
  - `register.HASHED: list[str]` (the 8 paths from the spec, in this order: `scripts/verdict/verdict.toml`, `scripts/baselines/condor_tuned.conf`, `scripts/zmfield.sh`, `scripts/verdict/verdict.sh`, `scripts/verdict/analyze.py`, `scripts/verdict/stats.py`, `scripts/verdict/config.py`, `scripts/verdict/probe.c`)
  - `register.compute_hash(root: Path, files: list[str] = HASHED) -> str` (sha256 over `relpath + b"\0" + bytes` in order; missing file contributes bytes `b"MISSING"`)
  - `register.write_registration(root: Path, out: Path) -> dict` (keys `hash`, `git_sha`, `utc`, `files`; `git_sha` is `"?"` when not a git repo)
  - `register.check(root: Path, registration: Path) -> bool`
  - CLI `register.py write ROOT OUT`, `register.py check ROOT REG` (exit 0 prints `VALID`, exit 1 prints `INVALID`)

`verdict.toml` content (task counts carry the comment `# task counts may be shrunk to fit the run budget BEFORE 'verdict register'`):

```toml
[run]    blocks=20 extend_to=40 warmup_blocks=1 settle_s=15 seed=20261030 alpha=0.05
         ratio_margin=0.05 owner_margin_ms=2.0 disqualify_frac=0.10 bootstrap=10000
         reexec_smooth_s=1.0   # added by this plan: avoids 0/0 on W5 re-executed CPU-seconds
[w1] tasks=200 task_secs=1      [w2] tasks=24 task_secs=60      [w3] tasks=48 task_secs=5 input_mb=256
[w4] changed_case="003"         [w5] tasks=60 task_secs=10 kill_frac=0.30 kill_mode="kill"
[w6] tasks=24 task_secs=30 probe_period_ms=100
[condor] tuned_conf="scripts/baselines/condor_tuned.conf"
```
(one key per line in the real file; the above is compressed).

- [ ] **Step 1: Write failing tests**

```python
# test_register.py (uses tmp_path copy of the 8 hashed files, created by a helper)
def test_hash_stable(tmp_tree):            assert compute_hash(tmp_tree) == compute_hash(tmp_tree)
def test_hash_changes_on_toml_edit(tmp_tree):
    h = compute_hash(tmp_tree); (tmp_tree/"scripts/verdict/verdict.toml").write_text("x=1\n"); assert compute_hash(tmp_tree) != h
def test_check_detects_edit_after_registration(tmp_tree, tmp_path):
    reg = tmp_path/"r.json"; write_registration(tmp_tree, reg); assert check(tmp_tree, reg) is True
    (tmp_tree/"scripts/verdict/verdict.toml").write_text("x=1\n"); assert check(tmp_tree, reg) is False
# test_config.py
def test_shell_vars_format(repo):   assert "VD_RUN_BLOCKS=20" in shell_vars(load(repo/"scripts/verdict/verdict.toml")).splitlines()
def test_run_order_deterministic_and_varies():
    s = ["zm","condor-default","condor-tuned"]
    assert run_order(1,3,s) == run_order(1,3,s)
    assert len({tuple(run_order(1,b,s)) for b in range(1,21)}) > 1
    assert sorted(run_order(1,3,s)) == sorted(s)
```

`tmp_tree` fixture (in `test_register.py`): copies `verdict.toml` and `condor_tuned.conf` from `repo`, writes dummy content for the rest, leaves `probe.c` absent (exercises MISSING).

- [ ] **Step 2: Run to verify failure**

Run: `python3 -m pytest -q scripts/verdict/tests/test_register.py scripts/verdict/tests/test_config.py`
Expected: FAIL (ImportError: no module `register`/`config`).

- [ ] **Step 3: Implement** `verdict.toml`, `config.py`, `register.py` per the interface block. `utc` is `datetime.now(timezone.utc).isoformat()`; `files` maps relpath to per-file sha256.

- [ ] **Step 4: Run to verify pass**

Run: `python3 -m pytest -q scripts/verdict/tests/test_register.py scripts/verdict/tests/test_config.py`
Expected: `5 passed`. Also `python3 scripts/verdict/config.py shell scripts/verdict/verdict.toml | head -3` prints `VD_...` lines.

- [ ] **Step 5: Meson entry and commit**

Add to `meson.build`:
```meson
test('verdict_analysis', python3,
  args: ['-m', 'pytest', '-q', meson.project_source_root() / 'scripts/verdict/tests'],
  timeout: 300,
  is_parallel: false,
)
```
Run: `meson test -C build verdict_analysis --print-errorlogs` (requires pytest/numpy/scipy for the meson python; if `python3` in meson is the venv, `setup` installs them). Expected: OK.
```bash
git add scripts/verdict meson.build scripts/zmfield.sh
git commit -m "feat(verdict): pre-registered config, hashing and run-order primitives"
```

---

### Task 2: Stats primitives

**Files:**
- Create: `scripts/verdict/stats.py`, `scripts/verdict/tests/test_stats.py`

**Interfaces:**
- Produces (numpy/scipy only):
  - `ratio(zm: float, base: float, higher_is_better: bool, zm_ok: bool, base_ok: bool) -> float` (zm fail & base ok -> `1e9`; base fail & zm ok -> `1e-9`; both fail -> `1.0`; else `zm/base`, or `base/zm` if higher_is_better)
  - `bootstrap_ci(x, stat=np.median, n=10000, seed=0, level=0.95) -> tuple[float, float]` (percentile method over resampled blocks)
  - `wilcoxon_p(x) -> float` (two-sided `scipy.stats.wilcoxon`; all-zero input returns `1.0`; callers pass log ratios or diffs)
  - `holm(pvals: list[float]) -> list[float]` (step-down adjusted, monotone, capped at 1, returned in input order)
  - `ratio_verdict(ratios, p_adj: float, margin: float = 0.05, seed: int = 0, n_boot: int = 10000) -> str`
  - `diff_verdict(diffs_ms, p_adj: float, margin_ms: float = 2.0, seed: int = 0, n_boot: int = 10000) -> str`
  - Both verdict functions return one of `"BETTER","WORSE","EQUIVALENT","INCONCLUSIVE"`, computing the median and bootstrap CI internally; BETTER/WORSE tested before EQUIVALENT. For diffs, "good side" is negative (zm lower).

- [ ] **Step 1: Write failing tests** (`np.random.default_rng(7)`)

```python
def test_identical_never_better():   r = rng.lognormal(0,0.02,20); assert ratio_verdict(r, wilcoxon_p(np.log(r))) in ("EQUIVALENT","INCONCLUSIVE")
def test_30pct_speedup_better():     r = 0.70*rng.lognormal(0,0.05,20); assert ratio_verdict(r, 1e-4) == "BETTER"
def test_30pct_slowdown_worse():     r = (1/0.70)*rng.lognormal(0,0.05,20); assert ratio_verdict(r, 1e-4) == "WORSE"
def test_noisy_inconclusive():       r = rng.lognormal(0,0.5,20); assert ratio_verdict(r, wilcoxon_p(np.log(r))) == "INCONCLUSIVE"
def test_holm_known_values():        assert holm([0.01,0.04,0.03]) == pytest.approx([0.03,0.06,0.06])
def test_ratio_failure_sentinels():
    assert ratio(5,10,False,False,True) == 1e9 and ratio(5,10,False,True,False) == 1e-9
    assert ratio(5,10,False,False,False) == 1.0 and ratio(5,10,False,True,True) == 0.5
    assert ratio(5,10,True,True,True) == 2.0
def test_diff_verdict_margin():
    assert diff_verdict(rng.normal(-5,0.5,20), 1e-4) == "BETTER"
    assert diff_verdict(rng.normal(0,0.3,20), 0.5) == "EQUIVALENT"
```

- [ ] **Step 2: Run to verify failure**

Run: `python3 -m pytest -q scripts/verdict/tests/test_stats.py`
Expected: FAIL (ImportError: `stats`).

- [ ] **Step 3: Implement** the six functions in `stats.py`. Holm: sort ascending, `adj_i = max over j<=i of (m-j)*p_(j)`, cap 1, unsort.

- [ ] **Step 4: Run to verify pass**

Run: `python3 -m pytest -q scripts/verdict/tests/test_stats.py`
Expected: `7 passed`. If `test_noisy_inconclusive` is flaky with the chosen seed, fix the seed in the test, never loosen the rule.

- [ ] **Step 5: Commit**
```bash
git add scripts/verdict/stats.py scripts/verdict/tests/test_stats.py
git commit -m "feat(verdict): stats primitives with known-answer tests"
```

---

### Task 3: Analysis and verdict

**Files:**
- Create: `scripts/verdict/analyze.py`, `scripts/verdict/tests/test_analyze.py`
- Modify: `scripts/verdict/tests/conftest.py` (add synthetic-data helpers)

**Interfaces:**
- Consumes: `stats.*` (Task 2), `config.load`, `register.check` (Task 1).
- CSV schema (owned here, written by Tasks 5-9): `block,order,workload,step,system,variant,wall_s,rx_mb,tx_mb,ok,total,retries,ttfr_s,reexec_task_s,probe_p99_ms,probe_idle_p99_ms,tasks_done,notes`. `block = -1` is warm-up (ignored). `ok == total` means correct. A row with `step == excluded` and `notes` containing `excluded=<reason>` drops cell `(block, workload)`; `workload == "-"` drops the whole block.
- Produces in `analyze.py`:
  - `METRICS: list[dict]` with ids and sources: `w1_makespan` (w1/main/wall_s), `w2_makespan` (w2/main/wall_s), `w4_rerun` (w4/rerun/wall_s), `w5_makespan` (w5/main variant `kill`/wall_s), `w3_bytes` (w3/main/tx_mb), `w5_reexec` (w5/main variant `kill`/reexec_task_s, both values `+ reexec_smooth_s`), `w6_owner` (w6/main, value `probe_p99_ms - probe_idle_p99_ms`, `kind="diff"`). Secondary, reported only: w3 makespan, w1 `ttfr_s`, w6 tasks/min (`tasks_done / (wall_s/60)`).
  - `FASTER_METRICS = {"w1_makespan","w2_makespan","w4_rerun","w5_makespan"}`
  - `load(csv: Path) -> list[dict]`
  - `best_baseline(rows, metric: dict) -> str` (`"condor-default"|"condor-tuned"`, lower median over valid blocks after the failure sentinel)
  - `per_metric(rows, cfg: dict) -> list[dict]` (keys `metric, baseline, n, median, ci_lo, ci_hi, p, p_adj, verdict`; Holm across all 7; for `w6_owner` the per-block statistic is a diff in ms with failure sentinels +/-1e6)
  - `disqualified(rows, frac: float) -> dict[str, list[str]]` (system -> workloads where `ok<total` in more than `frac` of valid blocks; every system counted independently)
  - `headline(results, dq) -> {"faster": str, "better": str, "why": str}`: FASTER over `FASTER_METRICS`: YES if >=1 BETTER, 0 WORSE, zm not disqualified; NO if >=1 WORSE and 0 BETTER; MIXED if both; TIE if all EQUIVALENT; else INCONCLUSIVE. BETTER over all 7: YES if zm not disqualified, BETTER on >= half, WORSE on none; NO/MIXED/TIE/INCONCLUSIVE as above. If zm is disqualified anywhere: both headlines `DISQUALIFIED on <wl>`. Baseline disqualification is only printed, it does not change the headline.
  - `needs_extension(results) -> bool` (any INCONCLUSIVE)
  - `write_outputs(results, headline, scope: dict, validity: str, outdir: Path, smoke: bool) -> None`
  - CLI: `analyze.py CSV --config TOML --registration REG --root ROOT --out DIR [--smoke]` (scope read from `DIR/scope.json` with keys `laptops, donors, network, condor_version, git_sha`; missing keys print `?`). Exit 0 always on successful analysis; prints `NEEDS_EXTENSION` on stdout when `needs_extension` and not `--smoke`, else nothing extra.
- `report.md` layout: line 1 `INVALID: config changed after registration` only when `validity == "INVALID"`; then `FASTER? <X> — <scope line>` and `BETTER? <X> — <scope line>` (smoke: both replaced by `SMOKE — no verdict`); per-metric table; excluded blocks/cells; disqualifications; a "Not measured" list copying spec section 4 (real lid close/suspend/unplug, Wi-Fi vs wired unless both run, more than the tested node count); HTCondor features used/unavailable parsed from `condor_features=` in `notes`; one-line W5 flag when `notes` contains `single_donor_all_nodes`; kill delay if `notes` has `kill_after=`.
- conftest helpers: `make_rows(blocks: int, wall: dict[tuple[str,str], float], noise: float = 0.02, seed: int = 0, bad: frozenset = frozenset()) -> list[dict]` (`wall[(workload, system)]`; `bad` holds `(block, workload, system)` that get `ok = total - 1`) and `write_csv(rows, path)`.

- [ ] **Step 1: Write failing tests**

```python
def test_zm_wrong_output_loses_block():      # bad={(3,"w1","zm")}; per-block ratio for block 3 == 1e9 (via an exposed block_ratios(rows, metric, baseline))
def test_dq_over_10pct():                    # 3/20 w1 blocks wrong for zm -> headline["faster"] == headline["better"] == "DISQUALIFIED on w1"; 2/20 -> not disqualified
def test_best_baseline_picks_lower_median(): # condor-tuned faster on w1, condor-default faster on w2 -> different baselines per metric
def test_headline_yes():   # w1 BETTER, others EQUIVALENT -> faster "YES"; better "NO" unless >= half BETTER (assert "INCONCLUSIVE"-free)
def test_headline_mixed(): # one BETTER one WORSE in FASTER set -> "MIXED"
def test_headline_tie():   # all EQUIVALENT -> "TIE"
def test_headline_inconclusive(): # BETTER none, WORSE none, one INCONCLUSIVE -> "INCONCLUSIVE"
def test_warmup_ignored():  # block -1 rows with wall 9999 do not change median
def test_smoke_no_verdict(tmp_path):  # smoke=True -> report contains "SMOKE — no verdict" and not "FASTER? YES"
def test_invalid_registration_banner(tmp_path):  # CLI after editing verdict.toml in a tmp root -> report.md line 1 == "INVALID: config changed after registration"
def test_both_fail_counts_for_both():
    # zm and condor-tuned both wrong in the same 3/20 w1 blocks: ratios for those blocks == 1.0
    # and disqualified(rows, 0.10) == {"zm": ["w1"], "condor-tuned": ["w1"]}
def test_excluded_cell_dropped():  # excluded row for (4,"w5") removes block 4 from w5 metrics only; n decreases by 1
```
The headline tests build `results` dicts directly (`{"metric": "w1_makespan", "verdict": "BETTER"}`), no CSV.

- [ ] **Step 2: Run to verify failure**

Run: `python3 -m pytest -q scripts/verdict/tests/test_analyze.py`
Expected: FAIL (ImportError: `analyze`).

- [ ] **Step 3: Implement** `analyze.py` and conftest helpers. Expose `block_ratios(rows, metric, baseline) -> dict[int, float]` for the first test. Per-block pairing uses the chosen baseline only; a baseline failure in that block uses the sentinel. Percentile bootstrap seeded with `cfg["run"]["seed"]`; record `bootstrap: "percentile"` and the seed in `verdict.json`.

- [ ] **Step 4: Run to verify pass**

Run: `python3 -m pytest -q scripts/verdict/tests/test_analyze.py`
Expected: `12 passed`.

- [ ] **Step 5: Commit**
```bash
git add scripts/verdict/analyze.py scripts/verdict/tests
git commit -m "feat(verdict): analysis, headline rules and report writer"
```

---

### Task 4: Burn v2 with verifiable outputs

**Files:**
- Modify: `scripts/zmfield.sh` — embedded C in `build_burn` (~746-794), `exec_system` ok-counting (~924-928), `condor_submit_file` (~845-850)
- Create: `scripts/verdict/tests/test_burn.py`

Problem being fixed: today `CASE.out` is `ok cpu <x> host`, independent of the case file and nonce, and `exec_system` only greps `^ok`, so stale cache hits and wrong outputs go undetected.

**Interfaces:**
- Produces C modes of `$ZMF_HOME/burn`:
  - `burn cpu ITERS CASE NONCE` and `burn sum FILE CASE NONCE` write `CASE.out` as `ok <kind> <digest16hex> <host>`; digest = FNV-1a-64 (offset `1469598103934665603`, prime `1099511628211`) over: 8-byte little-endian raw value, then bytes of the CASE file, then bytes of NONCE; printed `%016llx`. `sum` keeps the basename fallback for FILE.
  - `burn rawcpu ITERS` and `burn rawsum FILE` print the raw value in decimal.
  - `burn ref RAW CASE NONCE` prints the digest hex without doing the work.
- Produces bash:
  - `payload_kind WL -> stdout cpu|sum`: `$PAYLOAD_FORCE` if set, else `sum` for `w2`, `cpu` otherwise (keeps bench behaviour; verdict W2 is a CPU workload, so verdict.sh sets `PAYLOAD_FORCE` per workload: w1,w2,w5,w6 `cpu`; w3,w4 `sum`). `exec_system` and `condor_submit_file` use it instead of `[ "$wl" = w2 ]`.
  - `expected_digest RAW CASEPATH NONCE -> stdout hex` (calls `$ZMF_HOME/burn ref`).
  - `count_ok WD PREFIX NAMESFILE RAW NONCE -> stdout integer`: for each name `c` in NAMESFILE, counts 1 if `$WD/$PREFIX$c.out` has `$3 == expected_digest RAW $WD/cases/$c NONCE` and `$1 == ok`. `exec_system` uses it (replacing the `grep -q '^ok'` loop); total = line count of NAMESFILE.
  - RAW is computed once per stage: cpu -> `burn rawcpu $ITERS`, cached in `$RUNROOT/raw.$ITERS`; sum -> `burn rawsum $stage/data/big.bin`, cached in `$RUNROOT/raw.sum.$(basename "$stage")`.

- [ ] **Step 1: Write failing tests** (`test_burn.py`; fixture builds `burn` once per session with `subprocess.run(["bash","-c","source scripts/zmfield.sh; build_burn"], env={**os.environ,"ZMF_HOME":str(tmp)}, cwd=repo)`)

```python
def digest(case_text, nonce, iters=1000):   # runs burn cpu, returns field 3 of CASE.out
def test_digest_depends_on_case_content():  assert digest("case 001\n","n1") != digest("case 002\n","n1")
def test_digest_depends_on_nonce():         assert digest("case 001\n","n1") != digest("case 001\n","n2")
def test_ref_matches_real_run():            # RAW=burn rawcpu 1000; burn ref RAW CASE NONCE == digest(...)
def test_exec_counts_wrong_digest_as_fail():
    # wd/cases/000 + wd/cases/000.out containing "ok cpu 0000000000000000 host"; names file "000"
    # bash -c 'source scripts/zmfield.sh; count_ok WD cases/ NAMES RAW NONCE' -> "0"; with the correct digest line -> "1"
def test_bench_still_parses():              # bash -n scripts/zmfield.sh and `bash scripts/zmfield.sh --dry-run bench` exit 0
```

- [ ] **Step 2: Run to verify failure**

Run: `python3 -m pytest -q scripts/verdict/tests/test_burn.py`
Expected: FAIL (`burn ref` unknown mode / `count_ok: command not found`).

- [ ] **Step 3: Implement** the C modes and bash helpers per the interface block. Argument parsing replaces the old `[NONCE]` optional forms: `cpu` and `sum` now require NONCE (argc >= 5).

- [ ] **Step 4: Run to verify pass**

Run: `python3 -m pytest -q scripts/verdict/tests/test_burn.py && bash -n scripts/zmfield.sh`
Expected: `5 passed`, no syntax output.

- [ ] **Step 5: Commit**
```bash
git add scripts/zmfield.sh scripts/verdict/tests/test_burn.py
git commit -m "feat(zmfield): burn v2 digests tied to case content and nonce; count_ok verifies outputs"
```

---

### Task 5: Orchestration skeleton, W1, W2, fake backend

**Files:**
- Create: `scripts/verdict/verdict.sh`, `scripts/verdict/tests/test_orchestration.py`
- Modify: `scripts/zmfield.sh` — `main` dispatch `verdict) source "$ROOT/scripts/verdict/verdict.sh"; cmd_verdict "$@"`, `usage`, and `exec_system` row printing (~939-944)

**Interfaces:**
- Consumes: `config.py shell|order` CLI, `register.py write|check` CLI, `analyze.py` CLI, `count_ok`/`payload_kind` (Task 4).
- Produces in zmfield.sh: `emit_row WL STEP SYS VARIANT WALL RX TX OK TOTAL RETRIES NOTE` — default prints the legacy 11-column row to `$CSV`; if shell function `VD_ROW_HOOK` is set to a function name, calls it with the same args instead. `exec_system` calls only `emit_row`.
- Produces in verdict.sh:
  - `cmd_verdict register|run|report`
  - `vd_nonce BLOCK WL SYS -> "$RUNID-b$BLOCK-$WL-$SYS"`
  - `vd_row WL STEP SYS VARIANT WALL RX TX OK TOTAL RETRIES NOTE` (installed as `VD_ROW_HOOK`): prefixes `$VD_BLOCK,$VD_ORDER` and fills `ttfr_s,reexec_task_s,probe_p99_ms,probe_idle_p99_ms,tasks_done` from globals `VD_X_TTFR VD_X_REEXEC VD_X_PROBE VD_X_PROBE_IDLE VD_X_TASKS` (blank when empty; cleared after each row) in the Task 3 column order
  - `vd_exec SYS WL STEP VARIANT STAGEDIR` (real: `exec_system` with `vd_nonce` nonce; fake: writes a synthetic row)
  - `vd_sleep SECS` (no-op when `ZMV_FAKE=1`)
  - `cell_done BLOCK WL STEP SYS VARIANT -> 0|1` (greps `$CSV`)
  - `precheck_block -> 0|1`
  - `vd_calibrate WL` (one `calibrate` per session; ITERS for other task_secs scales linearly: `ITERS = ITERS_ref * task_secs / SECS_ref`)
- Behaviour of `cmd_verdict run --ssh USER@DONOR [--peer IP] [--smoke] [--resume DIR] [--network LABEL]`:
  - No `--ssh`: exit 2, message `verdict needs --ssh for kills and the owner probe`.
  - Unless `--smoke`: `register.py check` against `scripts/verdict/registration.json` (also when `--resume` is given); failure: exit 2, message `config changed since registration`.
  - `eval "$(python3 config.py shell verdict.toml)"`; smoke overrides `VD_RUN_BLOCKS=3`, `VD_RUN_EXTEND_TO=0`.
  - `RES=$ZMF_HOME/results/verdict-<UTC>` (or the `--resume` dir); copies `registration.json`, `verdict.toml`, `condor_tuned.conf` into `RES`, and every `register.HASHED` file into `$RES/registered_tree/<relpath>` (archive for `report`); writes `$RES/scope.json`; writes the CSV header once.
  - Block loop: warm-up block `-1`, then `1..N`. Per block, workloads in order `w1 w2 w3(+w4) w6 w5` (w5 last so a lost donor only endangers its own cell); per workload, system order from `config.py order $VD_RUN_SEED $block zm condor-default condor-tuned`; `zm-local` runs only in the warm-up as reference. `vd_sleep $VD_RUN_SETTLE_S` between runs. Before each block call `precheck_block`.
  - After N blocks run `analyze.py`; if its stdout has `NEEDS_EXTENSION` and not yet extended and `VD_RUN_EXTEND_TO > N`, run blocks `N+1..EXTEND_TO`, re-analyze.
  - W1: `stage_cpu` with `VD_W1_TASKS`, ITERS calibrated to `VD_W1_TASK_SECS`, step `main`, variant `none`, `PAYLOAD_FORCE=cpu`. W2: same shape with `VD_W2_*`.
- `precheck_block`: AC online on requester (`/sys/class/power_supply/*/online` any `1`, or no battery) and on donor via ssh; 1-minute loadavg < 1.0 on both; `admitted_peers >= 1` and `condor_machines | wc -l >= 2`. On failure wait 60 s (`vd_sleep`) and retry once; still failing: append a row `workload=-`, `step=excluded`, `notes=excluded=<reason>` and skip the block. Fake mode: fails only when `ZMV_FAKE_PRECHECK_FAIL` equals the current block id.
- Fake backend (`ZMV_FAKE=1`): `vd_exec` appends a row with `wall_s = $ZMV_FAKE_WALL_<SYS>` (`SYS` upper-cased, `-` to `_`, default `10`), `tx_mb = ${ZMV_FAKE_TX:-1.0}`, `ok = total = <tasks>`, `rx_mb=0`, `retries=0`; calibration, pool checks, ssh and sleeps are stubbed. Test environment: copy `scripts/` into a tmp tree used as ROOT (git sha falls back to `?`), set `ZMF_HOME` to a tmp dir, run `bash <tmp>/scripts/zmfield.sh verdict run ...`.

- [ ] **Step 1: Write failing tests** (`test_orchestration.py`; helper `run_verdict(args, env, tmp_root) -> CompletedProcess` and `read_csv(res_dir)`)

```python
def test_refuses_without_registration():   # no --smoke, no registration.json -> returncode 2, "config changed since registration" in stderr
def test_requires_ssh():                   # missing --ssh -> returncode 2, "verdict needs --ssh" in stderr
def test_smoke_runs_3_blocks_plus_warmup():
    # --smoke --ssh fake@x : CSV blocks for workload w1 and w2 == {-1,1,2,3}; 3 systems per (block>=1, wl); report.md starts with "SMOKE — no verdict"
def test_resume_skips_done_cells():
    # pre-seed CSV with block 1 w1 rows for all systems, rerun with --resume DIR -> no duplicate (block,wl,step,system,variant) keys
def test_resume_refuses_after_edit():      # register, edit verdict.toml, run --resume DIR (no --smoke) -> returncode 2, "config changed since registration"
def test_nonce_unique_per_block_and_system():
    # bash -c 'source ...verdict.sh; RUNID=R; vd_nonce 1 w1 zm; vd_nonce 2 w1 zm; vd_nonce 1 w1 condor-tuned' -> 3 distinct strings; first == "R-b1-w1-zm"
def test_order_matches_config_py():        # system order of block 2 w1 rows in CSV (by `order` column) == `config.py order SEED 2 zm condor-default condor-tuned`
def test_precheck_failure_excludes_block():# ZMV_FAKE_PRECHECK_FAIL=2 -> a row workload "-" with "excluded=" in notes for block 2; no other rows for block 2
def test_register_prints_commit_command():  # `verdict register` stdout contains "git add scripts/verdict/registration.json && git commit -m \"verdict: register "
```

- [ ] **Step 2: Run to verify failure**

Run: `python3 -m pytest -q scripts/verdict/tests/test_orchestration.py`
Expected: FAIL (`unknown command: verdict`).

- [ ] **Step 3: Implement** `emit_row` refactor in `exec_system`, `verdict.sh`, dispatch and usage text. `cmd_verdict register` runs `register.py write`, then prints (never runs) `git add scripts/verdict/registration.json && git commit -m "verdict: register <hash8>" && git push`. `cmd_verdict report DIR` runs `analyze.py` with `--root $DIR/registered_tree --registration $DIR/registration.json`, so validity is recomputed from the archived copies.

- [ ] **Step 4: Run to verify pass**

Run: `python3 -m pytest -q scripts/verdict/tests/test_orchestration.py && bash -n scripts/zmfield.sh scripts/verdict/verdict.sh`
Expected: `9 passed`, no syntax output.

- [ ] **Step 5: Commit**
```bash
git add scripts/verdict scripts/zmfield.sh
git commit -m "feat(verdict): orchestration skeleton with W1/W2, resume, precheck and fake backend"
```

---

### Task 6: Tuned HTCondor on every node, plus input-reuse check

**Files:**
- Modify: `scripts/baselines/condor_tuned.conf`, `scripts/zmfield.sh` (`condor_set_mode` ~581-603, `condor_submit_file` ~860, `main` dispatch, new `hostinfo` subcommand), `scripts/verdict/verdict.sh`
- Create: `scripts/verdict/tests/test_condor_conf.py`

**Interfaces:**
- Produces:
  - `zmfield.sh condor-mode default|tuned` (donor-side; calls `condor_set_mode`).
  - `condor_set_mode MODE`: after the local switch, when `SSH_TARGET` is non-empty also runs `run ssh -o BatchMode=yes "$SSH_TARGET" "bash $SSH_SCRIPT condor-mode $mode"`; the log line no longer says "requester-side knobs only".
  - `condor_submit_file` adds `max_retries = 3` in tuned mode (next to `job_lease_duration = 60`).
  - `zmfield.sh hostinfo` prints the `write_hostinfo` body to stdout (adds `power_profile:`, `link_speed:` from `/sys/class/net/$IFACE/speed` and `powerprofilesctl get` when present); verdict.sh stores requester and donor output as `$RES/hostinfo-requester.txt`, `$RES/hostinfo-donor.txt`, each followed by `condor_config_val NEGOTIATOR_INTERVAL NEGOTIATOR_CYCLE_DELAY SCHEDD_INTERVAL UPDATE_INTERVAL CLAIM_WORKLIFE JOB_RENICE_INCREMENT` effective values (read in tuned mode).
  - verdict.sh start-up check: `git rev-parse HEAD` on the donor over ssh must equal the requester's, else exit 2 `donor checkout differs`. Skipped under `ZMV_FAKE=1`.
  - `notes` field `condor_features=<name>` or `condor_features=none(<reason>)` on every condor row (global `VD_CONDOR_FEATURES`, appended in `vd_row` for `condor-*` systems).

- [ ] **Step 1: Write failing tests** (`test_condor_conf.py`)

```python
def test_submit_tuned_has_max_retries(tmp_path):
    # bash -c 'source scripts/zmfield.sh; CUR_ALL=1; ITERS=1; condor_cmd(){ :; }; mkdir -p $WD/cases; condor_submit_file $WD w1 N tuned'
    # (CUR_ALL=1 skips the requirements line; stub condor_cmd) -> job.sub contains "max_retries = 3" and "job_lease_duration = 60";
    # same call with mode "default" contains neither
def test_set_mode_pushes_to_donor_in_dry_run():
    # bash scripts/zmfield.sh --dry-run ... not available for set_mode alone, so:
    # bash -c 'source scripts/zmfield.sh; DRY=1; SSH_TARGET=u@d; condor_tier(){ echo native; }; condor_set_mode tuned' -> stdout+stderr contain "condor-mode tuned"
def test_tuned_conf_has_version_header(repo):
    # the first non-empty lines of condor_tuned.conf are comments and contain "checked against HTCondor"
```

- [ ] **Step 2: Run to verify failure**

Run: `python3 -m pytest -q scripts/verdict/tests/test_condor_conf.py`
Expected: FAIL (no `max_retries`, no `condor-mode tuned` output, no header).

- [ ] **Step 3: Implement** the zmfield.sh and verdict.sh changes, then the config work:
  1. Add to `condor_tuned.conf`: `CLAIM_WORKLIFE = 3600`, `JOB_RENICE_INCREMENT = 10`; keep the existing 5/2/5/10 intervals and `JobLeaseDuration = 60`.
  2. Run `condor_version` on the dev laptop; check each knob in the manual for that exact version (`https://htcondor.readthedocs.io/en/<ver>/` configuration macros / submit description pages). Delete any knob that does not exist there.
  3. Write a comment block at the top of the file: `# checked against HTCondor <version> on <date>`, knobs confirmed, knobs deleted, and whether an input-reuse / common-file-transfer feature exists in that version.
  4. If such a feature exists and works rootless/containerised in this setup, add it to `condor_submit_file` for w3/w4 in tuned mode and set `VD_CONDOR_FEATURES=<name>`; else `VD_CONDOR_FEATURES=none(<reason>)`. Mark in the header which knobs are doc-verified and which are not (only after the doc check).
  5. Add the `condor-mode` and `hostinfo` dispatch entries to `main` and `usage`.

- [ ] **Step 4: Run to verify pass**

Run: `python3 -m pytest -q scripts/verdict/tests/test_condor_conf.py scripts/verdict/tests/test_orchestration.py && bash -n scripts/zmfield.sh`
Expected: all pass.

- [ ] **Step 5: Commit**
```bash
git add scripts/baselines/condor_tuned.conf scripts/zmfield.sh scripts/verdict
git commit -m "feat(verdict): tuned HTCondor on every node, max_retries, doc-checked knob header"
```

---

### Task 7: W3 shared input and W4 incremental rerun

**Files:**
- Modify: `scripts/verdict/verdict.sh`
- Create: `scripts/verdict/tests/test_w3w4.py`

**Interfaces:**
- Consumes: `stage_data`, `vd_exec`, `vd_nonce`, `vd_row` globals (`VD_X_TASKS`), `payload_kind`/`count_ok`/`expected_digest` (Task 4).
- Produces: `vd_w3w4 BLOCK SYS` in verdict.sh. Steps:
  1. W3: `stage_data` with `VD_W3_TASKS` cases and `VD_W3_INPUT_MB` (payload `sum`, sized by MB; the 5 s per-task target is not enforced, the actual wall is recorded). `vd_exec SYS w3 main none`. Primary metric `tx_mb` (requester upload).
  2. W4: copy the stage, append `changed` to `cases/$VD_W4_CHANGED_CASE`, `vd_exec SYS w4 rerun none` with the SAME nonce as the W3 run (`vd_nonce BLOCK w3 SYS`); cache reuse is allowed and expected. Correctness for W4 counts the changed case only if its digest matches the reference for the NEW content, so a stale cache hit fails.
  3. `VD_X_TASKS` (column `tasks_done`) = tasks zm actually executed, parsed from `zm run` stderr (the builder inspects real `zm run --each` output for the cached/executed marker and records the marker string in a comment); HTCondor = total.
  - Resume: if the `w3 main` row exists but the `w4 rerun` row does not, delete the `w3` row for that cell and redo both (`cell_drop BLOCK WL STEP SYS VARIANT`).

- [ ] **Step 1: Write failing tests**

```python
def test_w3_rows_have_tx_mb():              # fake smoke: every w3/main row has a numeric tx_mb; w4 rows have step "rerun"
def test_w4_uses_same_nonce_as_w3():        # real-burn-free: bash -c sources verdict.sh with a stubbed exec_system that logs its nonce arg; w3 and w4 calls for (block 1, zm) log identical nonces
def test_w4_changed_case_has_new_digest_reference():
    # real burn (build_burn in tmp ZMF_HOME), 4 cases, stage_data 1 MB; run `burn sum` for all cases, then append "changed" to case 003 and keep the OLD 003.out:
    # count_ok on the changed stage returns 3 of 4 (stale digest for 003 is rejected); regenerating 003.out makes it 4
```

- [ ] **Step 2: Run to verify failure**

Run: `python3 -m pytest -q scripts/verdict/tests/test_w3w4.py`
Expected: FAIL (`vd_w3w4: command not found` / no w3 rows).

- [ ] **Step 3: Implement** `vd_w3w4` and `cell_drop`; wire into the block loop in place of the Task 5 placeholder (`w3` and `w4` rows are written consecutively per system).

- [ ] **Step 4: Run to verify pass**

Run: `python3 -m pytest -q scripts/verdict/tests/test_w3w4.py scripts/verdict/tests/test_orchestration.py`
Expected: all pass.

- [ ] **Step 5: Commit**
```bash
git add scripts/verdict
git commit -m "feat(verdict): W3 shared input and W4 incremental rerun with stale-hit detection"
```

---

### Task 8: W5 churn

**Files:**
- Modify: `scripts/verdict/verdict.sh`
- Create: `scripts/verdict/tests/test_w5.py`

**Interfaces:**
- Produces:
  - `vd_kill_delay -> stdout seconds`: reads `$RES/kill_delay`; in the warm-up block (`-1`) first runs a zm no-kill control (`vd_exec zm w5 main none`) on the W5 stage, writes `delay = VD_W5_KILL_FRAC * wall` (format `%.1f`) to `$RES/kill_delay`, and every later block reuses it. Row notes carry `kill_after=<delay>s`.
  - `vd_w5 BLOCK SYS`: `CUR_ALL=1; LOCAL_ONLY=0`; start `vd_exec SYS w5 main kill` in the background; `vd_sleep delay`; `chaos_trigger <zm|condor> $VD_W5_KILL_MODE`; wait; `chaos_trigger <kind> restore`; `wait_pool_ready`. Cells use `PAYLOAD_FORCE=cpu`, `VD_W5_TASKS` x `VD_W5_TASK_SECS`.
  - `wait_pool_ready` (existing) must return non-zero when the donor is not back after 3 minutes (modify to `return 1` after the warn paths); on non-zero, `vd_w5` writes rows `workload=w5, step=excluded, notes=excluded=donor_not_back` for every remaining w5 system in this block and for the one just run, and returns. Analysis drops `(block, w5)` entirely (Task 3).
  - `reexec_from_condor_log JOBLOG TASKS TASK_SECS -> stdout "%.1f"` = `(count of lines starting "001 (" - TASKS) * TASK_SECS`, floored at 0. For zm, `VD_X_REEXEC = retries * VD_W5_TASK_SECS`, `retries` from the existing `retry N/3` grep.
  - Single donor: when `admitted_peers == 1` (fake: `ZMV_FAKE_PEERS`, default `1`), every w5 row's notes include `single_donor_all_nodes`.
  - Fake backend extras: `ZMV_FAKE_POOL_BACK=0` makes `wait_pool_ready` fail; `chaos_trigger` is a no-op.

- [ ] **Step 1: Write failing tests**

```python
def test_kill_delay_fixed_across_blocks():   # fake smoke, ZMV_FAKE_WALL_ZM=10 -> $RES/kill_delay == "3.0" (written once); all w5/kill rows share notes "kill_after=3.0s"
def test_donor_not_back_excludes_rest_of_block():
    # ZMV_FAKE_POOL_BACK=0 smoke: block 1 has no normal w5 rows after the first system; has excluded rows with "excluded=donor_not_back";
    # analyze on this CSV does not count block 1 toward w5 n
def test_single_donor_flagged():             # default fake peers=1 -> every w5 row notes contain "single_donor_all_nodes"; report.md contains "single_donor_all_nodes" or its human sentence
def test_reexec_from_condor_log(tmp_path):
    # fixture job.log with 62 lines "001 (123.000.000) ... Job executing on host" + other event lines, 60 tasks, 10 s
    # bash -c 'source verdict.sh; reexec_from_condor_log LOG 60 10' -> "20.0"
```

- [ ] **Step 2: Run to verify failure**

Run: `python3 -m pytest -q scripts/verdict/tests/test_w5.py`
Expected: FAIL (`vd_w5: command not found`).

- [ ] **Step 3: Implement** the functions above and the report sentence for `single_donor_all_nodes` (Task 3 `write_outputs` already reads it; verify the wording: "W5 ran on all nodes with a single donor: the requester finished the work after the kill").

- [ ] **Step 4: Run to verify pass**

Run: `python3 -m pytest -q scripts/verdict/tests/test_w5.py scripts/verdict/tests/test_orchestration.py scripts/verdict/tests/test_analyze.py`
Expected: all pass.

- [ ] **Step 5: Commit**
```bash
git add scripts/verdict scripts/zmfield.sh
git commit -m "feat(verdict): W5 churn with fixed kill delay, re-execution accounting and donor-return exclusion"
```

---

### Task 9: Owner probe and W6

**Files:**
- Create: `scripts/verdict/probe.c`, `scripts/verdict/tests/test_probe.py`
- Modify: `scripts/zmfield.sh` (`build_probe`, donor-side `probe` subcommand, usage), `scripts/verdict/verdict.sh`

**Interfaces:**
- Produces:
  - `probe SECS OUT` (C, static): loops `clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME)` with a 100 ms period, records lateness in microseconds, and at the end writes `OUT` with lines `samples N`, `p50_ms X`, `p99_ms Y` (floats, `%.3f`).
  - `build_probe` in zmfield.sh: compiles `scripts/verdict/probe.c` with `cc -O1 -static` into `$ZMF_HOME/probe` (same error message style as `build_burn`).
  - `zmfield.sh probe SECS OUT`: builds if missing, runs in the invoking user's session (the owner slice; no `systemd-run`, no cgroup move).
  - `vd_w6 BLOCK SYS`: (1) idle probe 20 s on the donor over ssh (`ssh $SSH_TARGET "bash $SSH_SCRIPT probe 20 /tmp/zmv-idle"`, then `ssh ... cat`), set `VD_X_PROBE_IDLE` = idle `p99_ms`; (2) start the W6 load in the background (`vd_exec SYS w6 main none`, `VD_W6_TASKS` x `VD_W6_TASK_SECS`, remote-only, `PAYLOAD_FORCE=cpu`) and simultaneously run the donor probe for `PROBE_SECS = min(60, floor(0.8 * VD_W6_TASKS * VD_W6_TASK_SECS / donor_cores))`; `VD_X_PROBE` = loaded `p99_ms`. Secondary `tasks/min` is computed in analysis from `tasks_done` and `wall_s`; set `VD_X_TASKS` to the number of tasks completed.
  - Fake backend: `vd_probe` returns `p99_ms = ${ZMV_FAKE_PROBE_P99:-1.0}` (loaded) and `${ZMV_FAKE_PROBE_IDLE:-0.5}` (idle).

- [ ] **Step 1: Write failing tests**

```python
def test_probe_output_format(tmp_path):      # build_probe, run `probe 1 OUT`: file has keys samples, p50_ms, p99_ms; 8 <= samples <= 11
def test_probe_p99_ge_p50(tmp_path):         # run `probe 2 OUT`: p99_ms >= p50_ms >= 0
def test_w6_row_has_both_probe_fields():     # fake smoke: every w6 row has non-blank probe_p99_ms and probe_idle_p99_ms; csv header contains both columns
```

- [ ] **Step 2: Run to verify failure**

Run: `python3 -m pytest -q scripts/verdict/tests/test_probe.py`
Expected: FAIL (`build_probe: command not found`).

- [ ] **Step 3: Implement** `probe.c`, `build_probe`, the `probe` subcommand and `vd_w6`; wire `vd_w6` into the block loop (before `w5`). p99 index: `sorted[ceil(0.99*N)-1]`.

- [ ] **Step 4: Run to verify pass**

Run: `python3 -m pytest -q scripts/verdict/tests/test_probe.py scripts/verdict/tests/test_orchestration.py && bash -n scripts/zmfield.sh scripts/verdict/verdict.sh`
Expected: all pass.

- [ ] **Step 5: Commit**
```bash
git add scripts/verdict scripts/zmfield.sh
git commit -m "feat(verdict): owner wake-latency probe and W6 owner-impact workload"
```

---

### Task 10: Docs and end-to-end smoke

**Files:**
- Modify: `scripts/ZMFIELD.md` (new section "Verdict run" before "Verified vs not")

**Interfaces:** Consumes everything above; produces the operator documentation.

- [ ] **Step 1: Write the "Verdict run" section** covering, in order: requirements (2+ laptops, `--ssh USER@DONOR` with passwordless sudo on the donor for condor kills, AC power on both, same checkout/commit on both); steps `bash scripts/zmfield.sh verdict register` -> run the printed `git add ... && git commit ... && git push` -> `bash scripts/zmfield.sh verdict run --ssh USER@DONOR --peer DONOR_IP --network "5 GHz Wi-Fi"` (overnight; budget from spec section 8: roughly 50-90 min per block, so 17-30 h at n = 20, double with the extension; shrink task counts in `verdict.toml` BEFORE `register` if needed) -> `bash scripts/zmfield.sh verdict report DIR`; `--resume DIR`; `--smoke`; where outputs land (`~/.zmfield/results/verdict-<UTC>/`).

- [ ] **Step 2: Verify the whole suite**

Run: `python3 -m pytest -q scripts/verdict/tests` — Expected: all pass (no failures, no skips except documented cc-less environments).
Run: `meson test -C build verdict_analysis --print-errorlogs` — Expected: `verdict_analysis OK`.
Run: `bash -n scripts/zmfield.sh scripts/verdict/verdict.sh` — Expected: no output.
Run: `ZMV_FAKE=1 ZMF_HOME=$(mktemp -d) bash scripts/zmfield.sh verdict run --smoke --ssh fake@x` then `head -1 $ZMF_HOME/results/verdict-*/report.md` — Expected: `SMOKE — no verdict`.
Run: `bash scripts/zmfield.sh --dry-run bench` — Expected: unchanged bench plan, exit 0.

- [ ] **Step 3: Add the honesty list** to the new section, in the style of the existing "Verified vs not" in `ZMFIELD.md`. "Verified" lists only what Step 2 proved (statistics known answers, hashing/registration, fake-backend orchestration, burn digests, probe format). "Not verified until two real laptops run it": the real `zm run` cached/executed marker parse (W4 `tasks_done`); HTCondor knob effects and any input-reuse feature on the installed version; condor kill and restore on both install tiers; ssh-driven probe and `chaos` timing; precheck against real `/sys/class/power_supply`; that 15 s settle is enough for the pool to quiesce; per-block runtime versus the 45 min target; remote-only slot counts for W2 (fewer slots than tasks is not enforced by the harness); and the probe's own jitter on a loaded donor.

- [ ] **Step 4: Commit**
```bash
git add scripts/ZMFIELD.md
git commit -m "docs(verdict): operator guide and verified/unverified list for the verdict suite"
```
