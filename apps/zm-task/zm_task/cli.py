"""zm-task command-line entry point."""

from __future__ import annotations

import argparse
import sys
from pathlib import Path

from zm_task.tasks import parse_run_script
from zm_task.runner import RunOptions, run_tasks


def build_parser() -> argparse.ArgumentParser:
    p = argparse.ArgumentParser(
        prog="zm-task",
        description=(
            "Distribute a task folder across a ZeroMesh team.\n\n"
            "The task folder must contain a run.sh where each line is an\n"
            "independent command to run in parallel across the mesh.\n"
            "Any other files in the folder are uploaded alongside.\n\n"
            "Example run.sh:\n"
            "  python train.py --shard 0\n"
            "  python train.py --shard 1\n"
            "  python train.py --shard 2\n"
        ),
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    p.add_argument(
        "task_dir",
        type=Path,
        help="Path to the task folder (must contain run.sh)",
    )
    p.add_argument(
        "-o",
        "--output",
        type=Path,
        default=Path("output"),
        help="Directory for collected outputs (default: output/)",
    )
    p.add_argument(
        "--outputs",
        default="out/",
        help="Comma-separated output paths to collect from each task "
        "(default: out/). Each task should write results here.",
    )
    p.add_argument(
        "--group-size",
        type=int,
        default=1,
        help="Commands per group — sequential within a group, groups "
        "run in parallel (default: 1 = max parallelism)",
    )
    p.add_argument(
        "--env",
        choices=("host", "mirror"),
        default="host",
        help="Env provider: host (software on donor) or mirror (M5c)",
    )
    p.add_argument(
        "--mirror",
        type=Path,
        action="append",
        default=None,
        dest="mirror_paths",
        metavar="PATH",
        help="Host path to export for --env mirror (repeatable)",
    )
    p.add_argument(
        "--cores",
        type=int,
        default=1,
        help="Cores reserved per task group (default: 1)",
    )
    p.add_argument(
        "--remote-only",
        action="store_true",
        help="Only run on peer donors (skip local execution)",
    )
    p.add_argument(
        "--local-only",
        action="store_true",
        help="Only run locally (skip remote donors)",
    )
    p.add_argument(
        "--shell",
        default="/bin/sh",
        help="Shell to use on donors (default: /bin/sh)",
    )
    p.add_argument(
        "--socket",
        default=None,
        help="Override zmd unix socket path",
    )
    p.add_argument(
        "--dry-run",
        action="store_true",
        help="Parse run.sh and print tasks without submitting",
    )
    return p


def main(argv: list[str] | None = None) -> int:
    args = build_parser().parse_args(argv)

    task_dir = args.task_dir.resolve()
    if not task_dir.is_dir():
        print(f"zm-task: not a directory: {task_dir}", file=sys.stderr)
        return 2

    run_sh = task_dir / "run.sh"
    if not run_sh.is_file():
        print(f"zm-task: run.sh not found in {task_dir}", file=sys.stderr)
        return 2

    try:
        tasks = parse_run_script(run_sh)
    except Exception as e:
        print(f"zm-task: failed to parse run.sh: {e}", file=sys.stderr)
        return 2

    if not tasks:
        print("zm-task: run.sh has no executable commands", file=sys.stderr)
        return 2

    if args.group_size < 1:
        print("zm-task: --group-size must be >= 1", file=sys.stderr)
        return 2

    if args.env == "mirror" and not args.mirror_paths:
        print("zm-task: --env mirror requires at least one --mirror PATH", file=sys.stderr)
        return 2

    if args.remote_only and args.local_only:
        print("zm-task: --remote-only and --local-only are mutually exclusive", file=sys.stderr)
        return 2

    # Dry run: just show what would be submitted
    if args.dry_run:
        from zm_task.tasks import split_task_groups

        groups = split_task_groups(tasks, args.group_size)
        print(f"zm-task: {len(tasks)} commands → {len(groups)} groups", file=sys.stderr)
        for gi, group in enumerate(groups):
            print(f"\n  group_{gi}:", file=sys.stderr)
            for t in group:
                print(f"    [{t.label}] {t.command}", file=sys.stderr)
        return 0

    # Connect to zmd
    try:
        import zeromesh
    except ImportError:
        print(
            "zm-task: zeromesh SDK not installed (pip install ./sdk/python)",
            file=sys.stderr,
        )
        return 1

    try:
        client = zeromesh.connect(socket_path=args.socket)
    except zeromesh.DaemonNotRunning:
        print("zmd is not running (start it with: zmd)", file=sys.stderr)
        return 1

    output_paths = [p.strip() for p in args.outputs.split(",") if p.strip()]

    opts = RunOptions(
        task_dir=task_dir,
        tasks=tasks,
        out_dir=args.output,
        env=args.env,
        mirror_paths=list(args.mirror_paths or []),
        cores=args.cores,
        group_size=args.group_size,
        may_run_local=not args.remote_only,
        may_run_remote=not args.local_only,
        shell=args.shell,
        outputs=output_paths,
    )

    from zm_task.runner import TasksFailed

    try:
        with client:
            result = run_tasks(client, opts)
    except FileNotFoundError as e:
        print(f"zm-task: {e}", file=sys.stderr)
        return 1
    except TasksFailed as e:
        print(f"zm-task: {e}", file=sys.stderr)
        return 1
    except zeromesh.TaskFailed as e:
        print(f"zm-task: {e}", file=sys.stderr)
        return e.exit_code or 1
    except KeyboardInterrupt:
        print("zm-task: cancelled", file=sys.stderr)
        return 130

    print(
        f"zm-task: {len(result.tasks_ok)}/{len(tasks)} tasks completed → {opts.out_dir}",
        file=sys.stderr,
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
