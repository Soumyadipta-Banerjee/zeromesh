"""zm-render command-line entry point."""

from __future__ import annotations

import argparse
import sys
from pathlib import Path

from zm_render.frames import parse_frames
from zm_render.render import RenderOptions, parse_dispatch_policy, run_render


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
        "--mirror",
        type=Path,
        action="append",
        default=None,
        dest="mirror_paths",
        metavar="PATH",
        help="Host path to export for --env mirror (repeatable; required with --env mirror)",
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
        "--engine",
        default=None,
        metavar="NAME",
        help="Blender render engine (-E), e.g. CYCLES; default: use the .blend setting",
    )
    p.add_argument(
        "--cores",
        type=int,
        default=0,
        help="Cores per chunk task (default: 0 = full donor slot share). "
        "A positive value only lowers the share.",
    )
    p.add_argument(
        "--policy",
        default="pull",
        metavar="KIND[:N]",
        help="Dispatch policy: pull (default), batch or batch:N, fast, local-first, "
        "spread, or a custom plugin policy name",
    )
    p.add_argument(
        "--max-in-flight",
        type=int,
        default=None,
        metavar="N",
        help="Cap tasks in flight across the mesh (dispatch param max_in_flight)",
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
    if args.env == "mirror" and not args.mirror_paths:
        print("zm-render: --env mirror requires at least one --mirror PATH", file=sys.stderr)
        return 2
    if args.cores < 0:
        print("zm-render: --cores must be >= 0", file=sys.stderr)
        return 2
    try:
        dispatch_kind, dispatch_params = parse_dispatch_policy(args.policy)
    except ValueError as e:
        print(f"zm-render: {e}", file=sys.stderr)
        return 2
    if args.max_in_flight is not None:
        if args.max_in_flight < 1:
            print("zm-render: --max-in-flight must be >= 1", file=sys.stderr)
            return 2
        dispatch_params["max_in_flight"] = str(args.max_in_flight)

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
        mirror_paths=list(args.mirror_paths or []),
        encode=args.encode,
        blender_bin=args.blender,
        engine=args.engine,
        cores=args.cores,
        dispatch_kind=dispatch_kind,
        dispatch_params=dispatch_params,
        may_run_local=not args.remote_only,
        may_run_remote=True,
    )

    from zm_render.render import ChunkTasksFailed, MissingFramesError

    try:
        with client:
            result = run_render(client, opts)
    except FileNotFoundError as e:
        print(f"zm-render: {e}", file=sys.stderr)
        return 1
    except ChunkTasksFailed as e:
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
