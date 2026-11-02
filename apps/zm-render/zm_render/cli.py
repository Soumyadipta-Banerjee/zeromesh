"""zm-render: a Blender frame range across a ZeroMesh team, one item per frame."""

from __future__ import annotations

import os
import shutil
import sys
import tempfile
from pathlib import Path

from zeromesh.adapter import Adapter, AdapterError, Env, Item, Out, Policy, Program, Res, main as run

from zm_render.blend_paths import warn_external_paths
from zm_render.frames import frame_filename, parse_frames
from zm_render.render import BLENDER_ENV, blender_argv, encode_video


class Render(Adapter):
    name = "zm-render"
    kind = "blender"
    res = Res(cores=4)
    policy = Policy(keep_going=True)  # one bad frame does not stop the others; --resume redoes it

    def add_args(self, p):
        p.add_argument("scene", type=Path, help="the .blend to render")
        p.add_argument("--frames", required=True, help="N or A-B (inclusive)")
        p.add_argument("--blender", default="blender", help="Blender on the donors (default: blender)")
        p.add_argument("--blender-version", default="any", metavar="CONSTRAINT",
                       help='which Blender donors may use, e.g. ">=4.2,<5" (default: any)')
        p.add_argument("--engine", metavar="NAME", help="render engine (-E), e.g. CYCLES; default: the .blend's")
        p.add_argument("--encode", type=Path, metavar="MP4", help="encode the frames here with ffmpeg afterwards")

    def check(self, a):
        try:
            self.frames = parse_frames(a.frames)
        except ValueError as e:
            raise AdapterError(str(e)) from None
        self.scene = a.scene.resolve()
        if not self.scene.is_file():
            raise AdapterError(f"scene not found: {self.scene}")
        warn_external_paths(self.scene, file=sys.stderr)
        self.env = self._env(a)

    def _env(self, a) -> Env:
        if a.env == "capture":
            # Render one real frame here: Blender's scripts, data files and add-ons are read only then.
            # A stable path: the probe's command keys the capture, so an unchanged Blender is reused.
            scratch = os.path.join(tempfile.gettempdir(), f"zm-render-capture-{os.getuid()}")
            os.makedirs(scratch, mode=0o700, exist_ok=True)
            probe_out = os.path.join(scratch, "frame_####")
            first = str(self.frames.start)
            probe = blender_argv(str(self.scene), first, first, a.blender, a.engine, output=probe_out)
            found = shutil.which(a.blender)
            install = os.path.dirname(os.path.realpath(found)) if found else ""
            paths = [install] if install and not install.startswith("/usr/") else []  # e.g. a tarball in /opt
            return Env.capture([probe], paths, vars=BLENDER_ENV)
        if "/" in a.blender and not os.path.isabs(a.blender):
            return Env(vars=BLENDER_ENV)  # a Blender inside the scene directory travels with it
        version = ">=0" if a.blender_version == "any" else a.blender_version
        return Env.host(Program(a.blender, version), vars=BLENDER_ENV)

    def workspace(self, a):
        return str(self.scene.parent)

    def combine(self, a):
        # One Blender for consecutive frames: the scene loads once; each frame is still committed
        # (and retried) on its own as soon as its file is written.
        return blender_argv(self.scene.name, "{first}", "{last}", a.blender, a.engine)

    def items(self, a):
        for n in self.frames.frame_numbers():
            name = frame_filename(n)
            yield Item(id=name[:-len(".png")], seq=n, argv=blender_argv(self.scene.name, str(n), str(n), a.blender,
                                                                       a.engine),
                       outputs=[Out(f"out/{name}", dest=name)])

    def finish(self, a, run):
        if not a.encode:
            return
        if run.failed:
            print(f"zm-render: not encoding {a.encode}: {len(run.failed)} frame(s) missing", file=sys.stderr)
            return
        encode_video(Path(run.out_dir), self.frames, a.encode)
        print(f"zm-render: encoded {a.encode}", file=sys.stderr)


def main(argv: list[str] | None = None) -> None:
    run(Render, argv)


if __name__ == "__main__":
    main()
