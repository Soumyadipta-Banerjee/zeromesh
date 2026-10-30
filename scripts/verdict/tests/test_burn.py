"""burn v2: outputs carry a digest of (raw value, case file, nonce) that can be checked against a reference."""
import os
import subprocess

import pytest

ITERS = 1000


@pytest.fixture(scope="session")
def zmf_home(tmp_path_factory):
    repo = __import__("pathlib").Path(__file__).resolve().parents[3]
    home = tmp_path_factory.mktemp("zmfhome")
    subprocess.run(["bash", "-c", "source scripts/zmfield.sh; build_burn"],
                   env={**os.environ, "ZMF_HOME": str(home)}, cwd=repo, check=True)
    return home


def burn(zmf_home, *args):
    return subprocess.run([str(zmf_home / "burn"), *args], capture_output=True, text=True, check=True).stdout.strip()


def digest(zmf_home, tmp_path, case_text, nonce, iters=ITERS):
    case = tmp_path / "case"
    case.write_text(case_text)
    burn(zmf_home, "cpu", str(iters), str(case), nonce)
    line = (tmp_path / "case.out").read_text().split()
    assert line[0] == "ok" and line[1] == "cpu"
    return line[2]


def test_digest_depends_on_case_content(zmf_home, tmp_path):
    assert digest(zmf_home, tmp_path, "case 001\n", "n1") != digest(zmf_home, tmp_path, "case 002\n", "n1")


def test_digest_depends_on_nonce(zmf_home, tmp_path):
    assert digest(zmf_home, tmp_path, "case 001\n", "n1") != digest(zmf_home, tmp_path, "case 001\n", "n2")


def test_ref_matches_real_run(zmf_home, tmp_path):
    raw = burn(zmf_home, "rawcpu", str(ITERS))
    case = tmp_path / "case"
    case.write_text("case 001\n")
    real = digest(zmf_home, tmp_path, "case 001\n", "n1")
    assert burn(zmf_home, "ref", raw, str(case), "n1") == real
    assert len(real) == 16


def test_sum_matches_ref(zmf_home, tmp_path):
    data = tmp_path / "big.bin"
    data.write_bytes(bytes(range(256)) * 100)
    case = tmp_path / "case"
    case.write_text("case 001\n")
    burn(zmf_home, "sum", str(data), str(case), "n1")
    fields = (tmp_path / "case.out").read_text().split()
    raw = burn(zmf_home, "rawsum", str(data))
    assert fields[:2] == ["ok", "sum"]
    assert fields[2] == burn(zmf_home, "ref", raw, str(case), "n1")


def count_ok(repo, zmf_home, wd, names, raw, nonce):
    r = subprocess.run(["bash", "-c", 'source scripts/zmfield.sh; count_ok "$@"', "x", str(wd), "cases/", str(names), raw, nonce],
                       env={**os.environ, "ZMF_HOME": str(zmf_home)}, cwd=repo, capture_output=True, text=True, check=True)
    return r.stdout.strip()


def test_exec_counts_wrong_digest_as_fail(repo, zmf_home, tmp_path):
    wd = tmp_path / "wd"
    (wd / "cases").mkdir(parents=True)
    (wd / "cases" / "000").write_text("case 000\n")
    names = tmp_path / "names"
    names.write_text("000\n")
    out = wd / "cases" / "000.out"
    out.write_text("ok cpu 0000000000000000 host\n")
    raw = burn(zmf_home, "rawcpu", str(ITERS))
    assert count_ok(repo, zmf_home, wd, names, raw, "n1") == "0"
    good = burn(zmf_home, "ref", raw, str(wd / "cases" / "000"), "n1")
    out.write_text(f"ok cpu {good} host\n")
    assert count_ok(repo, zmf_home, wd, names, raw, "n1") == "1"
    assert count_ok(repo, zmf_home, wd, names, raw, "other-nonce") == "0"
    out.write_text(f"fail cpu {good} host\n")
    assert count_ok(repo, zmf_home, wd, names, raw, "n1") == "0"


def test_payload_kind(repo):
    def kind(wl, force=""):
        env = {**os.environ}
        env.pop("PAYLOAD_FORCE", None)
        if force:
            env["PAYLOAD_FORCE"] = force
        return subprocess.run(["bash", "-c", f"source scripts/zmfield.sh; payload_kind {wl}"],
                              env=env, cwd=repo, capture_output=True, text=True, check=True).stdout.strip()
    assert kind("w1") == "cpu" and kind("w2") == "sum" and kind("w3") == "cpu"
    assert kind("w2", "cpu") == "cpu" and kind("w4", "sum") == "sum"


def test_bench_still_parses(repo):
    subprocess.run(["bash", "-n", "scripts/zmfield.sh"], cwd=repo, check=True)
    subprocess.run(["bash", "scripts/zmfield.sh", "--dry-run", "bench"], cwd=repo, check=True, capture_output=True,
                   env={**os.environ, "ZMF_HOME": "/tmp/claude-1000/zmf-dryrun-test"})
