"""What is specific to Blender: its command line, its environment in a sandbox, and the local
video encode."""

from __future__ import annotations

import subprocess
from pathlib import Path
from typing import Optional

from zm_render.frames import FrameRange

# Where Blender writes frames, relative to the .blend; frame_0001.png and so on.
OUTPUT_PATTERN = "//out/frame_####"

# The sandbox has no PipeWire/Pulse: null audio backends. XDG_RUNTIME_DIR must be mode 0700, so
# not /tmp itself.
BLENDER_ENV = {
    "ALSOFT_DRIVERS": "null",
    "SDL_AUDIODRIVER": "dummy",
    "PIPEWIRE_RUNTIME_DIR": "/tmp/xdg-runtime",
    "XDG_RUNTIME_DIR": "/tmp/xdg-runtime",
}


def blender_argv(blend: str, first: str, last: str, blender_bin: str = "blender", engine: Optional[str] = None,
                 output: str = OUTPUT_PATTERN) -> list[str]:
    """Renders frames first..last of `blend` as PNG to `output`. first/last may be "{first}" and
    "{last}" (the combine template)."""
    # -noaudio: no sound server in the sandbox. No --cycles-device: Blender 5 reads unknown long
    # options as file names. The engine is opt-in (-E).
    argv = [blender_bin, "-b", blend, "-noaudio"]
    if engine:
        argv += ["-E", engine]
    return argv + ["-o", output, "-F", "PNG", "-s", first, "-e", last, "-a"]


def encode_video(frames_dir: Path, frames: FrameRange, dest: Path) -> None:
    """Encodes frame_NNNN.png in `frames_dir` to an mp4 with ffmpeg, here."""
    dest.parent.mkdir(parents=True, exist_ok=True)
    cmd = ["ffmpeg", "-y", "-framerate", "24", "-start_number", str(frames.start), "-i",
           str(frames_dir / "frame_%04d.png"), "-frames:v", str(frames.count), "-c:v", "libx264", "-pix_fmt",
           "yuv420p", str(dest)]
    try:
        subprocess.run(cmd, check=True, capture_output=True, text=True)
    except FileNotFoundError as e:
        raise SystemExit("ffmpeg not found; install it or omit --encode") from e
    except subprocess.CalledProcessError as e:
        raise SystemExit(f"ffmpeg failed: {e.stderr or e.stdout}") from e
