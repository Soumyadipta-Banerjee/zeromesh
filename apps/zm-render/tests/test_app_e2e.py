"""zm-render against a real zmd, with a fake Blender shipped inside the scene directory."""

from __future__ import annotations

import shutil
import sys
from pathlib import Path

import pytest

REPO = Path(__file__).resolve().parents[3]
APP = Path(__file__).resolve().parents[1]
FAKE_BLENDER = Path(__file__).resolve().parent / "fake_blender.py"
for p in (str(APP), str(REPO / "sdk" / "python"), str(REPO / "tests" / "integration")):
    if p not in sys.path:
        sys.path.insert(0, p)

import zmtest  # noqa: E402
from zeromesh import report  # noqa: E402
from zeromesh.adapter import run_adapter  # noqa: E402
from zeromesh.v1 import run_pb2  # noqa: E402
from zm_render.cli import Render  # noqa: E402
from zm_render.frames import frame_filename  # noqa: E402


@pytest.fixture(scope="module")
def zmd():
    daemon = zmtest.Zmd()
    yield daemon
    daemon.close()


def scene(tmp: Path, name: str) -> Path:
    d = tmp / "scene"
    d.mkdir()
    (d / name).write_bytes(b"BLENDER-fake\x00")
    shutil.copy(FAKE_BLENDER, d / "blender")
    (d / "blender").chmod(0o755)
    return d / name


def render(zmd, blend: Path, out: Path, *extra: str) -> tuple[int, run_pb2.Report]:
    rc = run_adapter(Render(), [str(blend), "--blender", "./blender", "--socket", zmd.socket, "-o", str(out),
                                "--cores", "1", "--no-cache", *extra])
    return rc, report.load(str(out / "report.json"))


def states(rep: run_pb2.Report) -> dict[str, int]:
    return {r.id: r.state for r in rep.items}


def test_every_frame_lands_in_out(zmd, tmp_path):
    out = tmp_path / "renders"
    rc, rep = render(zmd, scene(tmp_path, "shot.blend"), out, "--frames", "1-6")
    assert rc == 0, zmd.log()
    for n in range(1, 7):
        assert (out / frame_filename(n)).stat().st_size > 0
    assert set(states(rep).values()) == {run_pb2.ITEM_SUCCEEDED}


def test_a_crash_mid_batch_keeps_the_frames_before_it(zmd, tmp_path):
    out = tmp_path / "renders"
    # One Blender renders 1-4 and dies at 3: frames 1 and 2 were committed as they were written.
    rc, rep = render(zmd, scene(tmp_path, "partialfail.blend"), out, "--frames", "1-4", "--batch", "4")
    assert rc == 1
    assert (out / frame_filename(1)).is_file() and (out / frame_filename(2)).is_file()
    assert not (out / frame_filename(3)).exists()
    s = states(rep)
    assert s["frame_0001"] == s["frame_0002"] == run_pb2.ITEM_SUCCEEDED
    assert s["frame_0003"] == s["frame_0004"] == run_pb2.ITEM_FAILED


def test_a_frame_blender_skipped_is_a_missing_output(zmd, tmp_path):
    out = tmp_path / "renders"
    rc, rep = render(zmd, scene(tmp_path, "skip3.blend"), out, "--frames", "1-4", "--batch", "4")
    assert rc == 1
    assert [n for n in range(1, 5) if (out / frame_filename(n)).is_file()] == [1, 2, 4]
    failed = {r.id: r.failure.kind for r in report.failed(rep)}
    assert failed == {"frame_0003": run_pb2.FAILURE_MISSING_OUTPUT}


def test_usage_errors_and_a_missing_daemon(tmp_path):
    assert run_adapter(Render(), [str(tmp_path / "no.blend"), "--frames", "1"]) == 2
    blend = scene(tmp_path, "shot.blend")
    assert run_adapter(Render(), [str(blend), "--frames", "5-3"]) == 2
    assert run_adapter(Render(), [str(blend), "--frames", "1", "--socket", str(tmp_path / "none.sock")]) == 2
