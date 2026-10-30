"""zm-cc command-line entry point.

Three input forms, exactly one per invocation:

  zm-cc --compdb build/compile_commands.json
  zm-cc --cc g++ --flags "-O2 -std=c++20 -Iinclude" src/a.cpp src/b.cpp
  zm-cc -j8 -- gcc -O2 -c foo.c -o foo.o

Everything after the first ``--`` is a command. Everything before it is zm-cc's own options.
"""

from __future__ import annotations

import argparse
import os
import sys
from pathlib import Path
from typing import Any, Optional

from zm_cc.compile import BuildOptions, link_objects, run_build
from zm_cc.plan import (
    Plan,
    PlanError,
    build_plan,
    describe,
    entries_from_command,
    entries_from_files,
    load_compdb,
    make_tasks,
)
from zm_cc.report import UnitRecord, who_did_what, write_report


def build_parser() -> argparse.ArgumentParser:
    p = argparse.ArgumentParser(
        prog="zm-cc",
        description="Distribute compilation across a ZeroMesh team: preprocess locally, compile remotely.",
    )
    p.add_argument("sources", nargs="*", help="source files, compiled with --cc and --flags (form 2)")
    p.add_argument("--compdb", type=Path, metavar="FILE", help="compile_commands.json (form 1)")
    p.add_argument("--cc", default="cc", help="compiler for explicit SOURCES (default: cc)")
    p.add_argument("--flags", default="", help='flags for explicit SOURCES, one string, e.g. "-O2 -Iinclude"')
    p.add_argument(
        "-o",
        "--out",
        type=Path,
        default=None,
        help="object directory (default: zm-objs/). With `-- CMD -o X` and no --out, the object goes to X.",
    )
    p.add_argument("--chunk", type=int, default=1, help="translation units per task (default: 1)")
    p.add_argument(
        "--cores",
        type=int,
        default=2,
        help="cores reserved per task (default: 2). Compilers are mostly single-threaded; "
        "a donor with N cores and --cores 2 runs about (N-1)/2 tasks at once.",
    )
    p.add_argument(
        "-j",
        "--jobs",
        type=int,
        default=0,
        help="local preprocessing and passthrough concurrency (default: CPU count)",
    )
    p.add_argument("--env", choices=("host", "mirror"), default="host", help="env provider (default: host)")
    p.add_argument(
        "--mirror",
        type=Path,
        action="append",
        default=None,
        dest="mirror_paths",
        metavar="PATH",
        help="root directory to mirror for --env mirror (repeatable; see `zm-cc mkroot`)",
    )
    p.add_argument("--ld-library-path", default="", metavar="PATHS", help="LD_LIBRARY_PATH for donor tasks")
    p.add_argument("--remote-only", action="store_true", help="never run remote-eligible units here")
    p.add_argument(
        "--keep-going",
        action="store_true",
        help="do not cancel on the first failure; collect what compiled and exit 0 even if some units failed",
    )
    p.add_argument(
        "--submit-timeout",
        type=float,
        default=1800.0,
        help="seconds allowed for the upload and submit calls (default: 1800)",
    )
    p.add_argument("--link-output", type=Path, default=None, metavar="PATH", help="link the objects into PATH locally")
    p.add_argument("--link-flags", default="", metavar="FLAGS", help="extra flags for the local link")
    p.add_argument("--socket", default=None, help="override the zmd unix socket path")
    p.add_argument("--dry-run", action="store_true", help="print the plan and exit; needs no daemon")
    p.add_argument("-v", "--verbose", action="store_true", help="print each local command")
    return p


def _usage(msg: str) -> int:
    print(f"zm-cc: {msg}", file=sys.stderr)
    return 2


