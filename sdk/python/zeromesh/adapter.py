"""Writing an adapter: describe the work as items; the platform does the rest.

    from zeromesh.adapter import Adapter, Item, Out, Env, Res, main

    class Sweep(Adapter):
        name = "zm-sweep"
        kind = "solver"
        env = Env.host("solver", version=">=2")
        res = Res(cores=2, mem="1G")

        def add_args(self, p):
            p.add_argument("cases", nargs="+")

        def items(self, a):
            for case in a.cases:
                yield Item(id=case, argv=["solver", case], outputs=[Out(f"{case}.out")])

    if __name__ == "__main__":
        main(Sweep)

`main` adds the shared flags (zeromesh.cli), snapshots the workspace, resolves the environment,
submits one run, prints each item as it ends, places outputs under --out, writes
<out>/report.json and returns 0 when every item succeeded, 1 when some did not, 2 for a usage error
or a missing daemon and 130 when interrupted (finished items are kept; --resume continues).
"""

import argparse
import os
import re
import sys
from dataclasses import dataclass, field
from typing import Dict, Iterable, List, Optional, Sequence, Union

from zeromesh import cli, report
from zeromesh.client import Client, connect
from zeromesh.errors import AdapterError, DaemonNotRunning, ZeroMeshError
from zeromesh.v1 import run_pb2

_SIZE = re.compile(r"^\s*(\d+(?:\.\d+)?)\s*([kmgt]?)(?:i?b)?\s*$", re.IGNORECASE)


def parse_size(value: Union[str, int, None]) -> int:
    """Bytes in "512M", "2G", "1.5GiB", "4096" (powers of 1024), or an int as is. None is 0."""
    if value is None:
        return 0
    if isinstance(value, int):
        return value
    m = _SIZE.match(str(value))
    if not m:
        raise AdapterError(f"not a size: {value!r} (e.g. 512M, 2G)")
    scale = {"": 1, "k": 1 << 10, "m": 1 << 20, "g": 1 << 30, "t": 1 << 40}[m.group(2).lower()]
    return int(float(m.group(1)) * scale)


# ---- what an item needs ------------------------------------------------------------------------


@dataclass(frozen=True)
class Program:
    """A program the items need from a donor's own install. `version` "" means the exact build
    installed here; otherwise a constraint on what `probe` prints (">=4.0", ">=4.0,<5")."""
    name: str
    version: str = ""
    probe: Sequence[str] = ()


@dataclass(frozen=True)
class Env:
    """Where the items' software comes from. Build one with Env.host, Env.capture or Env.ref."""
    programs: Sequence[Program] = ()
    probes: Sequence[Sequence[str]] = ()  # capture: commands that exercise the software
    paths: Sequence[str] = ()             # capture: files or directories taken whole
    tree: str = ""                        # ref: a captured tree, "sha256:..."
    vars: Dict[str, str] = field(default_factory=dict)
    shared_paths: Sequence[str] = ()      # paths every node already has (NFS): never uploaded

    @staticmethod
    def host(*programs: Union[str, Program], version: str = "", **kw) -> "Env":
        """The donor's own install. Env.host("blender", version=">=4.0"), or several Programs."""
        progs = [p if isinstance(p, Program) else Program(p, version) for p in programs]
        return Env(programs=tuple(progs), **kw)

    @staticmethod
    def capture(probes: Sequence[Sequence[str]] = (), paths: Sequence[str] = (), **kw) -> "Env":
        """Captured here (what the probes read and run, plus paths) and shipped by digest, so
        donors without the software can run the items."""
        return Env(probes=tuple(tuple(p) for p in probes), paths=tuple(paths), **kw)

    @staticmethod
    def ref(tree: str, **kw) -> "Env":
        """A tree captured earlier (`zm env capture` printed it)."""
        return Env(tree=tree if tree.startswith("sha256:") else "sha256:" + tree, **kw)

    def as_capture(self) -> "Env":
        """This environment captured instead: a host environment's programs become probes
        (each program's version probe)."""
        if self.tree:
            raise AdapterError("--env capture: this adapter runs a fixed environment " + self.tree)
        if self.probes or self.paths:
            return self
        if not self.programs:
            raise AdapterError("--env capture: this adapter names no program to capture")
        probes = tuple(tuple(p.probe) if p.probe else (p.name, "--version") for p in self.programs)
        return Env(probes=probes, vars=self.vars, shared_paths=self.shared_paths)

    def resolve(self, client: Client, cwd: str) -> run_pb2.Env:
        out = run_pb2.Env(shared_paths=list(self.shared_paths))
        out.vars.update(self.vars)
        if self.probes or self.paths:
            cap = client.capture_env(self.probes, self.paths, cwd=cwd)
            if cap.note:
                print(f"zm: {cap.note}", file=sys.stderr)
            out.tree.CopyFrom(cap.tree)
        elif self.tree:
            out.tree.hash = self.tree[len("sha256:"):]
        for p in self.programs:
            out.programs.add(name=p.name, version=p.version, probe=list(p.probe))
        return out


