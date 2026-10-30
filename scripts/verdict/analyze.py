#!/usr/bin/env python3
"""Turn the verdict results CSV into verdict.json and report.md.

CSV schema (owned here; written by the field harness):
  block,order,workload,step,system,variant,wall_s,rx_mb,tx_mb,ok,total,retries,ttfr_s,
  reexec_task_s,probe_p99_ms,probe_idle_p99_ms,tasks_done,notes

  * block -1 is warm-up and ignored. ok == total means every output was correct.
  * A row with step == "excluded" and notes containing "excluded=<reason>" drops the cell
    (block, workload); workload "-" drops the whole block.
  * notes is a ";"-separated list of tokens. Recognised: excluded=<reason>, kill_after=<delay>,
    single_donor_all_nodes, condor_features=<tok>[,<tok>...] where <tok> is "name" or "used:name"
    (feature used) or "unavailable:name".

Rules applied here (spec sections 5 and 6):
  * Per block, a system whose workload has any row with ok < total (or no row at all) has failed that
    workload in that block, and loses every metric of the workload.
  * Per-block ratio r = zm/baseline (inverted for higher-is-better), sentinels from stats.ratio.
    w6_owner is a difference in ms (zm owner impact minus baseline owner impact), sentinels +/-1e6.
  * Wilcoxon p on log(r) (raw diffs for w6_owner), Holm across all seven scored metrics, percentile
    bootstrap seeded with [run].seed.
"""
import argparse
import csv
import json
import math
import re
import sys
from pathlib import Path

import numpy as np

import config as vconfig
import register
import stats

COLUMNS = ["block", "order", "workload", "step", "system", "variant", "wall_s", "rx_mb", "tx_mb", "ok",
           "total", "retries", "ttfr_s", "reexec_task_s", "probe_p99_ms", "probe_idle_p99_ms",
           "tasks_done", "notes"]
_INT = {"block", "order", "ok", "total", "retries", "tasks_done"}
_FLOAT = {"wall_s", "rx_mb", "tx_mb", "ttfr_s", "reexec_task_s", "probe_p99_ms", "probe_idle_p99_ms"}

ZM = "zm"
BASELINES = ("condor-default", "condor-tuned")
DIFF_SENTINEL_MS = 1e6


def _tasks_per_min(row):
    t, w = row["tasks_done"], row["wall_s"]
    return None if t is None or w is None or w <= 0 else t / (w / 60.0)


METRICS = [
    {"id": "w1_makespan", "workload": "w1", "step": "main", "field": "wall_s", "kind": "ratio"},
    {"id": "w2_makespan", "workload": "w2", "step": "main", "field": "wall_s", "kind": "ratio"},
    {"id": "w4_rerun", "workload": "w4", "step": "rerun", "field": "wall_s", "kind": "ratio"},
    {"id": "w5_makespan", "workload": "w5", "step": "main", "variant": "kill", "field": "wall_s",
     "kind": "ratio"},
    {"id": "w3_bytes", "workload": "w3", "step": "main", "field": "tx_mb", "kind": "ratio"},
    {"id": "w5_reexec", "workload": "w5", "step": "main", "variant": "kill", "field": "reexec_task_s",
     "kind": "ratio", "smooth_key": "reexec_smooth_s"},
    {"id": "w6_owner", "workload": "w6", "step": "main", "kind": "diff"},
]
FASTER_METRICS = {"w1_makespan", "w2_makespan", "w4_rerun", "w5_makespan"}

# Reported only: no p-value, no verdict, not in Holm, not in the headline.
SECONDARY = [
    {"id": "w3_makespan", "workload": "w3", "step": "main", "field": "wall_s", "kind": "ratio"},
    {"id": "w1_ttfr", "workload": "w1", "step": "main", "field": "ttfr_s", "kind": "ratio"},
    {"id": "w6_tasks_per_min", "workload": "w6", "step": "main", "fn": _tasks_per_min,
     "kind": "ratio", "higher_is_better": True},
]

NOT_MEASURED = [
    "Real lid close, suspend or unplug departures (W5 only kills processes).",
    "Wi-Fi vs wired, unless both were run.",
    "More than the tested number of nodes.",
]


# ---- loading ---------------------------------------------------------------------------------

