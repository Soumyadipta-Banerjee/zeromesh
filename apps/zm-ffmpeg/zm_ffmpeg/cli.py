"""zm-ffmpeg: independent media conversions across a ZeroMesh team, one item per input."""

from __future__ import annotations

import sys
from pathlib import Path
from typing import Sequence

from zeromesh.adapter import Adapter, AdapterError, Env, Item, Out, Policy, Program, Res, run_adapter

from zm_ffmpeg.convert import check_workspace_file, ffmpeg_argv, plan_conversions


class Ffmpeg(Adapter):
    name = "zm-ffmpeg"
    kind = "ffmpeg"
    res = Res(cores=1)
    policy = Policy(keep_going=True)  # inputs are independent; --resume retries only unfinished work

    def __init__(self, options: Sequence[str] = ()):
        self.options = tuple(options)

    def add_args(self, p):
        p.add_argument("inputs", nargs="+", type=Path, help="input files, relative to the current workspace")
        p.add_argument("--suffix", default=".mp4", help="output suffix (default: .mp4)")
        p.add_argument("--ffmpeg", default="ffmpeg", help="FFmpeg on each donor, or a workspace-relative executable")
        p.add_argument("--ffmpeg-version", default="", metavar="CONSTRAINT",
                       help='allowed donor version (default: this exact build; e.g. ">=6")')

    def check(self, a):
        try:
            self.conversions = plan_conversions(a.inputs, a.suffix)
            self.root = Path.cwd().resolve()
            for conversion in self.conversions:
                check_workspace_file(self.root, conversion)
        except ValueError as e:
            raise AdapterError(str(e)) from None

        # A relative ./tool travels inside the workspace. Capture mode instead exercises its
        # version command so the daemon can ship the executable and everything it actually loads.
        relative_tool = "/" in a.ffmpeg and not Path(a.ffmpeg).is_absolute()
        if a.env == "capture":
            self.env = Env.capture([(a.ffmpeg, "-version")])
        elif relative_tool:
            self.env = Env()
        else:
            self.env = Env.host(Program(a.ffmpeg, a.ffmpeg_version))

    def workspace(self, a):
        return str(self.root)

    def items(self, a):
        for conversion in self.conversions:
            yield Item(
                id=conversion.id,
                argv=ffmpeg_argv(a.ffmpeg, conversion, self.options),
                outputs=[Out(conversion.output.as_posix(), dest=conversion.output.as_posix())],
            )


def main(argv: Sequence[str] | None = None) -> None:
    raw = list(sys.argv[1:] if argv is None else argv)
    if "--" in raw:
        split = raw.index("--")
        adapter_args, options = raw[:split], raw[split + 1:]
    else:
        adapter_args, options = raw, []
    raise SystemExit(run_adapter(Ffmpeg(options), adapter_args))


if __name__ == "__main__":
    main()