@dataclass(frozen=True)
class Res:
    """Per item process. `mem` is a hard limit when set; otherwise the team learns what the kind
    needs and only reserves it."""
    cores: int = 1
    mem: Union[str, int, None] = None
    gpus: int = 0
    arch: str = ""

    def to_proto(self) -> run_pb2.Resources:
        return run_pb2.Resources(cores=self.cores, mem_bytes=parse_size(self.mem), gpus=self.gpus, arch=self.arch)


@dataclass(frozen=True)
class Policy:
    """How a run treats failures. None leaves zmd's default (see run.proto)."""
    retries: Optional[int] = None         # donor lost, sandbox failure (default 3)
    exit_retries: Optional[int] = None    # non-zero exit (default 0)
    timeout: Optional[int] = None         # seconds per item (default 3600)
    hung_after: Optional[int] = None      # seconds without progress (default 600; 0 = never)
    keep_going: Optional[bool] = None     # default: the first failure cancels the run
    local_fallback: Optional[bool] = None # default: an item no donor could run gets a local try
    cache: Optional[bool] = None          # default on
    batch: Optional[int] = None           # items per batch (default: sized by cost and slots)
    deadline: Optional[float] = None      # unix seconds: cancel what is left then
    budget: Optional[float] = None        # core-seconds: cancel what is left after

    def to_proto(self) -> run_pb2.Policy:
        p = run_pb2.Policy()
        for name, proto in (("retries", "infra_retries"), ("exit_retries", "exit_retries"), ("timeout", "timeout_s"),
                            ("hung_after", "hung_after_s"), ("keep_going", "keep_going"),
                            ("local_fallback", "local_fallback"), ("cache", "cache"), ("batch", "batch"),
                            ("budget", "budget_core_s")):
            value = getattr(self, name)
            if value is not None:
                setattr(p, proto, value)
        if self.deadline is not None:
            p.deadline_unix_ms = int(self.deadline * 1000)
        return p


_WHERE = {"anywhere": run_pb2.ANYWHERE, "local": run_pb2.LOCAL_ONLY, "remote": run_pb2.REMOTE_ONLY}


@dataclass(frozen=True)
class Stage:
    """Items that share software, resources and placement; an Item names its stage."""
    name: str = ""
    env: Optional[Env] = None
    res: Res = Res()
    kind: str = ""                       # costs and memory are learned per kind
    combine: Optional[Sequence[str]] = None  # one process for consecutive items: {first} {last} = seq
    warm: bool = False                   # /warm kept per (environment, kind) on each donor
    network: bool = False
    where: str = "anywhere"              # anywhere | local | remote

    def to_proto(self, env: run_pb2.Env) -> run_pb2.Stage:
        if self.where not in _WHERE:
            raise AdapterError(f"stage {self.name!r}: where is anywhere, local or remote, not {self.where!r}")
        s = run_pb2.Stage(name=self.name, env=env, resources=self.res.to_proto(), kind=self.kind,
                          warm=self.warm, network=self.network, where=_WHERE[self.where])
        if self.combine:
            s.combine.argv.extend(self.combine)
        return s


