"""End-to-end zm-render against a real zmd with a fake blender binary."""

from __future__ import annotations

import os
import shutil
import sys
import tempfile
from pathlib import Path

import pytest

REPO = Path(__file__).resolve().parents[3]
APP = Path(__file__).resolve().parents[1]
FAKE_BLENDER = Path(__file__).resolve().parent / "fake_blender.py"
INTEGRATION = REPO / "tests" / "integration"
SDK = REPO / "sdk" / "python"

for p in (str(APP), str(SDK), str(INTEGRATION)):
    if p not in sys.path:
        sys.path.insert(0, p)

import zmtest  # noqa: E402
import zeromesh  # noqa: E402
from zm_render.cli import main as zm_render_main  # noqa: E402
from zm_render.frames import frame_filename  # noqa: E402


@pytest.fixture
def zmd():
    daemon = zmtest.Zmd()
    yield daemon
    daemon.close()


def _scene_dir(tmp: Path, *, skip3: bool = False) -> Path:
    scene = tmp / "scene"
    scene.mkdir()
    name = "skip3.blend" if skip3 else "shot.blend"
    (scene / name).write_bytes(b"BLENDER-fake\x00")
    # Ship fake blender next to the scene (uploaded with the tree).
    blender = scene / "blender"
    shutil.copy(FAKE_BLENDER, blender)
    blender.chmod(0o755)
    return scene


def test_render_collects_all_frames(zmd, tmp_path):
    scene = _scene_dir(tmp_path)
    out = tmp_path / "renders"
    rc = zm_render_main([
        str(scene / "shot.blend"),
        "--frames",
        "1-6",
        "--chunk",
        "2",
        "-o",
        str(out),
        "--blender",
        "./blender",
        "--socket",
        zmd.socket,
    ])
    assert rc == 0
    for n in range(1, 7):
        p = out / frame_filename(n)
        assert p.is_file(), f"missing {p}"
        assert p.stat().st_size > 0


def test_partial_chunk_failure_still_fetches_ok_chunks(zmd, tmp_path):
    scene = tmp_path / "scene"
    scene.mkdir()
    (scene / "partialfail.blend").write_bytes(b"BLENDER-fake\x00")
    blender = scene / "blender"
    shutil.copy(FAKE_BLENDER, blender)
    blender.chmod(0o755)
    out = tmp_path / "renders"
    rc = zm_render_main([
        str(scene / "partialfail.blend"),
        "--frames",
        "1-4",
        "--chunk",
        "2",
        "-o",
        str(out),
        "--blender",
        "./blender",
        "--socket",
        zmd.socket,
    ])
    assert rc != 0
    assert (out / frame_filename(1)).is_file()
    assert (out / frame_filename(2)).is_file()
    assert not (out / frame_filename(3)).is_file()


def test_missing_frame_fails(zmd, tmp_path):
    scene = _scene_dir(tmp_path, skip3=True)
    out = tmp_path / "renders"
    rc = zm_render_main([
        str(scene / "skip3.blend"),
        "--frames",
        "1-4",
        "--chunk",
        "4",
        "-o",
        str(out),
        "--blender",
        "./blender",
        "--socket",
        zmd.socket,
    ])
    assert rc != 0
    # Frames 1,2,4 exist; 3 does not.
    assert (out / frame_filename(1)).is_file()
    assert not (out / frame_filename(3)).is_file()


def test_chunk_boundaries_in_submitted_job(zmd, tmp_path):
    """Upload + submit path: three chunks for 1-5 with --chunk 2."""
    scene = _scene_dir(tmp_path)
    zm = zeromesh.connect(socket_path=zmd.socket)
    from zm_render.frames import FrameRange, split_chunks
    from zm_render.render import RenderOptions, run_render

    opts = RenderOptions(
        blend=scene / "shot.blend",
        frames=FrameRange(1, 5),
        chunk=2,
        out_dir=tmp_path / "out",
        blender_bin="./blender",
    )
    with zm:
        result = run_render(zm, opts)
    assert [ (p.chunk.start, p.chunk.end) for p in result.progress ] == [
        (1, 2),
        (3, 4),
        (5, 5),
    ]
    assert result.frames_missing == []
    assert result.frames_ok == [1, 2, 3, 4, 5]


def test_connect_without_daemon(tmp_path, monkeypatch):
    monkeypatch.setenv("ZM_RUNTIME_DIR", str(tmp_path))
    rc = zm_render_main([
        str(tmp_path / "no.blend"),
        "--frames",
        "1-1",
    ])
    assert rc == 1
