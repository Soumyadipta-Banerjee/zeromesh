"""commit_unit: the task-side half of partial commit. Needs no daemon: it writes into $ZM_UNITS_DIR."""

import os
import sys

import pytest

current_dir = os.path.dirname(os.path.abspath(__file__))
sdk_dir = os.path.abspath(os.path.join(current_dir, ".."))
if sdk_dir not in sys.path:
    sys.path.insert(0, sdk_dir)

import zeromesh
from zeromesh import units


@pytest.fixture
def units_dir(tmp_path, monkeypatch):
    d = tmp_path / "units"
    d.mkdir()
    monkeypatch.setenv("ZM_UNITS_DIR", str(d))
    return d


def test_commit_bytes_str_and_file(units_dir, tmp_path):
    units.commit_unit("a", b"\x00\x01binary")
    units.commit_unit("b", "text é")
    src = tmp_path / "frame.exr"
    src.write_bytes(b"pixels")
    units.commit_unit("c", src)
    assert (units_dir / "a").read_bytes() == b"\x00\x01binary"
    assert (units_dir / "b").read_bytes() == "text é".encode()
    assert (units_dir / "c").read_bytes() == b"pixels"
    assert src.exists()  # a file is copied, not moved


def test_commit_leaves_no_staging_files(units_dir):
    units.commit_unit("a", b"x")
    assert sorted(p.name for p in units_dir.iterdir()) == ["a"]


def test_recommit_replaces_the_unit(units_dir):
    units.commit_unit("a", b"old")
    units.commit_unit("a", b"new")
    assert (units_dir / "a").read_bytes() == b"new"


def test_committed_units_lists_finished_units_only(units_dir):
    units.commit_unit("u2", b"2")
    units.commit_unit("u1", b"1")
    (units_dir / ".tmp-half-written").write_bytes(b"partial")
    assert units.committed_units() == ["u1", "u2"]
    assert units.is_committed("u1")
    assert not units.is_committed("u3")


def test_unit_result_reads_a_unit_committed_by_an_earlier_attempt(units_dir):
    (units_dir / "u1").write_bytes(b"from the first attempt")
    assert units.unit_result("u1") == b"from the first attempt"
    assert units.unit_result("nope") is None


@pytest.mark.parametrize("bad", ["", ".hidden", "a/b", "../x", "with space", "x" * 129])
def test_invalid_unit_ids_are_rejected(units_dir, bad):
    with pytest.raises(ValueError):
        units.commit_unit(bad, b"x")
    assert list(units_dir.iterdir()) == []


def test_outside_a_task_there_is_nowhere_to_commit(monkeypatch):
    monkeypatch.delenv("ZM_UNITS_DIR", raising=False)
    assert units.units_dir() is None
    assert units.committed_units() == []
    with pytest.raises(zeromesh.ZeroMeshError):
        units.commit_unit("a", b"x")


def test_exported_from_the_package(units_dir):
    zeromesh.commit_unit("a", b"x")
    assert zeromesh.committed_units() == ["a"]
