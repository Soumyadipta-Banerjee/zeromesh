"""The Python SDK: spec building without a daemon, then adapters, sessions and resume against real zmds."""

import argparse
import json
import os
import re
import shutil
import socket
import sys
import tempfile
import time

import pytest

current_dir = os.path.dirname(os.path.abspath(__file__))
repo_root = os.path.abspath(os.path.join(current_dir, "..", "..", ".."))
for p in (os.path.join(repo_root, "tests", "integration"), os.path.join(repo_root, "sdk", "python")):
    if p not in sys.path:
        sys.path.insert(0, p)

import zmtest  # noqa: E402
import zeromesh  # noqa: E402
from zeromesh import cli, report  # noqa: E402
from zeromesh.adapter import (Adapter, AdapterError, Depfile, Env, Item, Out, Policy, Program, Res, Stage,  # noqa: E402
                              parse_size, run_adapter)
from zeromesh.v1 import run_pb2  # noqa: E402


def free_port() -> int:
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    s.bind(("127.0.0.1", 0))
    port = s.getsockname()[1]
    s.close()
    return port


def wait_until(pred, timeout: float = 15.0, interval: float = 0.2) -> bool:
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if pred():
            return True
        time.sleep(interval)
    return False


def shared_flags(*argv):
    p = argparse.ArgumentParser()
    cli.add_shared_flags(p)
    return p.parse_args(list(argv))


@pytest.fixture
def tmp_dir():
    d = tempfile.mkdtemp(prefix="zmsdk-")
    yield d
    shutil.rmtree(d, ignore_errors=True)


@pytest.fixture(scope="module")
def shared_zmd():
    zmd = zmtest.Zmd()
    yield zmd
    zmd.close()


# ---- without a daemon --------------------------------------------------------------------------


def test_connect_raises_when_nothing_listens(tmp_dir, monkeypatch):
    with pytest.raises(zeromesh.DaemonNotRunning):
        zeromesh.connect(os.path.join(tmp_dir, "none.sock"))
    monkeypatch.setenv("ZM_SOCKET", "/somewhere/zmd.sock")
    assert zeromesh.default_socket_path() == "/somewhere/zmd.sock"
    monkeypatch.delenv("ZM_SOCKET")
    monkeypatch.setenv("ZM_RUNTIME_DIR", "/rt")
    assert zeromesh.default_socket_path() == "/rt/zmd.sock"


def test_sizes_and_durations():
    assert parse_size("512M") == 512 << 20
    assert parse_size("1.5GiB") == int(1.5 * (1 << 30))
    assert parse_size("4096") == 4096
    assert parse_size(None) == 0
    with pytest.raises(AdapterError):
        parse_size("lots")
    assert cli.parse_duration("90") == 90
    assert cli.parse_duration("30m") == 1800
    assert cli.parse_duration("2h") == 7200
    with pytest.raises(AdapterError):
        cli.parse_duration("soon")


def test_policy_sets_only_what_was_given():
    p = Policy(keep_going=True, retries=0, timeout=60).to_proto()
    assert p.HasField("keep_going") and p.keep_going
    assert p.HasField("infra_retries") and p.infra_retries == 0
    assert p.timeout_s == 60
    assert not p.HasField("cache") and not p.HasField("exit_retries")


def test_item_outputs_land_under_out():
    it = Item(id="a", argv=["tool", 3], outputs=[Out("x.png"), Out("log", dest="logs/a.log", required=False),
                                                 Out("/abs.txt", dest="/elsewhere/abs.txt")],
              footprint=Depfile("a.d", search_dirs=["include"]), after=["b"], seq=4).to_proto("/out")
    assert list(it.argv) == ["tool", "3"]
    assert [(o.path, o.dest, o.optional) for o in it.outputs] == [
        ("x.png", "/out/x.png", False), ("log", "/out/logs/a.log", True), ("/abs.txt", "/elsewhere/abs.txt", False)]
    assert it.depfile == "a.d" and list(it.search_dirs) == ["include"]
    assert list(it.after) == ["b"] and it.seq == 4
    assert Item(id="c", argv=["x"], outputs=[Out.changed("res")]).to_proto("/out").changed_dest == "/out/res"


def test_environments():
    host = Env.host("blender", version=">=4.0")
    assert host.programs == (Program("blender", ">=4.0"),)
    assert host.as_capture().probes == (("blender", "--version"),)
    probed = Env.host(Program("cc", probe=("cc", "-dumpversion")))
    assert probed.as_capture().probes == (("cc", "-dumpversion"),)
    assert Env.ref("abc").tree == "sha256:abc"
    with pytest.raises(AdapterError):
        Env.ref("abc").as_capture()
    with pytest.raises(AdapterError):
        Env().as_capture()


