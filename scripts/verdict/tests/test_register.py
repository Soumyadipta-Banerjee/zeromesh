import shutil

import pytest

from register import HASHED, check, compute_hash, write_registration


@pytest.fixture
def tmp_tree(tmp_path, repo):
    for rel in HASHED:
        dst = tmp_path / rel
        dst.parent.mkdir(parents=True, exist_ok=True)
        if rel in ("scripts/verdict/verdict.toml", "scripts/baselines/condor_tuned.conf"):
            shutil.copy(repo / rel, dst)
        elif not rel.endswith("probe.c"):  # probe.c stays absent: exercises MISSING
            dst.write_text(f"dummy {rel}\n")
    return tmp_path


def test_hash_stable(tmp_tree):
    assert compute_hash(tmp_tree) == compute_hash(tmp_tree)


def test_hash_changes_on_toml_edit(tmp_tree):
    h = compute_hash(tmp_tree)
    (tmp_tree / "scripts/verdict/verdict.toml").write_text("x=1\n")
    assert compute_hash(tmp_tree) != h


def test_check_detects_edit_after_registration(tmp_tree, tmp_path):
    reg = tmp_path / "r.json"
    write_registration(tmp_tree, reg)
    assert check(tmp_tree, reg) is True
    (tmp_tree / "scripts/verdict/verdict.toml").write_text("x=1\n")
    assert check(tmp_tree, reg) is False
