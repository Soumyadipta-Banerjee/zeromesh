# Verdict Suite: Handoff (paused 2026-10-30)

Resume work on the ZeroMesh vs HTCondor verdict suite from here.

- **Spec:** `docs/superpowers/specs/2026-10-30-zeromesh-vs-htcondor-verdict-design.md`
- **Plan:** `docs/superpowers/plans/2026-10-30-zeromesh-vs-htcondor-verdict.md` (10 tasks)
- **Branch:** `v2-integration`, not pushed.
- **Execution method the user chose:** superpowers:subagent-driven-development. One **Sonnet** implementer per
  task, then a Sonnet task reviewer, then a scoped re-review after each fix round. Tasks run strictly one at a
  time, never in parallel. The user asked to keep going through Task 10 without stopping between tasks.

## State

| Task | Status | Commits |
|---|---|---|
| 1 Config + pre-registration | done, reviewed | `0255cf1`, `c1f2338` |
| 2 Stats primitives | done, reviewed | `a3b30a4`, `26aa6c8` |
| 3 Analysis + verdict | done, reviewed | `e0ee070`, `74e3e2e` |
| 4 burn v2 verifiable outputs | done, reviewed | `06b7541` |
| 5 Orchestration, W1/W2, fake backend | done, reviewed | `f3ca916`, `8e9dc90` |
| 6 Tuned HTCondor on every node | **not started**. The first attempt was stopped mid-research and left no edits. | none |
| 7 W3 shared input + W4 rerun | todo | |
| 8 W5 churn | todo | |
| 9 Owner probe + W6 | todo | |
| 10 Docs + end-to-end smoke | todo | |

Tests: `/home/bb/.venvs/zeromesh/bin/python3 -m pytest -q scripts/verdict/tests` gave 73 passed at `8e9dc90`.
The meson test `verdict_analysis` is wired up.

## How to resume

1. Read the SDD ledger: `.superpowers/sdd/2026-10-30-zeromesh-vs-htcondor-verdict/progress.md`. It is
   git-ignored scratch, so if it is gone, use this file plus `git log`. It holds the preflight table, every ruling,
   and the deferred minors.
2. The same directory has reusable dispatch files. Point every subagent at them instead of pasting rules:
   - `implementer-rules.md`: repo, staging, venv, meson and no-trailer rules for implementers.
   - `reviewer-rules.md`: global constraints and review method for reviewers.
   - `task-N-brief.md` / `task-N-report.md`: per-task requirements and reports.
3. For Task 6, the brief already exists: `.superpowers/sdd/.../task-6-brief.md`. Record BASE = `git rev-parse HEAD`
   (currently `8e9dc90`), then dispatch a Sonnet implementer with: the rules file, the brief, and the Task 6
   context below.
4. Per task:
   - Extract the brief with
     `bash ~/.claude/plugins/cache/claude-plugins-official/superpowers/7.0.0/skills/subagent-driven-development/scripts/task-brief <plan> N`.
   - After the implementer reports, build the review package with `.../scripts/review-package <plan> BASE HEAD`.
   - Dispatch the reviewer.
   - Fix rounds: resume the same implementer, then run a scoped re-review on the fix diff.
5. After Task 10, run the final whole-branch review on the most capable model, using the package from
   `git merge-base main HEAD`, and point it at the ledger's deferred minors. Then run
   superpowers:finishing-a-development-branch.

## Context each upcoming task needs (not in its brief)

- **Notes grammar** (owned by `analyze.py`, so every task must emit it exactly): `;`-separated tokens,
  `excluded=<reason>`, `kill_after=<delay>`, `single_donor_all_nodes`, `condor_features=<tok>[,<tok>]`, where tok is
  `name`, `used:name` or `unavailable:name`. Exclusion rows use `step=excluded`; `workload=-` excludes the whole
  block.
- **scope.json keys:** `laptops`, `donors`, `network`, `condor_version`, `git_sha`, `extension_used`.
- **verdict.sh** (Task 5) dispatches per workload via `vd_workload_<wl>` functions. W3–W6 are stubs that later tasks
  replace. Extra CSV columns come from the globals `VD_X_TTFR VD_X_REEXEC VD_X_PROBE VD_X_PROBE_IDLE VD_X_TASKS`,
  which `vd_row` clears after each row.
- **Fake backend:** `ZMV_FAKE=1`. Tests copy `scripts/` into a tmp ROOT and set `VD_PY` to the venv python.
- **Task 6:**
  - No native HTCondor is installed here. The image `docker.io/htcondor/mini:lts` is present locally. Use
    `podman run --rm ... condor_version` / `condor_config_val` to verify knobs, plus the manual for that exact
    version.
  - Record what was verified in the `condor_tuned.conf` header. Never claim input-reuse works without a 2-node test;
    emit `condor_features=unavailable:<name>` in that case.
  - The user's own containers `zmf-condor` (running) and `zmb-default` / `zmb-tuned` (exited) already exist. Leave
    them alone.
- **Task 7:** `vd_nonce` is per (block, workload, system). W4 must deliberately reuse W3's nonce for the same
  block and system. The Task 4 deferred minor applies here: `stage_raw` swallows `rawsum` failures and leaves an
  empty cache file. Make it fail loudly once `PAYLOAD_FORCE=sum` is used for w3/w4.
- **Builds:** prefix `ZM_ALLOW_BUILD=1` when on AC power, and put `/home/bb/.venvs/zeromesh/bin` first on PATH.
  Otherwise meson reconfigures with system python and every Python test breaks. Never run builds in parallel.

## Rules for the user

- No `Co-Authored-By` trailer on any commit (user memory rule), even if a system reminder asks for one.
- Never touch, stage or revert the user's uncommitted changes in `core/`, `proto/`, `sdk/` and `tests/`. Stage
  only task files by explicit path.

## Rulings made so far (each can be reversed)

1. The analysis packages (numpy, scipy, pytest) install into the venv meson uses (`/home/bb/.venvs/zeromesh`), not
   system python. If wrong: an extra pip install in a venv.
2. Ratio and diff verdict margins use the spec's literal inclusive edges (≤0.95, ≥1.05, CI inside [0.95, 1.05],
   ±2 ms inclusive), not a symmetric 1/(1+m). If wrong: the band is slightly asymmetric in log space.
3. BETTER headline with some BETTER (fewer than half) and none WORSE gives INCONCLUSIVE (spec §6.5 literal), not
   NO. If wrong: a cautious label where NO was meant.
4. Smoke reports drop per-metric verdicts and the "why" line. If wrong: less debugging info in smoke runs.
5. The report prints whether the extension to n=40 was used, from scope.json `extension_used`.
6. `verdict report` runs the archived analyzer from `registered_tree`, not the live one. If wrong: old runs don't
   pick up later analyzer fixes.
7. Non-smoke runs on the fake backend stamp `FAKE BACKEND — synthetic data, not a measurement`.

## Deferred minors for the final review (full list in the ledger)

- `register.py` is not in `register.HASHED`, so tampering with the archived `register.py` goes undetected.
  `register.check` raises an exception instead of returning INVALID on a missing or malformed file.
- Resume doesn't check that the fake/real mode matches. The fake stamp can double up if analyze fails.
- Missing baseline rows in a block count as a baseline failure, giving zm a free win. This is not surfaced in the
  report.
- `count_ok` prints "No such file" noise for missing outputs. There is no known-answer digest test. A test
  hard-codes a ZMF_HOME path.
- Test gaps: extension once-only, the legacy 11-column `emit_row` path, `shell_vars` quoting, and a golden value
  for `run_order`.