@dataclass(frozen=True)
class Out:
    """An output file or directory, relative to the item's cwd. It is written to `dest`: relative
    to --out (default: the same path under --out), or absolute."""
    path: str
    dest: Optional[str] = None
    required: bool = True

    @staticmethod
    def changed(dest: str = "") -> "Changed":
        """Every file the item creates or modifies under its cwd, written under `dest`."""
        return Changed(dest)


@dataclass(frozen=True)
class Changed:
    dest: str = ""


@dataclass(frozen=True)
class Depfile:
    """The item writes a Make-style depfile (relative to its cwd) listing what it read; the cache
    keys on those files, plus the names in `search_dirs` (a new file there can shadow an old one)."""
    path: str
    search_dirs: Sequence[str] = ()


@dataclass
class Item:
    """One unit of work. `id` must be stable across runs (it names the item in reports, cache
    and --resume)."""
    id: str
    argv: Sequence[str]
    cwd: str = ""                          # relative to the workspace root
    env: Dict[str, str] = field(default_factory=dict)
    inputs: Sequence[str] = ()             # workspace paths it reads; empty = all
    outputs: Sequence[Union[Out, Changed]] = ()
    stage: str = ""
    seq: int = 0                           # position for Stage.combine
    cost: float = 0.0                      # relative size hint for the first run
    after: Sequence[str] = ()              # items that must succeed first
    priority: int = 0
    footprint: Optional[Depfile] = None

    def to_proto(self, out_dir: str) -> run_pb2.Item:
        it = run_pb2.Item(id=self.id, argv=[str(a) for a in self.argv], cwd=self.cwd, inputs=list(self.inputs),
                          stage=self.stage, seq=self.seq, cost=self.cost, after=list(self.after),
                          priority=self.priority)
        it.env.update(self.env)
        for o in self.outputs:
            if isinstance(o, Changed):
                it.changed_dest = os.path.join(out_dir, o.dest)
            else:
                dest = o.dest if o.dest is not None else o.path
                it.outputs.add(path=o.path, dest=os.path.join(out_dir, dest), optional=not o.required)
        if self.footprint:
            it.depfile = self.footprint.path
            it.search_dirs.extend(self.footprint.search_dirs)
        return it


@dataclass(frozen=True)
class Workspace:
    """The directory items see as /work. Files matching .zmignore and the secrets deny list stay out."""
    dir: str
    force_large: bool = False


# ---- the result an adapter's finish() gets -----------------------------------------------------


class Run:
    """A finished run: its status, every item's result and the report zmd wrote."""

    def __init__(self, run_id: str, status: run_pb2.RunStatus, rep: run_pb2.Report, out_dir: str):
        self.id = run_id
        self.status = status
        self.report = rep
        self.out_dir = out_dir
        self.results: Dict[str, run_pb2.ItemResult] = {r.id: r for r in rep.items}

    @property
    def succeeded(self) -> List[str]:
        return [i for i, r in self.results.items() if r.state == run_pb2.ITEM_SUCCEEDED]

    @property
    def failed(self) -> List[run_pb2.ItemResult]:
        return report.failed(self.report)

    def placed(self, item_id: str) -> List[str]:
        """Where the item's outputs were written on this machine."""
        r = self.results.get(item_id)
        return [o.placed for o in r.outputs if o.placed] if r else []


# ---- the base class and main() -----------------------------------------------------------------