def load(csv_path) -> list[dict]:
    with open(csv_path, newline="") as f:
        rd = csv.DictReader(f)
        if rd.fieldnames is None or list(rd.fieldnames) != COLUMNS:
            raise ValueError(f"{csv_path}: columns must be exactly {','.join(COLUMNS)}; "
                             f"got {','.join(rd.fieldnames or [])}")
        rows = []
        for i, raw in enumerate(rd, start=2):
            row = {}
            for c in COLUMNS:
                v = raw[c]
                if c == "notes" or c in ("workload", "step", "system", "variant"):
                    row[c] = v
                elif v == "":
                    row[c] = None
                else:
                    try:
                        row[c] = int(v) if c in _INT else float(v)
                    except ValueError:
                        raise ValueError(f"{csv_path}:{i}: bad {c}={v!r}") from None
            if row["block"] is None:
                raise ValueError(f"{csv_path}:{i}: empty block")
            rows.append(row)
    return rows


# ---- exclusions and per-cell state -----------------------------------------------------------

def _excl_reason(notes) -> str:
    m = re.search(r"excluded=([^;]*)", notes or "")
    return m.group(1).strip() if m else ""


def exclusions(rows) -> tuple[dict, dict]:
    """(whole blocks {block: reason}, cells {(block, workload): reason})."""
    blocks, cells = {}, {}
    for r in rows:
        if r["step"] != "excluded" or r["block"] < 0 or "excluded=" not in (r["notes"] or ""):
            continue
        reason = _excl_reason(r["notes"])
        if r["workload"] == "-":
            blocks[r["block"]] = reason
        else:
            cells[(r["block"], r["workload"])] = reason
    return blocks, cells


def _clean(rows) -> list[dict]:
    """Data rows only: no warm-up, no exclusion markers, no excluded blocks or cells."""
    xb, xc = exclusions(rows)
    return [r for r in rows
            if r["block"] >= 0 and r["step"] != "excluded"
            and r["block"] not in xb and (r["block"], r["workload"]) not in xc]


def _finite(v):
    return v is not None and math.isfinite(v)


def _workload_ok(clean, wl):
    """{(system, block): True if every row of that workload is correct}, plus {block set}."""
    state, blocks = {}, set()
    for r in clean:
        if r["workload"] != wl:
            continue
        blocks.add(r["block"])
        correct = r["ok"] is not None and r["ok"] == r["total"]
        key = (r["system"], r["block"])
        state[key] = state.get(key, True) and correct
    return state, blocks


def disqualified(rows, frac: float) -> dict[str, list[str]]:
    """system -> workloads where the system was wrong in more than frac of the valid blocks."""
    clean = _clean(rows)
    out: dict[str, list[str]] = {}
    for wl in sorted({r["workload"] for r in clean}):
        state, blocks = _workload_ok(clean, wl)
        for system in sorted({s for s, _ in state}):
            bad = sum(1 for b in blocks if not state.get((system, b), False))
            if bad > frac * len(blocks) + 1e-12:
                out.setdefault(system, []).append(wl)
    return out


# ---- metric extraction -----------------------------------------------------------------------

def _value(row, metric):
    if metric["kind"] == "diff":
        a, b = row["probe_p99_ms"], row["probe_idle_p99_ms"]
        v = a - b if _finite(a) and _finite(b) else None
    elif "fn" in metric:
        v = metric["fn"](row)
    else:
        v = row[metric["field"]]
        if _finite(v):
            v += metric.get("smooth", 0.0)
    return v if _finite(v) else None


def _cells(rows, metric):
    """(sorted valid blocks, {(system, block): (value|None, ok)}) for the metric."""
    clean = _clean(rows)
    state, _ = _workload_ok(clean, metric["workload"])
    want_variant = metric.get("variant")
    mine, seen, blocks = {}, set(), set()
    for r in clean:
        if r["workload"] != metric["workload"] or r["step"] != metric["step"]:
            continue
        if want_variant is not None and r["variant"] != want_variant:
            continue
        key = (r["system"], r["block"])
        if key in seen:
            raise ValueError(f"duplicate row for block={r['block']} workload={r['workload']} "
                             f"step={r['step']} variant={r['variant']!r} system={r['system']}")
        seen.add(key)
        blocks.add(r["block"])
        mine[key] = r
    cells = {}
    for b in blocks:
        for s in (ZM,) + BASELINES:
            r = mine.get((s, b))
            ok = r is not None and state.get((s, b), False)
            v = _value(r, metric) if ok else None
            if ok and v is None and not metric.get("secondary"):
                raise ValueError(f"block {b} {metric['workload']} {s}: no usable value for "
                                 f"{metric.get('field', metric['id'])} on a correct row")
            cells[(s, b)] = (v, ok)
    return sorted(blocks), cells


