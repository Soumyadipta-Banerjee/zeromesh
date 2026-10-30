"""Orchestration skeleton (verdict.sh) driven end to end through the fake backend (ZMV_FAKE=1)."""
import csv
import json
import os
import shutil
import subprocess
import sys
from pathlib import Path

import pytest

REPO = Path(__file__).resolve().parents[3]
SYSTEMS = ["zm", "condor-default", "condor-tuned"]


@pytest.fixture
def tmp_root(tmp_path) -> Path:
    """A private copy of scripts/ used as ROOT, plus an empty ZMF_HOME next to it."""
    shutil.copytree(REPO / "scripts", tmp_path / "root" / "scripts",
                    ignore=shutil.ignore_patterns("tests", "__pycache__", ".pytest_cache"))
    (tmp_path / "home").mkdir()
    return tmp_path


def run_verdict(args, env, tmp_root) -> subprocess.CompletedProcess:
    full = {**os.environ, "ZMF_HOME": str(tmp_root / "home"), "ZMV_FAKE": "1", "VD_PY": sys.executable}
    full.update(env or {})
    return subprocess.run(["bash", str(tmp_root / "root" / "scripts" / "zmfield.sh"), "verdict", *args],
                          env=full, capture_output=True, text=True, timeout=300)


def res_dir(tmp_root) -> Path:
    dirs = sorted((tmp_root / "home" / "results").glob("verdict-*"))
    assert dirs, "no results dir created"
    return dirs[-1]


def read_csv(res) -> list:
    with open(Path(res) / "results.csv", newline="") as f:
        return list(csv.DictReader(f))


def smoke(tmp_root, extra=(), env=None):
    return run_verdict(["run", "--smoke", "--ssh", "fake@x", *extra], env, tmp_root)


def test_refuses_without_registration(tmp_root):
    r = run_verdict(["run", "--ssh", "fake@x"], {}, tmp_root)
    assert r.returncode == 2
    assert "config changed since registration" in r.stderr


def test_requires_ssh(tmp_root):
    r = run_verdict(["run", "--smoke"], {}, tmp_root)
    assert r.returncode == 2
    assert "verdict needs --ssh" in r.stderr


def test_smoke_runs_3_blocks_plus_warmup(tmp_root):
    r = smoke(tmp_root)
    assert r.returncode == 0, r.stderr
    res = res_dir(tmp_root)
    rows = read_csv(res)
    for wl in ("w1", "w2"):
        assert {int(x["block"]) for x in rows if x["workload"] == wl} == {-1, 1, 2, 3}
        for b in (1, 2, 3):
            got = sorted(x["system"] for x in rows if x["workload"] == wl and int(x["block"]) == b)
            assert got == sorted(SYSTEMS)
        assert any(x["system"] == "zm-local" and int(x["block"]) == -1 for x in rows if x["workload"] == wl)
    assert not any(x["system"] == "zm-local" and int(x["block"]) > 0 for x in rows)
    assert (res / "report.md").read_text().startswith("SMOKE — no verdict")


def test_resume_skips_done_cells(tmp_root):
    assert smoke(tmp_root).returncode == 0
    res = res_dir(tmp_root)
    full = read_csv(res)
    keep = [x for x in full if int(x["block"]) == -1 or (int(x["block"]) == 1 and x["workload"] == "w1")]
    keep[-1]["wall_s"] = "99.00"   # a pre-seeded cell must survive untouched
    marker = (keep[-1]["block"], keep[-1]["workload"], keep[-1]["system"])
    with open(res / "results.csv", "w", newline="") as f:
        w = csv.DictWriter(f, fieldnames=list(full[0].keys()))
        w.writeheader()
        w.writerows(keep)
    r = run_verdict(["run", "--smoke", "--ssh", "fake@x", "--resume", str(res)], {}, tmp_root)
    assert r.returncode == 0, r.stderr
    rows = read_csv(res)
    keys = [(x["block"], x["workload"], x["step"], x["system"], x["variant"]) for x in rows]
    assert len(keys) == len(set(keys))
    assert len(rows) == len(full)
    kept = [x for x in rows if (x["block"], x["workload"], x["system"]) == marker]
    assert [x["wall_s"] for x in kept] == ["99.00"]


