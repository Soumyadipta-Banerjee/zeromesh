"""Frame-range parsing and chunking."""

from __future__ import annotations

from dataclasses import dataclass


@dataclass(frozen=True)
class FrameRange:
    start: int
    end: int  # inclusive

    def __post_init__(self) -> None:
        if self.start < 1:
            raise ValueError(f"frame start must be >= 1, got {self.start}")
        if self.end < self.start:
            raise ValueError(f"frame end {self.end} < start {self.start}")

    @property
    def count(self) -> int:
        return self.end - self.start + 1

    def frame_numbers(self) -> list[int]:
        return list(range(self.start, self.end + 1))


@dataclass(frozen=True)
class Chunk:
    start: int
    end: int  # inclusive

    @property
    def name(self) -> str:
        return f"frames_{self.start}_{self.end}"

    def frame_numbers(self) -> list[int]:
        return list(range(self.start, self.end + 1))


def parse_frames(spec: str) -> FrameRange:
    """Parse ``N`` or ``A-B`` into an inclusive FrameRange."""
    s = spec.strip()
    if not s:
        raise ValueError("empty --frames value")
    if "-" in s:
        left, _, right = s.partition("-")
        if not left.strip() or not right.strip():
            raise ValueError(f"invalid --frames {spec!r}; expected N or A-B")
        try:
            start = int(left.strip())
            end = int(right.strip())
        except ValueError as e:
            raise ValueError(f"invalid --frames {spec!r}; expected N or A-B") from e
        return FrameRange(start, end)
    try:
        n = int(s)
    except ValueError as e:
        raise ValueError(f"invalid --frames {spec!r}; expected N or A-B") from e
    return FrameRange(n, n)


def split_chunks(frames: FrameRange, chunk: int) -> list[Chunk]:
    """Split an inclusive frame range into chunks of at most ``chunk`` frames."""
    if chunk < 1:
        raise ValueError(f"--chunk must be >= 1, got {chunk}")
    out: list[Chunk] = []
    a = frames.start
    while a <= frames.end:
        b = min(a + chunk - 1, frames.end)
        out.append(Chunk(a, b))
        a = b + 1
    return out


def frame_filename(frame: int, prefix: str = "frame_", digits: int = 4, ext: str = "png") -> str:
    """Blender ``####``-style name, e.g. frame_0001.png."""
    return f"{prefix}{frame:0{digits}d}.{ext}"