def _higher(metric) -> bool:
    return bool(metric.get("higher_is_better", False))


def best_baseline(rows, metric: dict) -> str:
    """Baseline with the better (lower, or higher if higher-is-better) median after failures are
    scored as the worst value. Ties go to condor-tuned."""
    blocks, cells = _cells(rows, metric)
    if not blocks:
        return "condor-tuned"
    if metric["kind"] == "diff":
        worst = DIFF_SENTINEL_MS
    else:
        worst = -math.inf if _higher(metric) else math.inf
    meds = {}
    for s in BASELINES:
        vals = [cells[(s, b)][0] if cells[(s, b)][1] and cells[(s, b)][0] is not None else worst
                for b in blocks]
        meds[s] = float(np.median(vals))
    if _higher(metric):
        return "condor-default" if meds["condor-default"] > meds["condor-tuned"] else "condor-tuned"
    return "condor-default" if meds["condor-default"] < meds["condor-tuned"] else "condor-tuned"


def block_ratios(rows, metric: dict, baseline: str) -> dict[int, float]:
    """Per valid block, zm vs the given baseline: a ratio (<1 good for zm), or for kind "diff" the
    difference in ms (<0 good) with sentinels +/-1e6."""
    blocks, cells = _cells(rows, metric)
    out = {}
    for b in blocks:
        (zv, zok), (bv, bok) = cells[(ZM, b)], cells[(baseline, b)]
        if metric.get("secondary") and ((zok and zv is None) or (bok and bv is None)):
            continue
        if metric["kind"] == "diff":
            if zok and bok:
                out[b] = zv - bv
            elif bok:
                out[b] = DIFF_SENTINEL_MS
            elif zok:
                out[b] = -DIFF_SENTINEL_MS
            else:
                out[b] = 0.0
            continue
        if zok and bok:
            if zv == 0 and bv == 0:
                out[b] = 1.0
                continue
            if zv <= 0 or bv <= 0:
                raise ValueError(f"block {b} {metric['id']}: non-positive value "
                                 f"(zm={zv}, {baseline}={bv}); ratio undefined")
        out[b] = stats.ratio(zv or 0.0, bv or 0.0, _higher(metric), zok, bok)
    return out


# ---- per-metric analysis ---------------------------------------------------------------------

def _resolve(metric, cfg):
    m = dict(metric)
    if "smooth_key" in m:
        m["smooth"] = float(cfg["run"][m["smooth_key"]])
    return m


def per_metric(rows, cfg: dict) -> list[dict]:
    run = cfg["run"]
    seed, n_boot = run["seed"], run["bootstrap"]
    work = []
    for metric in METRICS:
        m = _resolve(metric, cfg)
        base = best_baseline(rows, m)
        br = block_ratios(rows, m, base)
        arr = [br[b] for b in sorted(br)]
        if not arr:
            work.append((m, base, arr, 1.0))
            continue
        x = np.array(arr) if m["kind"] == "diff" else np.log(arr)
        work.append((m, base, arr, stats.wilcoxon_p(x)))
    p_adj = stats.holm([w[3] for w in work])
    out = []
    for (m, base, arr, p), pa in zip(work, p_adj):
        res = {"metric": m["id"], "kind": m["kind"], "baseline": base, "n": len(arr),
               "median": None, "ci_lo": None, "ci_hi": None, "p": p, "p_adj": pa,
               "verdict": "INCONCLUSIVE"}
        if arr:
            lo, hi = stats.bootstrap_ci(arr, n=n_boot, seed=seed)
            res.update(median=float(np.median(arr)), ci_lo=lo, ci_hi=hi)
            if m["kind"] == "diff":
                res["verdict"] = stats.diff_verdict(arr, pa, margin_ms=run["owner_margin_ms"],
                                                    seed=seed, n_boot=n_boot)
            else:
                res["verdict"] = stats.ratio_verdict(arr, pa, margin=run["ratio_margin"],
                                                     seed=seed, n_boot=n_boot)
        out.append(res)
    return out


def secondary(rows, cfg: dict) -> list[dict]:
    run = cfg["run"]
    out = []
    for metric in SECONDARY:
        m = dict(metric, secondary=True)
        base = best_baseline(rows, m)
        br = block_ratios(rows, m, base)
        arr = [br[b] for b in sorted(br)]
        res = {"metric": m["id"], "baseline": base, "n": len(arr),
               "median": None, "ci_lo": None, "ci_hi": None}
        if arr:
            lo, hi = stats.bootstrap_ci(arr, n=run["bootstrap"], seed=run["seed"])
            res.update(median=float(np.median(arr)), ci_lo=lo, ci_hi=hi)
        out.append(res)
    return out


