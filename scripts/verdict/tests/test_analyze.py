import json
import subprocess
import sys

import pytest

import analyze
import register
from analyze import (METRICS, FASTER_METRICS, best_baseline, block_ratios, disqualified, headline,
                     load, needs_extension, per_metric, secondary, write_outputs)
from conftest import make_row, make_rows, write_csv

ZM, CD, CT = "zm", "condor-default", "condor-tuned"
M = {m["id"]: m for m in METRICS}


@pytest.fixture
def cfg(repo):
    import config
    c = config.load(repo / "scripts/verdict/verdict.toml")
    c["run"]["bootstrap"] = 500          # keep tests fast; determinism is what matters
    return c


def w1(zm=8.0, cd=12.0, ct=10.0):
    return {("w1", ZM): zm, ("w1", CD): cd, ("w1", CT): ct}


def test_metrics_table():
    assert [m["id"] for m in METRICS] == ["w1_makespan", "w2_makespan", "w4_rerun", "w5_makespan",
                                          "w3_bytes", "w5_reexec", "w6_owner"]
    assert FASTER_METRICS == {"w1_makespan", "w2_makespan", "w4_rerun", "w5_makespan"}
    assert M["w6_owner"]["kind"] == "diff"


def test_zm_wrong_output_loses_block():
    rows = make_rows(20, w1(), bad=frozenset({(3, "w1", ZM)}))
    r = block_ratios(rows, M["w1_makespan"], CT)
    assert r[3] == 1e9
    assert len(r) == 20
    assert all(0.7 < v < 0.9 for b, v in r.items() if b != 3)


def test_baseline_wrong_output_uses_tiny_sentinel():
    rows = make_rows(5, w1(), bad=frozenset({(2, "w1", CT)}))
    assert block_ratios(rows, M["w1_makespan"], CT)[2] == 1e-9


def test_dq_over_10pct():
    bad3 = frozenset({(b, "w1", ZM) for b in (1, 5, 9)})
    rows = make_rows(20, w1(), bad=bad3)
    dq = disqualified(rows, 0.10)
    assert dq == {ZM: ["w1"]}
    hl = headline(per_metric(rows, _cfg_for(rows)), dq)
    assert hl["faster"] == hl["better"] == "DISQUALIFIED on w1"
    bad2 = frozenset({(b, "w1", ZM) for b in (1, 5)})
    rows = make_rows(20, w1(), bad=bad2)
    assert disqualified(rows, 0.10) == {}
    assert headline([{"metric": "w1_makespan", "verdict": "BETTER"}], {})["faster"] == "YES"


def _cfg_for(rows):
    import config
    from pathlib import Path
    c = config.load(Path(analyze.__file__).parent / "verdict.toml")
    c["run"]["bootstrap"] = 200
    return c


def test_best_baseline_picks_lower_median():
    wall = {("w1", ZM): 8.0, ("w1", CD): 12.0, ("w1", CT): 10.0,
            ("w2", ZM): 8.0, ("w2", CD): 10.0, ("w2", CT): 12.0}
    rows = make_rows(20, wall)
    assert best_baseline(rows, M["w1_makespan"]) == CT
    assert best_baseline(rows, M["w2_makespan"]) == CD


def test_best_baseline_failures_count_as_infinity():
    # tuned is faster when it works, but wrong in 12/20 blocks -> default is the better baseline
    bad = frozenset({(b, "w1", CT) for b in range(12)})
    rows = make_rows(20, w1(), bad=bad)
    assert best_baseline(rows, M["w1_makespan"]) == CD


