"""Submit Blender frame chunks via the ZeroMesh SDK and collect outputs."""

from __future__ import annotations

import os
import shutil
import subprocess
import sys
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any, Iterable, Optional, TextIO

from zm_render.blend_paths import warn_external_paths
from zm_render.frames import Chunk, FrameRange, frame_filename, split_chunks


@dataclass
class RenderOptions:
    blend: Path
    frames: FrameRange
    chunk: int = 1
    out_dir: Path = Path("renders")
    env: str = "host"
    encode: Optional[Path] = None
    blender_bin: str = "blender"
    cores: int = 1
    may_run_local: bool = True
    may_run_remote: bool = True


@dataclass
class ChunkProgress:
    chunk: Chunk
    executed_by: str = ""
    done: bool = False


@dataclass
class RenderResult:
    frames_ok: list[int] = field(default_factory=list)
    frames_missing: list[int] = field(default_factory=list)
    progress: list[ChunkProgress] = field(default_factory=list)


class MissingFramesError(RuntimeError):
    def __init__(self, frames: Iterable[int], message: str):
        super().__init__(message)
        self.frames = list(frames)


def blender_argv(blend_rel: str, chunk: Chunk, blender_bin: str = "blender") -> list[str]:
    """Argv for one chunk: render frames [start, end] as PNG under //out/."""
    return [
        blender_bin,
        "-b",
        blend_rel,
        "-o",
        "//out/frame_####",
        "-F",
        "PNG",
        "-s",
        str(chunk.start),
        "-e",
        str(chunk.end),
        "-a",
    ]


def build_job_spec(
    *,
    input_root: Any,
    blend_rel: str,
    chunks: list[Chunk],
    env: str,
    cores: int,
    blender_bin: str,
    common_pb2: Any,
    may_run_local: bool = True,
    may_run_remote: bool = True,
) -> Any:
    """Build a JobSpec with one task per chunk."""
    policy = common_pb2.Policy(
        may_run_local=may_run_local,
        may_run_remote=may_run_remote,
    )
    tasks = []
    for ch in chunks:
        tasks.append(
            common_pb2.TaskSpec(
                name=ch.name,
                argv=blender_argv(blend_rel, ch, blender_bin=blender_bin),
                input_root=input_root,
                outputs=["out/"],
                env_spec=common_pb2.EnvSpec(name=env),
                resources=common_pb2.Resources(cores=cores),
                policy=policy,
            )
        )
    return common_pb2.JobSpec(name="zm-render", tasks=tasks)


def expected_frame_path(out_dir: Path, frame: int) -> Path:
    return out_dir / frame_filename(frame)


def collect_frames(out_dir: Path, frames: FrameRange) -> RenderResult:
    """Check that every expected frame file exists under out_dir."""
    result = RenderResult()
    for n in frames.frame_numbers():
        p = expected_frame_path(out_dir, n)
        if p.is_file() and p.stat().st_size > 0:
            result.frames_ok.append(n)
        else:
            result.frames_missing.append(n)
    return result


def _print_progress(
    done_frames: int,
    total_frames: int,
    chunk: Chunk,
    executed_by: str,
    file: TextIO,
) -> None:
    who = executed_by or "?"
    print(
        f"[{done_frames}/{total_frames}] frames {chunk.start}-{chunk.end} on {who}",
        file=file,
    )


