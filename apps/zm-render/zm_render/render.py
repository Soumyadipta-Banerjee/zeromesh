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
    mirror_paths: list[Path] = field(default_factory=list)
    encode: Optional[Path] = None
    blender_bin: str = "blender"
    engine: Optional[str] = None
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


@dataclass
class ChunkTaskFailure:
    chunk: Chunk
    message: str


class ChunkTasksFailed(RuntimeError):
    """One or more render chunks failed on the mesh."""

    def __init__(self, failures: list[ChunkTaskFailure], message: str):
        super().__init__(message)
        self.failures = failures


def blender_argv(
    blend_rel: str,
    chunk: Chunk,
    blender_bin: str = "blender",
    engine: Optional[str] = None,
) -> list[str]:
    """Argv for one chunk: render frames [start, end] as PNG under //out/."""
    # -noaudio: sandbox has no PipeWire. Do not pass --cycles-device (Blender 5
    # treats unknown long opts as filenames). Engine is opt-in via -E.
    argv = [
        blender_bin,
        "-b",
        blend_rel,
        "-noaudio",
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
    if engine:
        idx = argv.index("-noaudio") + 1
        argv[idx:idx] = ["-E", engine]
    return argv


# Sandbox has no PipeWire/Pulse; force null audio backends.
# XDG_RUNTIME_DIR must be mode 0700 (sandbox creates /tmp/xdg-runtime) — not /tmp.
_BLENDER_ENV = {
    "ALSOFT_DRIVERS": "null",
    "SDL_AUDIODRIVER": "dummy",
    "PIPEWIRE_RUNTIME_DIR": "/tmp/xdg-runtime",
    "XDG_RUNTIME_DIR": "/tmp/xdg-runtime",
}


def build_job_spec(
    *,
    input_root: Any,
    blend_rel: str,
    chunks: list[Chunk],
    env: str,
    cores: int,
    blender_bin: str,
    common_pb2: Any,
    engine: Optional[str] = None,
    may_run_local: bool = True,
    may_run_remote: bool = True,
    mirror_paths: Optional[list[str]] = None,
) -> Any:
    """Build a JobSpec with one task per chunk."""
    policy = common_pb2.Policy(
        may_run_local=may_run_local,
        may_run_remote=may_run_remote,
    )
    env_params: dict[str, str] = {}
    if env == "mirror" and mirror_paths:
        env_params["mirror_paths"] = "\n".join(mirror_paths)

    tasks = []
    for ch in chunks:
        tasks.append(
            common_pb2.TaskSpec(
                name=ch.name,
                argv=blender_argv(
                    blend_rel, ch, blender_bin=blender_bin, engine=engine
                ),
                input_root=input_root,
                outputs=["out/"],
                env_spec=common_pb2.EnvSpec(name=env, params=env_params),
                resources=common_pb2.Resources(cores=cores),
                policy=policy,
                env=_BLENDER_ENV,
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


def _peer_names(client: Any) -> dict[str, str]:
    """Map node_id (and 8-char prefix) → hostname."""
    out: dict[str, str] = {}
    try:
        for p in client.peers():
            nid = getattr(p, "node_id", "") or ""
            host = getattr(p, "hostname", "") or nid[:8]
            if not nid:
                continue
            out[nid] = host
            out[nid[:8]] = host
    except Exception:
        pass
    return out


def _who(node_id: str, names: dict[str, str]) -> str:
    if not node_id:
        return "?"
    host = names.get(node_id) or names.get(node_id[:8])
    if host:
        return f"{host} ({node_id[:8]})"
    return node_id[:8]


def _print_event(kind: str, chunk: Chunk, who: str, file: TextIO, extra: str = "") -> None:
    msg = f"{kind:9} frames {chunk.start:>3}-{chunk.end:<3}  →  {who}"
    if extra:
        msg += f"  {extra}"
    print(msg, file=file, flush=True)


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
    if opts.env == "mirror" and not opts.mirror_paths:
        raise ValueError("--env mirror requires at least one --mirror PATH")

    warn_external_paths(blend, file=prog)

    scene_dir = blend.parent
    blend_rel = blend.name
    chunks = split_chunks(opts.frames, opts.chunk)
    total = opts.frames.count

    names = _peer_names(client)
    print(
        f"zm-render: {total} frames → {len(chunks)} chunks "
        f"(chunk={opts.chunk}, env={opts.env})",
        file=prog,
        flush=True,
    )
    for ch in chunks:
        print(f"  queue     frames {ch.start:>3}-{ch.end:<3}  ({ch.name})", file=prog, flush=True)

    root = client.upload(scene_dir)
    mirror_paths = None
    if opts.env == "mirror":
        mirror_paths = [str(p.resolve()) for p in opts.mirror_paths]

    job_spec = build_job_spec(
        input_root=root,
        blend_rel=blend_rel,
        chunks=chunks,
        env=opts.env,
        cores=opts.cores,
        blender_bin=opts.blender_bin,
        engine=opts.engine,
        common_pb2=common_pb2,
        may_run_local=opts.may_run_local,
        may_run_remote=opts.may_run_remote,
        mirror_paths=mirror_paths,
    )
    job = client.submit(job_spec)
    print(f"zm-render: submitted job {job.job_id}", file=prog, flush=True)

    progress = [ChunkProgress(chunk=ch) for ch in chunks]
    by_name = {p.chunk.name: p for p in progress}
    done_frames = 0
    started: set[str] = set()

    running = common_pb2.RUNNING
    placed = common_pb2.PLACED
    succeeded = common_pb2.SUCCEEDED
    for ev in job.events():
        name = getattr(ev, "task_name", "") or ""
        if name not in by_name:
            continue
        cp = by_name[name]
        state = getattr(ev, "state", None)
        detail = (getattr(ev, "detail", "") or "").strip()
        if state in (running, placed) and name not in started:
            started.add(name)
            # detail sometimes carries placer hint; result.executed_by only on finish
            who = detail if detail else "mesh…"
            _print_event("RUNNING", cp.chunk, who, prog)
        if state == succeeded and not cp.done:
            res = getattr(ev, "result", None)
            who_id = getattr(res, "executed_by", "") if res is not None else ""
            cp.executed_by = who_id or cp.executed_by
            cp.done = True
            done_frames += cp.chunk.end - cp.chunk.start + 1
            _print_event(
                "DONE",
                cp.chunk,
                _who(cp.executed_by, names),
                prog,
                extra=f"[{done_frames}/{total} frames]",
            )

    status = job.wait(check=False)
    chunk_failures: list[ChunkTaskFailure] = []
    succeeded_chunks: list[Chunk] = []

    for ch in chunks:
        res = status.results.get(ch.name)
        cp = by_name[ch.name]
        if res is not None and not cp.executed_by:
            cp.executed_by = res.executed_by
        if res is None:
            chunk_failures.append(
                ChunkTaskFailure(ch, "no task result from daemon")
            )
            continue
        if res.cancelled:
            chunk_failures.append(ChunkTaskFailure(ch, "cancelled"))
            continue
        if res.timed_out:
            chunk_failures.append(ChunkTaskFailure(ch, "timed out"))
            continue
        if res.exit_code != 0:
            err = (res.stderr_head or b"").decode("utf-8", errors="replace").strip()
            # Prefer last lines — Blender often prints a harmless mount warning first.
            if err:
                lines = [ln for ln in err.splitlines() if ln.strip()]
                err = " | ".join(lines[-3:]) if lines else err
            else:
                # Blender reports load errors (e.g. .blend from a newer version) on stdout.
                out = (res.stdout_head or b"").decode("utf-8", errors="replace")
                lines = [ln.strip() for ln in out.splitlines() if ln.strip()]
                errors = [ln for ln in lines if ln.startswith("Error")]
                err = " | ".join((errors or lines)[-3:])
            detail = f"exit {res.exit_code}: {err}" if err else f"exit code {res.exit_code}"
            if res.error:
                detail = f"{detail} ({res.error})"
            chunk_failures.append(ChunkTaskFailure(ch, detail))
            continue
        if res.error:
            chunk_failures.append(ChunkTaskFailure(ch, res.error))
            continue
        succeeded_chunks.append(ch)
        if not cp.done:
            cp.done = True
            done_frames += cp.chunk.end - cp.chunk.start + 1
            _print_event(
                "DONE",
                cp.chunk,
                _who(cp.executed_by, names),
                prog,
                extra=f"[{done_frames}/{total} frames]",
            )

    # Summary by host
    by_host: dict[str, list[str]] = {}
    for cp in progress:
        label = _who(cp.executed_by, names)
        by_host.setdefault(label, []).append(f"{cp.chunk.start}-{cp.chunk.end}")
    print("zm-render: who did what", file=prog, flush=True)
    for host, ranges in sorted(by_host.items()):
        print(f"  {host}: frames {', '.join(ranges)}", file=prog, flush=True)

    out_dir = opts.out_dir.resolve()
    out_dir.mkdir(parents=True, exist_ok=True)

    # Fetch succeeded chunks into a staging dir, then flatten frames into out_dir.
    for ch in succeeded_chunks:
        staging = out_dir / f".chunk_{ch.start}_{ch.end}"
        if staging.exists():
            shutil.rmtree(staging)
        staging.mkdir(parents=True)
        job.fetch(ch.name, staging)
        _flatten_frames(staging, out_dir)
        shutil.rmtree(staging, ignore_errors=True)

    result = collect_frames(out_dir, opts.frames)
    result.progress = progress

    if chunk_failures:
        lines = [
            f"frames {f.chunk.start}-{f.chunk.end}: {f.message}" for f in chunk_failures
        ]
        raise ChunkTasksFailed(
            chunk_failures,
            "chunk task(s) failed:\n  " + "\n  ".join(lines),
        )

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