def test_headline_yes():
    res = [{"metric": "w1_makespan", "verdict": "BETTER"}] + [
        {"metric": m, "verdict": "EQUIVALENT"} for m in
        ["w2_makespan", "w4_rerun", "w5_makespan", "w3_bytes", "w5_reexec", "w6_owner"]]
    hl = headline(res, {})
    assert hl["faster"] == "YES"
    assert hl["better"] == "NO"          # 1 of 7 BETTER is under half, nothing inconclusive
    assert "INCONCLUSIVE" not in (hl["faster"], hl["better"])
    res[1:4] = [{"metric": m, "verdict": "BETTER"} for m in ["w2_makespan", "w4_rerun", "w5_makespan"]]
    assert headline(res, {})["better"] == "YES"      # 4 of 7 >= half


def test_headline_better_yes_tolerates_inconclusive_if_half_better():
    res = [{"metric": m, "verdict": "BETTER"} for m in
           ["w1_makespan", "w2_makespan", "w4_rerun", "w5_makespan"]] + [
        {"metric": m, "verdict": "INCONCLUSIVE"} for m in ["w3_bytes", "w5_reexec", "w6_owner"]]
    assert headline(res, {})["better"] == "YES"


def test_headline_mixed():
    res = [{"metric": "w1_makespan", "verdict": "BETTER"}, {"metric": "w2_makespan", "verdict": "WORSE"}]
    hl = headline(res, {})
    assert hl["faster"] == "MIXED" and hl["better"] == "MIXED"


def test_headline_no():
    res = [{"metric": "w1_makespan", "verdict": "WORSE"}, {"metric": "w2_makespan", "verdict": "EQUIVALENT"}]
    assert headline(res, {})["faster"] == "NO"
    assert headline(res, {})["better"] == "NO"


def test_headline_tie():
    res = [{"metric": m["id"], "verdict": "EQUIVALENT"} for m in METRICS]
    hl = headline(res, {})
    assert hl["faster"] == "TIE" and hl["better"] == "TIE"


def test_headline_inconclusive():
    res = [{"metric": m["id"], "verdict": "EQUIVALENT"} for m in METRICS]
    res[0]["verdict"] = "INCONCLUSIVE"
    hl = headline(res, {})
    assert hl["faster"] == "INCONCLUSIVE" and hl["better"] == "INCONCLUSIVE"
    assert needs_extension(res) is True
    res[0]["verdict"] = "EQUIVALENT"
    assert needs_extension(res) is False


def test_headline_faster_yes_blocked_by_dq_and_baseline_dq_ignored():
    res = [{"metric": "w1_makespan", "verdict": "BETTER"}]
    assert headline(res, {CT: ["w1"]})["faster"] == "YES"          # baseline DQ does not change headline
    hl = headline(res, {ZM: ["w2", "w1"], CD: ["w3"]})
    assert hl["faster"] == hl["better"] == "DISQUALIFIED on w1, w2"


def test_warmup_ignored(cfg):
    rows = make_rows(20, w1())
    base = per_metric(rows, cfg)
    junk = [make_row(block=-1, order=0, workload="w1", step="main", system=s, wall_s=9999.0,
                     ok=10, total=10) for s in (ZM, CD, CT)]
    junk.append(make_row(block=-1, order=0, workload="w1", step="main", system=ZM, wall_s=9999.0,
                         ok=0, total=10))
    assert per_metric(rows + junk, cfg) == base
    assert disqualified(rows + junk, 0.10) == {}


def test_both_fail_counts_for_both():
    bad = frozenset({(b, "w1", s) for b in (2, 6, 11) for s in (ZM, CT)})
    rows = make_rows(20, w1(ct=10.0, cd=30.0), bad=bad)   # tuned is the best baseline
    assert best_baseline(rows, M["w1_makespan"]) == CT
    r = block_ratios(rows, M["w1_makespan"], CT)
    assert [r[b] for b in (2, 6, 11)] == [1.0, 1.0, 1.0]
    assert disqualified(rows, 0.10) == {ZM: ["w1"], CT: ["w1"]}