class Adapter:
    """Subclass, set the class attributes, implement items(); see the module docstring."""

    name = "zm-adapter"
    kind = ""                       # the default stage's kind
    env: Optional[Env] = None       # the default stage's environment (None: the donor's host)
    res = Res()
    policy = Policy()
    where = "anywhere"
    warm = False
    network = False

    def add_args(self, parser: argparse.ArgumentParser) -> None:
        """Domain flags; the shared ones (zeromesh.cli) are added for you."""

    def check(self, a: argparse.Namespace) -> None:
        """Fail early: raise AdapterError with what to fix."""

    def workspace(self, a: argparse.Namespace) -> Union[str, Workspace]:
        """The directory the items run in (default: the current one)."""
        return os.getcwd()

    def items(self, a: argparse.Namespace) -> Iterable[Item]:
        raise NotImplementedError("an adapter yields its items")

    def combine(self, a: argparse.Namespace) -> Optional[Sequence[str]]:
        """A command that runs consecutive items of the default stage as one process: "{first}" and
        "{last}" become their seq. Outputs are still committed per item."""
        return None

    def stages(self, a: argparse.Namespace) -> List[Stage]:
        """Stages besides the default one ("", built from the class attributes)."""
        return []

    def finish(self, a: argparse.Namespace, run: Run) -> None:
        """Runs here after the run ended, with every result in hand (also when items failed)."""

    # Building the run (also used by --dry-run).
    def default_stage(self, a: argparse.Namespace) -> Stage:
        return Stage(name="", env=self.env, res=self.res, kind=self.kind or self.name, combine=self.combine(a),
                     warm=self.warm, network=self.network, where=self.where)


def _print_event(ev: run_pb2.RunEvent, prog: str) -> None:
    if ev.HasField("log"):
        stream = sys.stderr if ev.log.fd == 2 else sys.stdout
        stream.buffer.write(ev.log.chunk)
        stream.flush()
    elif ev.HasField("result"):
        r = ev.result
        if r.state == run_pb2.ITEM_SUCCEEDED:
            where = "(cached)" if r.cache.hit else "on " + report.node_of(r)
            print(f"  done    {r.id}  {where}", file=sys.stderr)
        else:
            mark = "cancel  " if r.state == run_pb2.ITEM_CANCELLED else "FAILED  "
            print(f"  {mark}{report.describe(r)}", file=sys.stderr)
    elif ev.HasField("update") and ev.update.state == run_pb2.ITEM_QUEUED and ev.update.detail:
        print(f"{prog}: {ev.update.id}: {ev.update.detail}", file=sys.stderr)
    elif ev.HasField("notice"):
        print(f"{prog}: {ev.notice.text}", file=sys.stderr)


def follow(client: Client, run_id: str, prog: str) -> Optional[run_pb2.RunStatus]:
    """Prints a run's items as they end until it finishes; Ctrl-C cancels it (finished items are
    kept). Returns the final status, or None when zmd went away."""
    status = None
    try:
        for ev in client.watch(run_id):
            _print_event(ev, prog)
            if ev.HasField("finished"):
                status = ev.finished
    except KeyboardInterrupt:
        print(f"\n{prog}: cancelling; finished items are kept (resume: {prog} --resume {run_id})", file=sys.stderr)
        client.cancel(run_id)
        for ev in client.watch(run_id, from_start=False):
            if ev.HasField("finished"):
                status = ev.finished
        raise
    return status


def _build(adapter: Adapter, a: argparse.Namespace, client: Optional[Client], ws_dir: str):
    """(stages, items, policy) as protos; environments are resolved only with a client."""
    stages = [adapter.default_stage(a)] + list(adapter.stages(a))
    stages = [cli.apply_to_stage(a, s) for s in stages]
    names = [s.name for s in stages]
    if len(set(names)) != len(names):
        raise AdapterError("two stages share a name")
    items = list(adapter.items(a))
    if not items:
        raise AdapterError("nothing to do: no items")
    ids = set()
    for it in items:
        if it.id in ids:
            raise AdapterError(f"two items share the id {it.id!r}")
        ids.add(it.id)
        if it.stage not in names:
            raise AdapterError(f"item {it.id!r} names an unknown stage {it.stage!r}")
    out_dir = os.path.abspath(a.out)
    env_protos = [(s.env or Env()).resolve(client, ws_dir) if client else run_pb2.Env() for s in stages]
    stage_protos = [s.to_proto(e) for s, e in zip(stages, env_protos)]
    policy = cli.apply_to_policy(a, adapter.policy).to_proto()
    return stage_protos, [it.to_proto(out_dir) for it in items], policy