# ---- headline --------------------------------------------------------------------------------

def _counts(verdicts):
    return {v: verdicts.count(v) for v in ("BETTER", "WORSE", "EQUIVALENT", "INCONCLUSIVE")}


def _call(c, n, yes) -> str:
    if yes:
        return "YES"
    if c["BETTER"] and c["WORSE"]:
        return "MIXED"
    if c["WORSE"] and not c["BETTER"]:
        return "NO"
    if n and c["EQUIVALENT"] == n:
        return "TIE"
    if not c["INCONCLUSIVE"] and n:
        return "NO"        # everything decided, but not enough BETTER and nothing WORSE
    return "INCONCLUSIVE"


def headline(results, dq) -> dict:
    if ZM in dq:
        txt = "DISQUALIFIED on " + ", ".join(sorted(dq[ZM]))
        return {"faster": txt, "better": txt,
                "why": "ZeroMesh gave wrong or missing output in more than the allowed share of blocks."}
    fv = [r["verdict"] for r in results if r["metric"] in FASTER_METRICS]
    av = [r["verdict"] for r in results]
    cf, ca = _counts(fv), _counts(av)
    faster = _call(cf, len(fv), cf["BETTER"] >= 1 and cf["WORSE"] == 0)
    better = _call(ca, len(av), ca["BETTER"] * 2 >= len(av) and len(av) > 0 and ca["WORSE"] == 0)
    why = (f"FASTER set ({len(fv)} metrics): {cf['BETTER']} better, {cf['WORSE']} worse, "
           f"{cf['EQUIVALENT']} equivalent, {cf['INCONCLUSIVE']} inconclusive. "
           f"All ({len(av)} metrics): {ca['BETTER']} better, {ca['WORSE']} worse, "
           f"{ca['EQUIVALENT']} equivalent, {ca['INCONCLUSIVE']} inconclusive.")
    return {"faster": faster, "better": better, "why": why}


def needs_extension(results) -> bool:
    return any(r["verdict"] == "INCONCLUSIVE" for r in results)


# ---- outputs ---------------------------------------------------------------------------------

def _scope_line(scope: dict) -> str:
    g = lambda k: scope.get(k, "?")  # noqa: E731
    return (f"{g('laptops')} laptops, {g('donors')} donors, {g('network')}, "
            f"HTCondor {g('condor_version')}, git {g('git_sha')}")


def _fmt(v, kind="ratio") -> str:
    if v is None:
        return "n/a"
    return f"{v:.2f} ms" if kind == "diff" else f"{v:.4g}"


def _note_values(rows, key) -> list[str]:
    vals = []
    for r in rows:
        for m in re.finditer(rf"(?:^|;){key}=([^;]*)", r["notes"] or ""):
            vals.append(m.group(1).strip())
    return vals


def _features(rows):
    used, unavailable = [], []
    for v in _note_values(rows, "condor_features"):
        for tok in re.split(r"[,\s]+", v):
            if not tok:
                continue
            kind, _, name = tok.partition(":")
            if not name:
                kind, name = "used", tok
            target = unavailable if kind == "unavailable" else used
            if name not in target:
                target.append(name)
    return used, unavailable