def test_shared_flags_change_every_stage_and_the_policy():
    stage = Stage(env=Env.host("solver"), res=Res(cores=1, mem="1G"), kind="k")
    s = cli.apply_to_stage(shared_flags("--env", "capture", "--cores", "4", "--remote-only"), stage)
    assert s.env.probes == (("solver", "--version"),)
    assert s.res.cores == 4 and s.res.mem == "1G" and s.where == "remote"
    s = cli.apply_to_stage(shared_flags("--env", "sha256:ff", "--mem", "2G", "--local-only"), stage)
    assert s.env.tree == "sha256:ff" and s.res.mem == 2 << 30 and s.where == "local"
    assert cli.apply_to_stage(shared_flags(), stage) is stage
    with pytest.raises(AdapterError):
        cli.apply_to_stage(shared_flags("--env", "docker"), stage)
    p = cli.apply_to_policy(shared_flags("--keep-going", "--no-cache", "--batch", "5", "--budget", "1h"), Policy())
    assert p.keep_going and p.cache is False and p.batch == 5 and p.budget == 3600


class Echo(Adapter):
    """Writes one file per name; `exit:N` and `noout` misbehave on purpose."""

    name = "zm-echo"
    kind = "echo"
    policy = Policy(keep_going=True)

    def add_args(self, p):
        p.add_argument("names", nargs="+")

    def check(self, a):
        if "bad" in a.names:
            raise AdapterError("'bad' is not a name")

    def items(self, a):
        for n in a.names:
            if n.startswith("exit:"):
                cmd = f"exit {n[5:]}"
            elif n == "noout":
                cmd = "true"
            else:
                cmd = f'printf "%s %s" "$ZM_ITEM_ID" "$(cat input.txt)" > {n}.txt'
            yield Item(id=n.replace(":", "-"), argv=["sh", "-c", cmd], outputs=[Out(f"{n}.txt")])

    def finish(self, a, run):
        self.run = run


def test_dry_run_needs_no_daemon(tmp_dir, capsys):
    rc = run_adapter(Echo(), ["--dry-run", "--socket", os.path.join(tmp_dir, "none.sock"), "a", "b"])
    assert rc == 0
    out = capsys.readouterr().out
    assert "a: sh -c" in out and "2 item(s)" in out


def test_check_errors_are_usage_errors(tmp_dir, capsys):
    assert run_adapter(Echo(), ["--socket", os.path.join(tmp_dir, "none.sock"), "bad"]) == 2
    assert "'bad' is not a name" in capsys.readouterr().err
    assert run_adapter(Echo(), ["--socket", os.path.join(tmp_dir, "none.sock"), "a"]) == 2  # no daemon


# ---- against zmd -------------------------------------------------------------------------------


def run_echo(zmd, workspace, out, *names, extra=()):
    adapter = Echo()
    old = os.getcwd()
    os.chdir(workspace)
    try:
        rc = run_adapter(adapter, ["--socket", zmd.socket, "-o", out, *extra, *names])
    finally:
        os.chdir(old)
    return rc, getattr(adapter, "run", None)


def test_adapter_runs_items_places_outputs_and_reports(shared_zmd, tmp_dir):
    ws, out = os.path.join(tmp_dir, "ws"), os.path.join(tmp_dir, "out")
    os.makedirs(ws)
    with open(os.path.join(ws, "input.txt"), "w") as f:
        f.write("hello")
    rc, run = run_echo(shared_zmd, ws, out, "a", "b", "c")
    assert rc == 0, shared_zmd.log()
    for n in "abc":
        with open(os.path.join(out, f"{n}.txt")) as f:
            assert f.read() == f"{n} hello"
    assert sorted(run.succeeded) == ["a", "b", "c"] and not run.failed
    assert run.placed("a") == [os.path.join(out, "a.txt")]
    rep = report.load(os.path.join(out, "report.json"))
    assert rep.run.succeeded == 3 and len(rep.items) == 3
    assert os.path.exists(os.path.join(out, "report.trace.json"))

    # Unchanged items in an unchanged workspace are cache hits.
    shutil.rmtree(out)
    rc, run = run_echo(shared_zmd, ws, out, "a", "b", "c")
    assert rc == 0 and run.status.cached == 3
    assert os.path.exists(os.path.join(out, "b.txt"))


