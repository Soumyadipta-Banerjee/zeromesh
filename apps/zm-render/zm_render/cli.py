"""zm-render command-line entry point."""

from __future__ import annotations

import argparse
import sys
from pathlib import Path

from zm_render.frames import parse_frames
from zm_render.render import RenderOptions, run_render


def build_parser() -> argparse.ArgumentParser:
    p = argparse.ArgumentParser(
        prog="zm-render",
        description="Render a Blender frame range across a ZeroMesh team.",
    )
    p.add_argument("scene", type=Path, help="Path to SCENE.blend")
    p.add_argument(
        "--frames",
        required=True,
        help="Frame range: N or A-B (inclusive)",
    )
    p.add_argument(
        "--chunk",
        type=int,
        default=1,
        help="Frames per task (default: 1)",
    )
    p.add_argument(
        "-o",
        "--output",
        type=Path,
        default=Path("renders"),
        help="Directory for collected frames (default: renders/)",
    )
    p.add_argument(
        "--env",
        choices=("host", "mirror"),
        default="host",
        help="Env provider: host (Blender on donor) or mirror (M5c)",
    )
    p.add_argument(
        "--encode",
        type=Path,
        default=None,
        help="Optional local ffmpeg encode to this .mp4 path",
    )
    p.add_argument(
        "--blender",
        default="blender",
        help="Blender binary name/path on the donor (default: blender)",
    )
    p.add_argument(
        "--cores",
        type=int,
        default=8,
        help="Cores reserved per chunk (default: 8). Higher ≈ fewer concurrent "
        "Blenders per machine (Flamenco-style: keep peers fed from a queue).",
    )
    p.add_argument(
        "--remote-only",
        action="store_true",
        help="Only run on peer donors (needed when local Blender lives under /opt)",
    )
    p.add_argument(
        "--socket",
        default=None,
        help="Override zmd unix socket path",
    )
    return p


def main(argv: list[str] | None = None) -> int:
    args = build_parser().parse_args(argv)
    try:
        frames = parse_frames(args.frames)
    except ValueError as e:
        print(f"zm-render: {e}", file=sys.stderr)
        return 2

    if args.chunk < 1:
        print("zm-render: --chunk must be >= 1", file=sys.stderr)
        return 2

    try:
        import zeromesh
    except ImportError:
        print(
            "zm-render: zeromesh SDK not installed (pip install ./sdk/python)",
            file=sys.stderr,
        )
        return 1

    try:
        client = zeromesh.connect(socket_path=args.socket)
    except zeromesh.DaemonNotRunning:
        print("zmd is not running (start it with: zmd)", file=sys.stderr)
        return 1

    opts = RenderOptions(
        blend=args.scene,
        frames=frames,
        chunk=args.chunk,
        out_dir=args.output,
        env=args.env,
        encode=args.encode,
        blender_bin=args.blender,
        cores=args.cores,
        may_run_local=not args.remote_only,
        may_run_remote=True,
    )

    from zm_render.render import MissingFramesError

    try:
        with client:
            result = run_render(client, opts)
    except FileNotFoundError as e:
        print(f"zm-render: {e}", file=sys.stderr)
        return 1
    except MissingFramesError as e:
        print(f"zm-render: {e}", file=sys.stderr)
        return 1
    except zeromesh.TaskFailed as e:
        print(f"zm-render: {e}", file=sys.stderr)
        return e.exit_code or 1
    except KeyboardInterrupt:
        print("zm-render: cancelled", file=sys.stderr)
        return 130

    print(
        f"wrote {len(result.frames_ok)} frames to {opts.out_dir}",
        file=sys.stderr,
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