def _report(results, hl, scope, validity, smoke, ctx) -> str:
    rows = ctx.get("rows") or []
    L = []
    if validity == "INVALID":
        L.append("INVALID: config changed after registration")
    if smoke:
        L.append("SMOKE — no verdict")
    else:
        sl = _scope_line(scope)
        L.append(f"FASTER? {hl['faster']} — {sl}")
        L.append(f"BETTER? {hl['better']} — {sl}")
    L += ["", hl.get("why", ""), "", "## Per-metric results", "",
          "Effect is the median per-block ratio ZeroMesh/baseline (below 1 favours ZeroMesh); "
          "w6_owner is the median difference in ms (below 0 favours ZeroMesh). "
          "Bootstrap: percentile. p is Wilcoxon signed-rank, Holm-adjusted across all metrics.", "",
          "| Metric | Baseline | n | Effect (median) | 95% CI | p | p (Holm) | Verdict |",
          "|---|---|---|---|---|---|---|---|"]
    for r in results:
        k = r.get("kind", "ratio")
        L.append(f"| {r['metric']} | {r['baseline']} | {r['n']} | {_fmt(r['median'], k)} | "
                 f"[{_fmt(r['ci_lo'], k)}, {_fmt(r['ci_hi'], k)}] | {r['p']:.3g} | {r['p_adj']:.3g} | "
                 f"{r['verdict']} |")
    sec = ctx.get("secondary") or []
    if sec:
        L += ["", "## Secondary metrics (reported only, no verdict)", "",
              "| Metric | Baseline | n | Median ratio | 95% CI |", "|---|---|---|---|---|"]
        for s in sec:
            L.append(f"| {s['metric']} | {s['baseline']} | {s['n']} | {_fmt(s['median'])} | "
                     f"[{_fmt(s['ci_lo'])}, {_fmt(s['ci_hi'])}] |")
    xb, xc = exclusions(rows)
    L += ["", "## Excluded blocks and cells", ""]
    if not xb and not xc:
        L.append("None.")
    L += [f"- block {b}: {why}" for b, why in sorted(xb.items())]
    L += [f"- block {b}, {wl}: {why}" for (b, wl), why in sorted(xc.items())]
    dq = ctx.get("dq") or {}
    L += ["", "## Disqualifications", ""]
    if not dq:
        L.append("None.")
    L += [f"- {s}: {', '.join(w)}" for s, w in sorted(dq.items())]
    L += ["", "## Not measured", ""] + [f"- {t}" for t in NOT_MEASURED]
    used, unavailable = _features(rows)
    L += ["", "## HTCondor features", "",
          f"- Used: {', '.join(used) if used else 'none recorded'}",
          f"- Unavailable: {', '.join(unavailable) if unavailable else 'none recorded'}"]
    flags = []
    if any("single_donor_all_nodes" in (r["notes"] or "") for r in rows):
        flags.append("W5: every node was the single donor, so the kill removed the only donor.")
    kills = sorted(set(_note_values(rows, "kill_after")))
    if kills:
        flags.append(f"W5 kill delay: {', '.join(kills)} after start.")
    if flags:
        L += ["", "## W5 notes", ""] + [f"- {f}" for f in flags]
    return "\n".join(L) + "\n"


def write_outputs(results, headline, scope: dict, validity: str, outdir: Path, smoke: bool,
                  *, ctx: dict | None = None) -> None:
    """ctx (optional): rows, dq, secondary, seed, n_boot -- richer report and verdict.json."""
    ctx = ctx or {}
    outdir = Path(outdir)
    outdir.mkdir(parents=True, exist_ok=True)
    hl = headline
    if smoke:
        hl = {"faster": "SMOKE — no verdict", "better": "SMOKE — no verdict", "why": hl.get("why", "")}
    doc = {"validity": validity, "smoke": smoke, "headline": hl, "scope": scope, "metrics": results,
           "secondary": ctx.get("secondary", []), "disqualified": ctx.get("dq", {}),
           "needs_extension": needs_extension(results),
           "bootstrap": "percentile", "seed": ctx.get("seed"), "n_boot": ctx.get("n_boot")}
    (outdir / "verdict.json").write_text(json.dumps(doc, indent=2, sort_keys=True) + "\n")
    (outdir / "report.md").write_text(_report(results, headline, scope, validity, smoke, ctx))


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("csv")
    ap.add_argument("--config", required=True)
    ap.add_argument("--registration", required=True)
    ap.add_argument("--root", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--smoke", action="store_true")
    a = ap.parse_args(argv)

    cfg = vconfig.load(a.config)
    rows = load(a.csv)
    out = Path(a.out)
    try:
        valid = register.check(Path(a.root), Path(a.registration))
    except Exception:
        valid = False
    scope_path = out / "scope.json"
    scope = json.loads(scope_path.read_text()) if scope_path.exists() else {}

    results = per_metric(rows, cfg)
    dq = disqualified(rows, cfg["run"]["disqualify_frac"])
    hl = headline(results, dq)
    ctx = {"rows": rows, "dq": dq, "secondary": secondary(rows, cfg),
           "seed": cfg["run"]["seed"], "n_boot": cfg["run"]["bootstrap"]}
    write_outputs(results, hl, scope, "VALID" if valid else "INVALID", out, a.smoke, ctx=ctx)
    if needs_extension(results) and not a.smoke:
        print("NEEDS_EXTENSION")
    return 0


if __name__ == "__main__":
    sys.exit(main())