def small_registered_tree(tmp_root):
    toml = tmp_root / "root" / "scripts" / "verdict" / "verdict.toml"
    toml.write_text(toml.read_text().replace("blocks = 20", "blocks = 2").replace("extend_to = 40", "extend_to = 0"))
    r = run_verdict(["register"], {}, tmp_root)
    assert r.returncode == 0, r.stderr
    return toml


def test_registered_run_archives_tree_and_reports(tmp_root):
    small_registered_tree(tmp_root)
    r = run_verdict(["run", "--ssh", "fake@x"], {}, tmp_root)
    assert r.returncode == 0, r.stderr
    res = res_dir(tmp_root)
    for rel in ("registration.json", "verdict.toml", "condor_tuned.conf", "scope.json",
                "registered_tree/scripts/verdict/verdict.toml", "registered_tree/scripts/verdict/verdict.sh",
                "registered_tree/scripts/zmfield.sh"):
        assert (res / rel).exists(), rel
    assert {int(x["block"]) for x in read_csv(res)} == {-1, 1, 2}
    (res / "report.md").unlink()
    r = run_verdict(["report", str(res)], {}, tmp_root)
    assert r.returncode == 0, r.stderr
    assert "INVALID" not in (res / "report.md").read_text()
    assert json.loads((res / "verdict.json").read_text())["validity"] == "VALID"


def test_resume_refuses_after_edit(tmp_root):
    toml = small_registered_tree(tmp_root)
    assert run_verdict(["run", "--ssh", "fake@x"], {}, tmp_root).returncode == 0
    res = res_dir(tmp_root)
    toml.write_text(toml.read_text().replace("alpha = 0.05", "alpha = 0.10"))
    r = run_verdict(["run", "--ssh", "fake@x", "--resume", str(res)], {}, tmp_root)
    assert r.returncode == 2
    assert "config changed since registration" in r.stderr


def test_nonce_unique_per_block_and_system():
    sh = REPO / "scripts" / "verdict" / "verdict.sh"
    r = subprocess.run(["bash", "-c", f"source {sh}; RUNID=R; vd_nonce 1 w1 zm; vd_nonce 2 w1 zm; vd_nonce 1 w1 condor-tuned"],
                       capture_output=True, text=True, check=True)
    lines = r.stdout.split()
    assert len(set(lines)) == 3
    assert lines[0] == "R-b1-w1-zm"


def test_order_matches_config_py(tmp_root):
    assert smoke(tmp_root).returncode == 0
    rows = [x for x in read_csv(res_dir(tmp_root)) if int(x["block"]) == 2 and x["workload"] == "w1"]
    got = [x["system"] for x in sorted(rows, key=lambda x: int(x["order"]))]
    seed = 20261030
    want = subprocess.run([sys.executable, str(REPO / "scripts/verdict/config.py"), "order", str(seed), "2",
                           "zm", "condor-default", "condor-tuned"], capture_output=True, text=True,
                          check=True).stdout.split()
    assert got == want


def test_precheck_failure_excludes_block(tmp_root):
    r = smoke(tmp_root, env={"ZMV_FAKE_PRECHECK_FAIL": "2"})
    assert r.returncode == 0, r.stderr
    rows = read_csv(res_dir(tmp_root))
    b2 = [x for x in rows if int(x["block"]) == 2]
    assert len(b2) == 1
    assert b2[0]["workload"] == "-" and b2[0]["step"] == "excluded" and "excluded=" in b2[0]["notes"]
    assert {int(x["block"]) for x in rows if x["workload"] == "w1"} == {-1, 1, 3}


def test_register_prints_commit_command(tmp_root):
    r = run_verdict(["register"], {}, tmp_root)
    assert r.returncode == 0, r.stderr
    assert 'git add scripts/verdict/registration.json && git commit -m "verdict: register ' in r.stdout
    assert (tmp_root / "root" / "scripts" / "verdict" / "registration.json").exists()