def _dry_run(adapter: Adapter, a: argparse.Namespace, stages, items) -> None:
    for s in stages:
        print(f"stage {s.name or '(default)'}: kind={s.kind} cores={s.resources.cores or 1}"
              f"{' combine=' + ' '.join(s.combine.argv) if s.combine.argv else ''}")
    for it in items:
        print(f"  {it.id}: {' '.join(it.argv)}")
    print(f"{len(items)} item(s); outputs under {os.path.abspath(a.out)}")


def run_adapter(adapter: Adapter, argv: Optional[Sequence[str]] = None) -> int:
    parser = argparse.ArgumentParser(prog=adapter.name)
    adapter.add_args(parser)
    cli.add_shared_flags(parser)
    a = parser.parse_args(argv)
    prog = adapter.name
    client = None
    try:
        adapter.check(a)
        if a.dry_run and not a.show_upload:
            ws = adapter.workspace(a)
            ws_dir = ws.dir if isinstance(ws, Workspace) else ws
            stages, items, _ = _build(adapter, a, None, os.path.abspath(ws_dir))
            _dry_run(adapter, a, stages, items)
            return 0
        client = connect(a.socket)
        out_dir = os.path.abspath(a.out)
        os.makedirs(out_dir, exist_ok=True)
        if a.resume:
            run_id = client.resume(a.resume)
            print(f"{prog}: run {run_id} resumes {a.resume}", file=sys.stderr)
        else:
            ws = adapter.workspace(a)
            if not isinstance(ws, Workspace):
                ws = Workspace(ws)
            ws_dir = os.path.abspath(ws.dir)
            snap = client.snapshot(ws_dir, force_large=ws.force_large or a.force_large, list_largest=a.show_upload)
            for d in snap.denied:
                print(f"{prog}: kept out (secret): {d}", file=sys.stderr)
            if snap.warning:
                print(f"{prog}: {snap.warning}", file=sys.stderr)
            if a.show_upload:
                print(f"{ws_dir}: {snap.files} files, {snap.total_bytes} bytes")
                for f in snap.largest:
                    print(f"  {f.bytes:>14}  {f.path}")
            stages, items, policy = _build(adapter, a, client, ws_dir)
            if a.dry_run:
                _dry_run(adapter, a, stages, items)
                return 0
            spec = run_pb2.RunSpec(name=prog, workspace=snap.root, workspace_path=ws_dir, stages=stages, items=items,
                                   policy=policy, report_path=os.path.join(out_dir, "report.json"),
                                   stream_logs=a.verbose)
            run_id = client.submit(spec)
            print(f"{prog}: run {run_id}: {len(items)} item(s)", file=sys.stderr)
        try:
            status = follow(client, run_id, prog)
        except KeyboardInterrupt:
            return 130
        if status is None:
            print(f"{prog}: lost the run (zm attach {run_id})", file=sys.stderr)
            return 1
        print(f"{prog}: {report.summary(status)}", file=sys.stderr)
        adapter.finish(a, Run(run_id, status, client.get_report(run_id), out_dir))
        return 0 if status.failed == 0 and status.cancelled == 0 else 1
    except AdapterError as e:
        print(f"{prog}: {e}", file=sys.stderr)
        return 2
    except DaemonNotRunning as e:
        print(f"{prog}: {e}", file=sys.stderr)
        return 2
    except ZeroMeshError as e:
        print(f"{prog}: {e}", file=sys.stderr)
        return 1
    finally:
        if client:
            client.close()


def main(adapter_class, argv: Optional[Sequence[str]] = None) -> None:
    """Runs the adapter as a command and exits with its status."""
    sys.exit(run_adapter(adapter_class(), argv))