def test_failures_carry_their_kind_and_resume_reruns_only_them(shared_zmd, tmp_dir, capsys):
    ws, out = os.path.join(tmp_dir, "ws"), os.path.join(tmp_dir, "out")
    os.makedirs(ws)
    with open(os.path.join(ws, "input.txt"), "w") as f:
        f.write("x")
    rc, run = run_echo(shared_zmd, ws, out, "ok", "exit:3", "noout")
    assert rc == 1
    kinds = {r.id: r.failure.kind for r in run.failed}
    assert kinds == {"exit-3": run_pb2.FAILURE_EXIT, "noout": run_pb2.FAILURE_MISSING_OUTPUT}
    assert {r.id: r.failure.exit_code for r in run.failed}["exit-3"] == 3
    err = capsys.readouterr().err
    assert "FAILED  exit-3: exit" in err and "FAILED  noout: missing output" in err
    r = run_pb2.ItemResult(id="x", failure=run_pb2.Failure(
        kind=run_pb2.FAILURE_EXIT, message="exit 1",
        stderr_tail=b"a.c:4:10: \x1b[01;31mfatal error: \x1b[mx.h: No such file\n    4 | #include <x.h>\n"
                    b"      |          ^~~~~\ncompilation terminated.\n"))
    assert report.describe(r) == "x: exit: exit 1: a.c:4:10: fatal error: x.h: No such file"

    rc, resumed = run_echo(shared_zmd, ws, out, "ok", extra=("--resume", run.id))
    assert rc == 1  # the same two items fail again
    assert resumed.id != run.id and resumed.status.cached == 1 and resumed.status.failed == 2


class Combined(Adapter):
    """Three items, one process: the combine command writes each item's file."""

    name = "zm-combined"
    kind = "combined"

    def combine(self, a):
        return ["sh", "-c", 'for i in $(seq {first} {last}); do echo "combined $i" > f$i.txt; done']

    def items(self, a):
        for i in range(1, 4):
            yield Item(id=f"f{i}", seq=i, argv=["sh", "-c", f'echo "part {i}" > f{i}.txt'], outputs=[Out(f"f{i}.txt")])

    def finish(self, a, run):
        self.run = run


def test_combine_runs_consecutive_items_as_one_process(shared_zmd, tmp_dir):
    out = os.path.join(tmp_dir, "out")
    adapter = Combined()
    old = os.getcwd()
    os.chdir(tmp_dir)
    try:
        rc = run_adapter(adapter, ["--socket", shared_zmd.socket, "-o", out, "--batch", "3", "--no-cache"])
    finally:
        os.chdir(old)
    assert rc == 0, shared_zmd.log()
    for i in range(1, 4):
        with open(os.path.join(out, f"f{i}.txt")) as f:
            assert f.read() == f"combined {i}\n"  # the combine command ran, not each item's argv
    batches = {r.attempts[-1].batch for r in adapter.run.results.values()}
    assert len(batches) == 1, batches


def test_session_takes_items_while_it_runs(shared_zmd, tmp_dir):
    client = zeromesh.connect(shared_zmd.socket)
    out = os.path.join(tmp_dir, "out")
    with zeromesh.Session("zm-explore", workspace=tmp_dir, out=out, client=client,
                          policy=Policy(cache=False)) as s:
        s.add(Item(id="first", argv=["sh", "-c", "echo 1 > n.txt"], outputs=[Out("n.txt", dest="first.txt")]))
        seen = []
        for r in s.as_completed():
            seen.append(r.id)
            if r.id == "first":
                s.add(Item(id="second", argv=["sh", "-c", "echo 2 > n.txt"], outputs=[Out("n.txt", dest="second.txt")]))
                s.close()
    assert seen == ["first", "second"]
    assert s.status.succeeded == 2
    with open(os.path.join(out, "second.txt")) as f:
        assert f.read() == "2\n"
    client.close()


def test_adapter_items_run_on_a_peer_with_remote_only(tmp_dir):
    discovery_port = free_port()

    def env_for(port):
        return {"ZM_NODE_PORT": str(port), "ZM_DISCOVERY_PORT": str(discovery_port)}

    a = zmtest.Zmd(env=env_for(free_port()))
    b = zmtest.Zmd(env=env_for(free_port()))
    try:
        m = re.search(r"'([^']+)'", a.zm("team", "create").stdout)
        assert m
        assert b.zm("team", "join", m.group(1)).returncode == 0
        ca, cb = zeromesh.connect(a.socket), zeromesh.connect(b.socket)
        id_b = next(p.node_id for p in cb.peers() if p.is_self)
        assert wait_until(lambda: any(p.node_id == id_b and p.admitted for p in ca.peers())), "A never admitted B"
        with open(os.path.join(tmp_dir, "input.txt"), "w") as f:
            f.write("remote")
        rc, run = run_echo(a, tmp_dir, os.path.join(tmp_dir, "out"), "r1", "r2", extra=("--remote-only",))
        assert rc == 0, a.log() + b.log()
        assert {r.attempts[-1].node for r in run.results.values()} == {id_b}
        with open(os.path.join(tmp_dir, "out", "r2.txt")) as f:
            assert f.read() == "r2 remote"
        doc = json.load(open(os.path.join(tmp_dir, "out", "report.json")))
        assert doc["run"]["name"] == "zm-echo"
        ca.close()
        cb.close()
    finally:
        a.close()
        b.close()