def test_excluded_cell_dropped(cfg):
    wall = {**w1(), ("w5", ZM): 8.0, ("w5", CD): 12.0, ("w5", CT): 10.0}
    rows = make_rows(20, wall)
    n0 = {r["metric"]: r["n"] for r in per_metric(rows, cfg)}
    rows.append(make_row(block=4, order=0, workload="w5", step="excluded", system="-",
                         notes="excluded=precheck_failed"))
    res = {r["metric"]: r["n"] for r in per_metric(rows, cfg)}
    assert res["w5_makespan"] == n0["w5_makespan"] - 1 == 19
    assert res["w5_reexec"] == 19
    assert res["w1_makespan"] == 20


def test_excluded_block_dropped_for_every_workload(cfg):
    wall = {**w1(), ("w5", ZM): 8.0, ("w5", CD): 12.0, ("w5", CT): 10.0}
    rows = make_rows(20, wall, bad=frozenset({(4, "w1", ZM)}))
    rows.append(make_row(block=4, order=0, workload="-", step="excluded", system="-",
                         notes="excluded=load_average"))
    res = {r["metric"]: r["n"] for r in per_metric(rows, cfg)}
    assert res["w1_makespan"] == res["w5_makespan"] == 19
    assert disqualified(rows, 0.0) == {}          # the failing block was excluded


def test_per_metric_known_answers(cfg):
    wall = {**w1(zm=8, cd=12, ct=10), ("w2", ZM): 10.0, ("w2", CD): 10.0, ("w2", CT): 10.0}
    res = {r["metric"]: r for r in per_metric(make_rows(20, wall), cfg)}
    w1r, w2r = res["w1_makespan"], res["w2_makespan"]
    assert w1r["baseline"] == CT and w1r["n"] == 20
    assert w1r["verdict"] == "BETTER" and 0.75 < w1r["median"] < 0.85
    assert w1r["ci_lo"] <= w1r["median"] <= w1r["ci_hi"] < 1.0
    assert w1r["p_adj"] >= w1r["p"] and w1r["p_adj"] < 0.05
    assert w2r["verdict"] == "EQUIVALENT"
    assert set(w1r) >= {"metric", "baseline", "n", "median", "ci_lo", "ci_hi", "p", "p_adj", "verdict"}


def test_w6_owner_is_a_diff_in_ms(cfg):
    wall = {("w6", ZM): 4.0, ("w6", CD): 20.0, ("w6", CT): 12.0}     # p99 under load; idle p99 = 1 ms
    rows = make_rows(20, wall, noise=0.01)
    r = block_ratios(rows, M["w6_owner"], CT)
    assert all(-9 < v < -7 for v in r.values())                      # (4-1) - (12-1) = -8 ms
    res = {x["metric"]: x for x in per_metric(rows, cfg)}
    assert res["w6_owner"]["verdict"] == "BETTER" and res["w6_owner"]["baseline"] == CT
    bad = make_rows(5, wall, bad=frozenset({(1, "w6", ZM), (2, "w6", CT), (3, "w6", ZM), (3, "w6", CT)}))
    r = block_ratios(bad, M["w6_owner"], CT)
    assert (r[1], r[2], r[3]) == (1e6, -1e6, 0.0)


def test_w5_reexec_smoothing_avoids_zero_over_zero(cfg):
    wall = {("w5", ZM): 0.0, ("w5", CD): 0.0, ("w5", CT): 0.0}
    rows = make_rows(20, wall)
    for r in rows:
        r["reexec_task_s"] = 0.0
    res = {x["metric"]: x for x in per_metric(rows, cfg)}
    assert res["w5_reexec"]["median"] == 1.0 and res["w5_reexec"]["verdict"] == "EQUIVALENT"


def test_variant_filter_for_w5():
    rows = make_rows(3, {("w5", ZM): 8.0, ("w5", CT): 10.0, ("w5", CD): 10.0})
    extra = [dict(r, variant="sigterm", wall_s=1.0) for r in rows]
    r = block_ratios(rows + extra, M["w5_makespan"], CT)
    assert len(r) == 3 and all(0.7 < v < 0.9 for v in r.values())


