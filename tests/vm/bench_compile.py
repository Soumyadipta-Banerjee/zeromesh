#!/usr/bin/env python3
"""Run paired distcc/zm-cc builds inside the requester VM; emit JSON lines.

The source tree and compilation database must already exist. Both clients receive the same
commands, with LTO disabled (one compiler per donated core). Output stays in a fresh temporary
directory, outside the source tree. DISTCC_FALLBACK=0 makes remote failures visible.
"""

import argparse
import collections
import concurrent.futures
import json
import os
from pathlib import Path
import shlex
import subprocess
import tempfile
import time


def commands(compdb, optimization):
    entries = json.loads(compdb.read_text())
    result = []
    for entry in entries:
        cwd = Path(entry.get("directory", "."))
        if not cwd.is_absolute():
            cwd = compdb.parent / cwd
        argv = entry.get("arguments") or shlex.split(entry["command"])
        while Path(argv[0]).name in ("ccache", "sccache", "distcc"):
            argv = argv[1:]
        normalized = []
        skip = False
        for arg in argv:
            if skip:
                skip = False
                continue
            if arg in ("-o", "-MF", "-MT", "-MQ"):
                skip = True
            elif arg in ("-MD", "-MMD", "-MP") or arg.startswith("-flto"):
                continue
            elif optimization and (arg.startswith("-O") or arg.startswith("-g")):
                continue
            else:
                normalized.append(arg)
        if "-c" not in normalized:
            raise ValueError("benchmark accepts compile-only commands: " + shlex.join(argv))
        if optimization:
            normalized.insert(1, optimization)
        result.append({"directory": str(cwd.resolve()), "file": entry["file"], "arguments": normalized})
    if not result:
        raise ValueError("empty compilation database")
    return result


def cpu_snapshot():
    cpu = Path("/proc/stat").read_text().splitlines()[0].split()[1:]
    daemon = {}
    for proc in Path("/proc").iterdir():
        if not proc.name.isdigit():
            continue
        try:
            if (proc / "comm").read_text().strip() != "zmd":
                continue
            # /proc/PID/stat has a parenthesized command; fields here start at field 3.
            fields = (proc / "stat").read_text().rsplit(")", 1)[1].split()
            daemon = {"pid": int(proc.name), "minor_faults": int(fields[7]),
                      "user_ticks": int(fields[11]), "system_ticks": int(fields[12])}
            break
        except (FileNotFoundError, ProcessLookupError):
            continue
    return {"cpu_ticks": [int(x) for x in cpu], "zmd": daemon}


def build(tool, entries, database, root, out, slots, hosts, label, legacy):
    before = cpu_snapshot()
    start = time.monotonic()
    if tool == "distcc":
        env = dict(os.environ, DISTCC_HOSTS=hosts, DISTCC_FALLBACK="0", CCACHE_DISABLE="1")

        def compile_one(pair):
            i, entry = pair
            argv = ["distcc", *entry["arguments"], "-o", str(out / f"{i}.o")]
            result = subprocess.run(argv, cwd=entry["directory"], env=env, capture_output=True, text=True)
            if result.returncode:
                raise RuntimeError(shlex.join(argv) + "\n" + result.stderr)

        with concurrent.futures.ThreadPoolExecutor(max_workers=slots) as pool:
            list(pool.map(compile_one, enumerate(entries)))
        report = {}
    else:
        argv = ["zm-cc", "--compdb", str(database), "--root", str(root), "-o", str(out)]
        argv += ["--no-retry-local"] if legacy else ["--no-cache"]
        result = subprocess.run(argv, capture_output=True, text=True)
        if result.returncode:
            raise RuntimeError(shlex.join(argv) + "\n" + result.stdout + result.stderr)
        report = json.loads((out / ("zm-cc-report.json" if legacy else "report.json")).read_text())
        if any(item.get("cache_hit") or item.get("cache", {}).get("hit")
               for item in report.get("items", report.get("units", []))):
            raise RuntimeError("cached compilation invalidates the benchmark")
    wall = time.monotonic() - start
    after = cpu_snapshot()
    objects = len(list(out.glob("*.o")))
    if objects != len(entries):
        raise RuntimeError(f"{tool}: {objects} objects, expected {len(entries)}")
    distribution = collections.Counter()
    for item in report.get("items", report.get("units", [])):
        attempts = item.get("attempts", [])
        node = item.get("node") or ((attempts[-1].get("hostname") or attempts[-1].get("node")) if attempts else None)
        if node:
            distribution[node] += 1
    return {"tool": tool, "label": label, "wall_s": wall, "objects": objects,
            "before": before, "after": after, "nodes": dict(distribution),
            "summary": report.get("run", report.get("summary", {})),
            "batches": len({attempt.get("batch") for item in report.get("items", [])
                            for attempt in item.get("attempts", []) if attempt.get("batch")}),
            "retried": sum(len(item.get("attempts", [])) > 1 for item in report.get("items", []))}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--compdb", type=Path, required=True)
    parser.add_argument("--root", type=Path, required=True)
    parser.add_argument("--pairs", type=int, default=10)
    parser.add_argument("--slots", type=int, default=4)
    parser.add_argument("--distcc-hosts", default="127.0.0.1/2 192.168.122.174/2")
    parser.add_argument("--optimization", choices=["O0", "O2", "O3"], default=None,
                        help="override optimization and remove debug flags equally for both tools")
    parser.add_argument("--label", default="New_Arch")
    parser.add_argument("--legacy", action="store_true", help="zm_perf_cc CLI/report; unique unused macro avoids task cache hits")
    args = parser.parse_args()
    if args.pairs < 1 or args.slots < 1:
        parser.error("pairs and slots must be positive")
    entries = commands(args.compdb.resolve(), "-" + args.optimization if args.optimization else None)
    print(json.dumps({"configuration": vars(args) | {"compdb": str(args.compdb), "root": str(args.root)},
                      "units": len(entries), "clock_ticks_per_second": os.sysconf("SC_CLK_TCK"),
                      "first_command": entries[0], "lto": False}), flush=True)
    with tempfile.TemporaryDirectory(prefix="zm-compile-bench-") as scratch:
        base = Path(scratch)
        database = base / "compile_commands.json"
        database.write_text(json.dumps(entries))
        for pair in range(args.pairs + 1):
            # Pair zero warms both tools and their input/toolchain caches. Alternate measured order.
            tools = ("distcc", "zm-cc") if pair % 2 == 0 else ("zm-cc", "distcc")
            pair_entries = entries
            if args.legacy:
                nonce = time.monotonic_ns()
                pair_entries = [entry | {"arguments": [*entry["arguments"], f"-DZEROMESH_BENCHMARK_RUN={nonce}"]}
                                for entry in entries]
                database.write_text(json.dumps(pair_entries))
            for tool in tools:
                out = base / f"{pair}-{tool}"
                out.mkdir()
                result = build(tool, pair_entries, database, args.root.resolve(), out, args.slots,
                               args.distcc_hosts, args.label, args.legacy)
                print(json.dumps(result | {"pair": pair, "warmup": pair == 0}), flush=True)


if __name__ == "__main__":
    main()
