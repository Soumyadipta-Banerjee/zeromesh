"""The flags every adapter shares, and how they change the run an adapter describes.

    --env host|capture|sha256:TREE   where the software comes from (every stage)
    --cores N --mem SIZE --gpu N     per item process (every stage)
    --remote-only | --local-only     where items may run
    --keep-going --no-cache --batch N
    --deadline DURATION --budget DURATION   cancel what is left at a time / after that much core time
    --dry-run --show-upload          print the plan / what the snapshot holds, then stop
    --force-large                    snapshot more than zmd's upload limit
    --resume RUN                     submit a past run again; finished items come from the cache
    --socket PATH  -v  -o/--out DIR
"""

import argparse
import dataclasses
import re
import time
from typing import Optional

from zeromesh.errors import AdapterError

_DURATION = re.compile(r"^\s*(\d+(?:\.\d+)?)\s*([smhd]?)\s*$")


def parse_duration(value: str) -> float:
    """Seconds in "90", "90s", "30m", "2h", "1.5d"."""
    m = _DURATION.match(value)
    if not m:
        raise AdapterError(f"not a duration: {value!r} (e.g. 90s, 30m, 2h)")
    return float(m.group(1)) * {"": 1, "s": 1, "m": 60, "h": 3600, "d": 86400}[m.group(2)]


def add_shared_flags(p: argparse.ArgumentParser) -> None:
    g = p.add_argument_group("zeromesh")
    g.add_argument("--env", metavar="host|capture|sha256:TREE",
                   help="where the software comes from: the donors' install, captured here, or a captured tree")
    g.add_argument("--cores", type=int, help="cores per item process")
    g.add_argument("--mem", help="memory limit per item process (e.g. 2G)")
    g.add_argument("--gpu", type=int, help="GPUs per item process")
    where = g.add_mutually_exclusive_group()
    where.add_argument("--remote-only", action="store_true", help="never run items on this machine")
    where.add_argument("--local-only", action="store_true", help="run every item on this machine")
    g.add_argument("--keep-going", action="store_true", help="a failed item does not stop the others")
    g.add_argument("--no-cache", action="store_true", help="run every item, even unchanged ones")
    g.add_argument("--batch", type=int, metavar="N", help="items per batch (default: sized automatically)")
    g.add_argument("--deadline", metavar="DURATION", help="cancel what is left after this long (e.g. 2h)")
    g.add_argument("--budget", metavar="DURATION", help="cancel what is left after this much core time")
    g.add_argument("--dry-run", action="store_true", help="print the items and stop")
    g.add_argument("--show-upload", action="store_true", help="print what the workspace snapshot holds")
    g.add_argument("--force-large", action="store_true", help="snapshot a workspace above the upload limit")
    g.add_argument("--resume", metavar="RUN", help="continue an earlier run")
    g.add_argument("--socket", metavar="PATH", help="zmd's socket (default: $ZM_SOCKET or the usual place)")
    g.add_argument("-v", "--verbose", action="store_true", help="stream every item's output")
    g.add_argument("-o", "--out", default="zm-out", metavar="DIR", help="where outputs and report.json go")


def apply_to_stage(a: argparse.Namespace, stage):
    """The stage with the shared flags applied."""
    from zeromesh.adapter import Env, Res, parse_size  # adapter imports this module

    changes = {}
    if a.env:
        current = stage.env or Env()
        if a.env == "host":
            if current.tree or current.probes or current.paths:
                raise AdapterError("--env host: this adapter's environment is captured, not a host install")
            changes["env"] = current
        elif a.env == "capture":
            changes["env"] = current.as_capture()
        elif a.env.startswith("sha256:"):
            changes["env"] = Env.ref(a.env, vars=current.vars, shared_paths=current.shared_paths)
        else:
            raise AdapterError(f"--env is host, capture or sha256:TREE, not {a.env!r}")
    res: Optional[Res] = None
    if a.cores is not None or a.mem is not None or a.gpu is not None:
        res = dataclasses.replace(stage.res,
                                  cores=a.cores if a.cores is not None else stage.res.cores,
                                  mem=parse_size(a.mem) if a.mem is not None else stage.res.mem,
                                  gpus=a.gpu if a.gpu is not None else stage.res.gpus)
        changes["res"] = res
    if a.remote_only:
        changes["where"] = "remote"
    elif a.local_only:
        changes["where"] = "local"
    return dataclasses.replace(stage, **changes) if changes else stage


def apply_to_policy(a: argparse.Namespace, policy):
    changes = {}
    if a.keep_going:
        changes["keep_going"] = True
    if a.no_cache:
        changes["cache"] = False
    if a.batch is not None:
        changes["batch"] = a.batch
    if a.deadline:
        changes["deadline"] = time.time() + parse_duration(a.deadline)
    if a.budget:
        changes["budget"] = parse_duration(a.budget)
    return dataclasses.replace(policy, **changes) if changes else policy