def main(argv: Optional[list[str]] = None) -> int:
    raw = list(sys.argv[1:] if argv is None else argv)
    if raw[:1] == ["mkroot"]:
        from zm_cc.mkroot import main as mkroot_main

        return mkroot_main(raw[1:])

    command: Optional[list[str]] = None
    own = raw
    if "--" in raw:
        i = raw.index("--")
        own, command = raw[:i], raw[i + 1 :]

    try:
        args = build_parser().parse_args(own)
    except SystemExit as e:
        return int(e.code or 0)

    chosen = [args.compdb is not None, bool(args.sources), command is not None]
    if sum(chosen) != 1:
        return _usage("give exactly one of --compdb FILE, SOURCES..., or -- COMMAND")
    if command is not None and not command:
        return _usage("nothing after --")
    if args.chunk < 1 or args.cores < 1:
        return _usage("--chunk and --cores must be >= 1")
    if args.env == "mirror" and not args.mirror_paths:
        return _usage("--env mirror needs at least one --mirror PATH (make one with `zm-cc mkroot`)")

    cwd = Path.cwd()
    out_dir = (args.out or Path("zm-objs")).absolute()
    try:
        if args.compdb is not None:
            entries = load_compdb(args.compdb)
        elif args.sources:
            entries = entries_from_files(args.cc, args.flags, args.sources, cwd)
        else:
            entries = entries_from_command(command or [], cwd)
        plan = build_plan(entries, out_dir, honour_output=command is not None and args.out is None)
    except PlanError as e:
        print(f"zm-cc: {e}", file=sys.stderr)
        return 2
    if not plan.units:
        return _usage("no translation units to build")

    tasks = make_tasks(plan.remote_units, args.chunk)
    if args.dry_run:
        print(describe(plan, tasks, env=args.env, cores=args.cores))
        print(f"(dry run: nothing executed; {len(plan.remote_units)} remote, {len(plan.local_units)} local)")
        return 0

    opts = BuildOptions(
        out_dir=out_dir,
        env=args.env,
        mirror_paths=[str(p.resolve()) for p in (args.mirror_paths or [])],
        ld_library_path=args.ld_library_path,
        cores=args.cores,
        chunk=args.chunk,
        jobs=args.jobs or os.cpu_count() or 4,
        may_run_local=not args.remote_only,
        keep_going=args.keep_going,
        submit_timeout=args.submit_timeout,
        verbose=args.verbose,
    )
    prog = sys.stderr
    print(
        f"zm-cc: {len(plan.units)} unit(s) -> {len(tasks)} remote task(s) "
        f"(chunk={args.chunk}, env={args.env}, cores={args.cores}), {len(plan.local_units)} local",
        file=prog,
        flush=True,
    )
    zm: Any = None  # the SDK module, once imported; needed to classify its errors below
    try:
        if plan.remote_units:
            import zeromesh as zm  # deferred: --dry-run and all-local builds need no SDK
            from zeromesh.v1 import common_pb2

            with zm.connect(socket_path=args.socket) as client:
                records, total = run_build(plan, opts, client, common_pb2=common_pb2, progress=prog)
        else:
            records, total = run_build(plan, opts, None, progress=prog)
    except KeyboardInterrupt:
        print("zm-cc: cancelled", file=sys.stderr)
        return 130
    except ImportError:
        print("zm-cc: zeromesh SDK not installed (pip install ./sdk/python)", file=sys.stderr)
        return 1
    except Exception as e:  # SDK errors are not all one class; report them and stop
        if zm is not None and isinstance(e, zm.DaemonNotRunning):
            print("zmd is not running (start it with: zmd)", file=sys.stderr)
            return 1
        if zm is not None and isinstance(e, zm.ZeroMeshError):
            print(f"zm-cc: {e}", file=sys.stderr)
            return 1
        raise

    return _finish(plan, records, total, args, opts, prog)


def _finish(
    plan: Plan,
    records: list[UnitRecord],
    total: float,
    args: argparse.Namespace,
    opts: BuildOptions,
    prog: Any,
) -> int:
    failed = [r for r in records if r.status == "failed"]
    objects = [r for r in records if r.status == "ok" and r.output]
    report_path = opts.out_dir / "zm-cc-report.json"
    write_report(
        report_path,
        records,
        total_s=total,
        meta={"env": opts.env, "cores": opts.cores, "chunk": opts.chunk, "keep_going": opts.keep_going},
    )

    print("zm-cc: who did what", file=prog, flush=True)
    for node, labels in who_did_what(records):
        print(f"  {node}: {len(labels)} unit(s): {', '.join(labels)}", file=prog, flush=True)

    rc = 0
    if args.link_output is not None:
        if failed:
            print(f"zm-cc: link skipped, {len(failed)} unit(s) failed", file=prog)
            rc = 1
        else:
            code, err = link_objects(plan, records, output=args.link_output, flags=args.link_flags)
            if code != 0:
                print(f"zm-cc: link failed: {err.strip()[-400:]}", file=prog)
                rc = 1
            else:
                print(f"zm-cc: linked {len(objects)} object(s) into {args.link_output}", file=prog)

    print(
        f"zm-cc: {len(objects)}/{len(records)} unit(s) compiled; objects are listed in {report_path} "
        f"(\"objects\"); total {total:.2f}s",
        file=prog,
        flush=True,
    )
    if failed:
        print(f"zm-cc: {len(failed)} unit(s) failed", file=prog)
        if not opts.keep_going:
            rc = 1
    return rc


if __name__ == "__main__":
    raise SystemExit(main())