def test_duplicate_row_is_an_error():
    rows = make_rows(3, w1())
    with pytest.raises(ValueError, match="duplicate"):
        block_ratios(rows + [dict(rows[0])], M["w1_makespan"], CT)


def test_missing_value_on_ok_row_is_an_error():
    rows = make_rows(3, w1())
    rows[0]["wall_s"] = None
    with pytest.raises(ValueError, match="wall_s"):
        block_ratios(rows, M["w1_makespan"], CT)


def test_secondary_metrics(cfg):
    wall = {**w1(), ("w3", ZM): 5.0, ("w3", CD): 5.0, ("w3", CT): 5.0,
            ("w6", ZM): 4.0, ("w6", CD): 20.0, ("w6", CT): 12.0}
    res = {s["metric"]: s for s in secondary(make_rows(20, wall), cfg)}
    assert set(res) == {"w3_makespan", "w1_ttfr", "w6_tasks_per_min"}
    assert 0.75 < res["w1_ttfr"]["median"] < 0.85
    assert res["w3_makespan"]["n"] == 20 and "verdict" not in res["w3_makespan"]


def test_csv_roundtrip(tmp_path, cfg):
    rows = make_rows(6, w1(), bad=frozenset({(2, "w1", ZM)}))
    p = tmp_path / "r.csv"
    write_csv(rows, p)
    back = load(p)
    assert per_metric(back, cfg) == per_metric(rows, cfg)
    assert disqualified(back, 0.10) == disqualified(rows, 0.10)
    assert isinstance(back[0]["block"], int) and isinstance(back[0]["wall_s"], float)
    assert back[0]["ttfr_s"] is not None and back[0]["probe_p99_ms"] is None


def test_load_rejects_wrong_schema(tmp_path):
    p = tmp_path / "bad.csv"
    p.write_text("block,order\n0,0\n")
    with pytest.raises(ValueError, match="columns"):
        load(p)


def _res(verdict="BETTER"):
    return [{"metric": "w1_makespan", "baseline": CT, "n": 20, "median": 0.8, "ci_lo": 0.78,
             "ci_hi": 0.82, "p": 0.001, "p_adj": 0.007, "verdict": verdict, "kind": "ratio"}]


def test_smoke_no_verdict(tmp_path):
    hl = {"faster": "YES", "better": "YES", "why": "x"}
    write_outputs(_res(), hl, {}, "VALID", tmp_path, smoke=True)
    text = (tmp_path / "report.md").read_text()
    assert "SMOKE — no verdict" in text and "FASTER? YES" not in text and "FASTER?" not in text
    assert json.loads((tmp_path / "verdict.json").read_text())["smoke"] is True


def test_report_layout_and_scope(tmp_path):
    hl = {"faster": "YES", "better": "NO", "why": "x"}
    write_outputs(_res(), hl, {"laptops": 3, "donors": 2, "git_sha": "abc"}, "VALID", tmp_path, smoke=False)
    lines = (tmp_path / "report.md").read_text().splitlines()
    assert lines[0].startswith("FASTER? YES — ") and "3 laptops" in lines[0] and "?" in lines[0]
    assert lines[1].startswith("BETTER? NO — ")
    text = "\n".join(lines).lower()
    for needle in ("real lid close", "wi-fi vs wired", "more than the tested number of nodes",
                   "w1_makespan"):
        assert needle in text
    j = json.loads((tmp_path / "verdict.json").read_text())
    assert j["bootstrap"] == "percentile" and j["validity"] == "VALID" and j["headline"]["faster"] == "YES"