def run_render(
    client: Any,
    opts: RenderOptions,
    *,
    progress_file: TextIO | None = None,
) -> RenderResult:
    """Upload scene, submit chunked Blender tasks, fetch and verify frames."""
    from zeromesh.v1 import common_pb2

    prog = progress_file if progress_file is not None else sys.stderr
    blend = opts.blend.resolve()
    if not blend.is_file():
        raise FileNotFoundError(f"scene not found: {blend}")
    if opts.env not in ("host", "mirror"):
        raise ValueError(f"--env must be host or mirror, got {opts.env!r}")

    warn_external_paths(blend, file=prog)

    scene_dir = blend.parent
    blend_rel = blend.name
    chunks = split_chunks(opts.frames, opts.chunk)
    total = opts.frames.count

    root = client.upload(scene_dir)
    job_spec = build_job_spec(
        input_root=root,
        blend_rel=blend_rel,
        chunks=chunks,
        env=opts.env,
        cores=opts.cores,
        blender_bin=opts.blender_bin,
        common_pb2=common_pb2,
        may_run_local=opts.may_run_local,
        may_run_remote=opts.may_run_remote,
    )
    job = client.submit(job_spec)

    progress = [ChunkProgress(chunk=ch) for ch in chunks]
    by_name = {p.chunk.name: p for p in progress}
    done_frames = 0

    succeeded = common_pb2.SUCCEEDED
    for ev in job.events():
        name = getattr(ev, "task_name", "") or ""
        state = getattr(ev, "state", None)
        if name in by_name and state == succeeded:
            cp = by_name[name]
            if not cp.done:
                res = getattr(ev, "result", None)
                who = getattr(res, "executed_by", "") if res is not None else ""
                cp.executed_by = who or cp.executed_by
                cp.done = True
                done_frames += cp.chunk.end - cp.chunk.start + 1
                _print_progress(done_frames, total, cp.chunk, cp.executed_by, prog)

    status = job.wait(check=True)
    # Fill executed_by / progress from final results if the event stream was sparse.
    for name, res in status.results.items():
        if name not in by_name:
            continue
        cp = by_name[name]
        if not cp.executed_by:
            cp.executed_by = res.executed_by
        if not cp.done and res.exit_code == 0:
            cp.done = True
            done_frames += cp.chunk.end - cp.chunk.start + 1
            _print_progress(done_frames, total, cp.chunk, cp.executed_by, prog)

    out_dir = opts.out_dir.resolve()
    out_dir.mkdir(parents=True, exist_ok=True)

    # Fetch each chunk into a staging dir, then flatten frames into out_dir.
    for ch in chunks:
        staging = out_dir / f".chunk_{ch.start}_{ch.end}"
        if staging.exists():
            shutil.rmtree(staging)
        staging.mkdir(parents=True)
        job.fetch(ch.name, staging)
        _flatten_frames(staging, out_dir)
        shutil.rmtree(staging, ignore_errors=True)

    result = collect_frames(out_dir, opts.frames)
    result.progress = progress

    if result.frames_missing:
        missing = ", ".join(str(n) for n in result.frames_missing)
        raise MissingFramesError(result.frames_missing, f"missing frames: {missing}")

    if opts.encode is not None:
        encode_video(out_dir, opts.frames, opts.encode)

    return result


def _flatten_frames(staging: Path, out_dir: Path) -> None:
    """Move frame_*.png from staging (possibly under out/) into out_dir."""
    candidates: list[Path] = []
    for root, _dirs, files in os.walk(staging):
        for f in files:
            if f.startswith("frame_") and f.lower().endswith(".png"):
                candidates.append(Path(root) / f)
    for src in candidates:
        dest = out_dir / src.name
        if dest.exists():
            dest.unlink()
        shutil.move(str(src), str(dest))


def encode_video(out_dir: Path, frames: FrameRange, dest: Path) -> None:
    """Encode collected PNGs to mp4 with ffmpeg (local)."""
    pattern = str(out_dir / "frame_%04d.png")
    dest.parent.mkdir(parents=True, exist_ok=True)
    cmd = [
        "ffmpeg",
        "-y",
        "-framerate",
        "24",
        "-start_number",
        str(frames.start),
        "-i",
        pattern,
        "-frames:v",
        str(frames.count),
        "-c:v",
        "libx264",
        "-pix_fmt",
        "yuv420p",
        str(dest),
    ]
    try:
        subprocess.run(cmd, check=True, capture_output=True, text=True)
    except FileNotFoundError as e:
        raise SystemExit("ffmpeg not found; install it or omit --encode") from e
    except subprocess.CalledProcessError as e:
        raise SystemExit(f"ffmpeg failed: {e.stderr or e.stdout}") from e