def test_report_notes_sections(tmp_path, cfg):
    rows = make_rows(20, w1(), bad=frozenset({(b, "w1", CT) for b in (0, 1, 2)}))
    rows[0]["notes"] = "condor_features=docker,used:file_transfer,unavailable:cgroups"
    rows[1]["notes"] = "single_donor_all_nodes;kill_after=3.2s"
    rows.append(make_row(block=7, order=0, workload="w2", step="excluded", system="-",
                         notes="excluded=precheck_failed"))
    res = per_metric(rows, cfg)
    dq = disqualified(rows, 0.10)
    ctx = {"rows": rows, "dq": dq, "secondary": secondary(rows, cfg), "seed": 1, "n_boot": 500}
    write_outputs(res, headline(res, dq), {}, "VALID", tmp_path, smoke=False, ctx=ctx)
    text = (tmp_path / "report.md").read_text()
    assert "block 7, w2: precheck_failed" in text
    assert "condor-tuned: w1" in text
    assert "docker" in text and "file_transfer" in text and "cgroups" in text
    assert "single donor" in text.lower()
    assert "3.2s" in text


def test_invalid_registration_banner(tmp_path, repo, capsys):
    root = tmp_path / "root"
    (root / "scripts/verdict").mkdir(parents=True)
    toml = root / "scripts/verdict/verdict.toml"
    toml.write_text((repo / "scripts/verdict/verdict.toml").read_text())
    reg = tmp_path / "registration.json"
    register.write_registration(root, reg)
    toml.write_text(toml.read_text() + "\n# edited after registration\n")
    csvp, out = tmp_path / "r.csv", tmp_path / "out"
    write_csv(make_rows(20, w1()), csvp)
    rc = analyze.main([str(csvp), "--config", str(toml), "--registration", str(reg),
                       "--root", str(root), "--out", str(out)])
    assert rc == 0
    assert (out / "report.md").read_text().splitlines()[0] == "INVALID: config changed after registration"
    assert json.loads((out / "verdict.json").read_text())["validity"] == "INVALID"


def test_missing_registration_is_invalid(tmp_path, repo):
    csvp, out = tmp_path / "r.csv", tmp_path / "out"
    write_csv(make_rows(20, w1()), csvp)
    rc = analyze.main([str(csvp), "--config", str(repo / "scripts/verdict/verdict.toml"),
                       "--registration", str(tmp_path / "nope.json"), "--root", str(repo),
                       "--out", str(out)])
    assert rc == 0
    assert (out / "report.md").read_text().splitlines()[0] == "INVALID: config changed after registration"


def test_cli_end_to_end_deterministic_and_needs_extension(tmp_path, repo):
    wall = {**w1(zm=10.0, cd=10.0, ct=10.0)}
    rows = make_rows(20, wall, noise=0.2)           # very noisy: neither BETTER nor EQUIVALENT
    csvp = tmp_path / "r.csv"
    write_csv(rows, csvp)
    (tmp_path / "scope.json").write_text(json.dumps({"laptops": 3, "donors": 2, "network": "wired",
                                                      "condor_version": "24.0", "git_sha": "abc"}))
    reg = tmp_path / "registration.json"
    register.write_registration(repo, reg)
    cmd = [sys.executable, str(repo / "scripts/verdict/analyze.py"), str(csvp), "--config",
           str(repo / "scripts/verdict/verdict.toml"), "--registration", str(reg), "--root", str(repo),
           "--out", str(tmp_path)]
    p1 = subprocess.run(cmd, capture_output=True, text=True)
    assert p1.returncode == 0, p1.stderr
    j1 = (tmp_path / "verdict.json").read_text()
    r1 = (tmp_path / "report.md").read_text()
    p2 = subprocess.run(cmd, capture_output=True, text=True)
    assert (tmp_path / "verdict.json").read_text() == j1 and (tmp_path / "report.md").read_text() == r1
    assert "NEEDS_EXTENSION" in p1.stdout
    assert "3 laptops, 2 donors, wired, HTCondor 24.0" in r1
    p3 = subprocess.run(cmd + ["--smoke"], capture_output=True, text=True)
    assert p3.returncode == 0 and "NEEDS_EXTENSION" not in p3.stdout
    assert "SMOKE — no verdict" in (tmp_path / "report.md").read_text()
