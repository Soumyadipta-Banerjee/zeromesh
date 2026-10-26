"""Unit and integration tests for the ZeroMesh Python SDK."""

import os
import re
import shutil
import socket
import sys
import tempfile
import time
import pytest

# Ensure tests can import zmtest from tests/integration and zeromesh from sdk/python
current_dir = os.path.dirname(os.path.abspath(__file__))
repo_root = os.path.abspath(os.path.join(current_dir, "..", "..", ".."))
integration_dir = os.path.join(repo_root, "tests", "integration")
sdk_dir = os.path.join(repo_root, "sdk", "python")

for p in (integration_dir, sdk_dir):
    if p not in sys.path:
        sys.path.insert(0, p)

import zmtest
import zeromesh
from zeromesh.v1 import common_pb2


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


def test_connect_daemon_not_running(tmp_dir):
    """Verifies that connect() raises DaemonNotRunning when zmd is not running."""
    non_existent = os.path.join(tmp_dir, "nonexistent.sock")
    with pytest.raises(zeromesh.DaemonNotRunning) as exc_info:
        zeromesh.connect(socket_path=non_existent)
    assert "zmd is not running (start it with: zmd)" in str(exc_info.value)

    # Also test default socket resolution when pointing to empty directory
    old_env = os.environ.get("ZM_RUNTIME_DIR")
    try:
        os.environ["ZM_RUNTIME_DIR"] = tmp_dir
        with pytest.raises(zeromesh.DaemonNotRunning) as exc_info:
            zeromesh.connect()
        assert "zmd is not running (start it with: zmd)" in str(exc_info.value)
    finally:
        if old_env is not None:
            os.environ["ZM_RUNTIME_DIR"] = old_env
        else:
            os.environ.pop("ZM_RUNTIME_DIR", None)


def test_upload_map_wait_fetch(shared_zmd, tmp_dir):
    """Tests the full pipeline: upload directory, map tasks, wait for completion, fetch outputs."""
    zm = zeromesh.connect(socket_path=shared_zmd.socket)

    # 1. Create a directory of 3 files
    input_dir = os.path.join(tmp_dir, "input_files")
    os.makedirs(input_dir)
    files = {
        "file1.txt": "12345",              # 5 bytes
        "file2.txt": "abcdefgh",           # 8 bytes
        "file3.txt": "hello world\n",      # 12 bytes
    }
    for fname, content in files.items():
        with open(os.path.join(input_dir, fname), "w") as f:
            f.write(content)

    # 2. Upload tree to CAS
    root_digest = zm.upload(input_dir)
    assert root_digest.hash
    assert root_digest.size_bytes > 0

    # 3. Map command across the 3 files
    # sh -c 'wc -c < {} > {}.len'
    job = zm.map(
        argv=["sh", "-c", "wc -c < {} > {}.len"],
        items=list(files.keys()),
        input_root=root_digest,
        outputs=["{}.len"],
        name="test_wc",
    )

    # 4. Wait for completion
    status = job.wait()
    assert status.done
    assert len(status.results) == 3

    # 5. Fetch and check the 3 results
    fetch_dir = os.path.join(tmp_dir, "fetched_outputs")
    for idx, fname in enumerate(files.keys()):
        task_name = f"task_{idx}"
        written = job.fetch(task_name, fetch_dir)
        len_file = os.path.join(fetch_dir, f"{fname}.len")
        assert os.path.exists(len_file), f"Expected {len_file} to exist in {written}"
        with open(len_file) as f:
            content = f.read().strip()
            expected = str(len(files[fname]))
            assert content == expected, f"File {fname} length mismatch: got {content}, expected {expected}"


def test_task_failed_exit_code(shared_zmd):
    """Verifies that a task exiting non-zero raises TaskFailed with exit_code and stderr."""
    zm = zeromesh.connect(socket_path=shared_zmd.socket)

    job = zm.map(
        argv=["sh", "-c", "echo 'something went wrong' >&2; exit 3"],
        items=[1],
        name="failing_task",
    )

    with pytest.raises(zeromesh.TaskFailed) as exc_info:
        job.wait()

    err = exc_info.value
    assert err.exit_code == 3
    assert b"something went wrong" in err.stderr_head
    assert err.task_name == "task_0"
    assert "exit code 3" in str(err)


def test_cancel_sleep_finishes_within_2s(shared_zmd):
    """Verifies that cancel() on a sleep 30 task finishes within 2 seconds."""
    zm = zeromesh.connect(socket_path=shared_zmd.socket)

    job = zm.map(
        argv=["sleep", "30"],
        items=[1],
        name="long_sleep",
    )

    # Brief delay so daemon queues/places the task
    time.sleep(0.1)

    t0 = time.monotonic()
    job.cancel()
    st = job.wait(check=False)
    elapsed = time.monotonic() - t0

    assert elapsed < 2.0, f"cancel() and wait() took {elapsed:.2f}s, expected < 2.0s"
    assert st.done
    res = st.results["task_0"]
    assert res.cancelled or st.tasks["task_0"] == common_pb2.CANCELLED


def test_two_node_team_remote_execution():
    """Verifies end-to-end execution on a 2-node team with may_run_local=False."""
    discovery_port = free_port()
    port_a = free_port()
    port_b = free_port()

    def env_for(port):
        return {
            "ZM_NODE_PORT": str(port),
            "ZM_DISCOVERY_PORT": str(discovery_port),
        }

    zmd_a = zmtest.Zmd(env=env_for(port_a))
    zmd_b = zmtest.Zmd(env=env_for(port_b))
    try:
        # A creates team
        create = zmd_a.zm("team", "create")
        assert create.returncode == 0, f"team create failed: {create.stderr}"
        m = re.search(r"'([^']+)'", create.stdout)
        assert m, f"no phrase in output: {create.stdout}"
        phrase = m.group(1)

        # B joins team
        join = zmd_b.zm("team", "join", phrase)
        assert join.returncode == 0, f"team join failed: {join.stderr}"

        # Connect SDK clients to node A and node B
        zm_a = zeromesh.connect(socket_path=zmd_a.socket)
        zm_b = zeromesh.connect(socket_path=zmd_b.socket)

        # Get Node B's ID via its ClientApi
        id_b = next(p.node_id for p in zm_b.peers() if p.is_self)

        # Wait until node A sees node B admitted
        def b_admitted():
            peers = zm_a.peers()
            for p in peers:
                if p.node_id == id_b and p.admitted:
                    return True
            return False

        assert wait_until(b_admitted, timeout=15.0), "Node A never admitted Node B"

        # Submit task with may_run_local=False, may_run_remote=True
        job = zm_a.map(
            argv=["sh", "-c", "echo 'remote execution on peer' > out.txt"],
            items=[1],
            outputs=["out.txt"],
            may_run_local=False,
            may_run_remote=True,
            name="remote_job",
        )

        st = job.wait()
        assert st.done
        res = st.results["task_0"]
        assert res.exit_code == 0
        assert res.executed_by == id_b, f"Expected task to execute on peer {id_b}, but got {res.executed_by}"

        # Fetch output from task
        with tempfile.TemporaryDirectory() as out_dir:
            job.fetch("task_0", out_dir)
            out_file = os.path.join(out_dir, "out.txt")
            assert os.path.exists(out_file)
            with open(out_file) as f:
                assert f.read() == "remote execution on peer\n"
    finally:
        zmd_a.close()
        zmd_b.close()


def test_job_events_stream(shared_zmd):
    """Verifies that job.events() streams real-time log chunks and status events."""
    zm = zeromesh.connect(socket_path=shared_zmd.socket)
    job = zm.map(
        argv=["sh", "-c", "echo 'streamed stdout message'"],
        items=[1],
        name="events_test",
    )

    events = list(job.events())
    assert len(events) > 0
    logs = [e.log_chunk for e in events if e.log_chunk]
    combined_log = b"".join(logs).decode("utf-8")
    assert "streamed stdout message" in combined_log

    status = job.wait()
    assert status.done


def test_submit_dag_pipeline(shared_zmd, tmp_dir):
    """Tests low-level submit() with a 3-task DAG with depends_on and inputs_from."""
    zm = zeromesh.connect(socket_path=shared_zmd.socket)
    host = common_pb2.EnvSpec(name="host")

    t_a = common_pb2.TaskSpec(
        name="task_a",
        argv=["sh", "-c", "echo 10 > a.txt"],
        outputs=["a.txt"],
        env_spec=host,
    )
    t_b = common_pb2.TaskSpec(
        name="task_b",
        argv=["sh", "-c", "echo 20 > b.txt"],
        outputs=["b.txt"],
        env_spec=host,
    )
    t_c = common_pb2.TaskSpec(
        name="task_c",
        argv=["sh", "-c", "cat from_a/a.txt from_b/b.txt > combined.txt"],
        outputs=["combined.txt"],
        env_spec=host,
        depends_on=["task_a", "task_b"],
        inputs_from=[
            common_pb2.InputFrom(task="task_a", path="from_a"),
            common_pb2.InputFrom(task="task_b", path="from_b"),
        ],
    )

    spec = common_pb2.JobSpec(name="dag_job", tasks=[t_a, t_b, t_c])
    job = zm.submit(spec)
    st = job.wait()
    assert st.done
    for name in ("task_a", "task_b", "task_c"):
        assert st.results[name].exit_code == 0

    fetch_dir = os.path.join(tmp_dir, "dag_fetch")
    job.fetch("task_c", fetch_dir)
    res_file = os.path.join(fetch_dir, "combined.txt")
    assert os.path.exists(res_file)
    with open(res_file) as f:
        assert f.read() == "10\n20\n"


def test_wait_check_raises_job_cancelled(shared_zmd):
    zm = zeromesh.connect(socket_path=shared_zmd.socket)
    job = zm.map(argv=["sleep", "30"], items=[1], name="cancel_me")
    time.sleep(0.1)
    job.cancel()
    with pytest.raises(zeromesh.JobCancelled):
        job.wait(check=True)


def test_wait_check_raises_job_timeout_on_timed_out_flag():
    """timed_out must win over a non-zero exit_code from the daemon."""
    from unittest import mock

    from zeromesh.job import Job

    res = mock.Mock(
        exit_code=-1,
        timed_out=True,
        cancelled=False,
        error="",
        stderr_head=b"",
    )
    st = mock.Mock(done=True, results={"task_0": res})
    client = mock.Mock()
    client._stub.WatchJob.return_value = iter([])
    client._stub.GetJob.return_value = st

    job = Job(client, "job-timeout")
    with pytest.raises(zeromesh.JobTimeout):
        job.wait(check=True)


def test_wait_check_raises_job_cancelled_on_cancelled_flag():
    from unittest import mock

    from zeromesh.job import Job

    res = mock.Mock(
        exit_code=-1,
        timed_out=False,
        cancelled=True,
        error="",
        stderr_head=b"",
    )
    st = mock.Mock(done=True, results={"task_0": res})
    client = mock.Mock()
    client._stub.WatchJob.return_value = iter([])
    client._stub.GetJob.return_value = st

    job = Job(client, "job-cancel")
    with pytest.raises(zeromesh.JobCancelled):
        job.wait(check=True)


def test_wait_check_false(shared_zmd):
    """Verifies that wait(check=False) returns JobStatus instead of raising on task error."""
    zm = zeromesh.connect(socket_path=shared_zmd.socket)
    job = zm.map(
        argv=["sh", "-c", "exit 42"],
        items=[1],
        name="failing_no_check",
    )
    st = job.wait(check=False)
    assert st.done
    assert st.results["task_0"].exit_code == 42


def test_team_info_and_peers(shared_zmd):
    """Verifies that team_info() and peers() return valid protobuf responses."""
    zm = zeromesh.connect(socket_path=shared_zmd.socket)
    info = zm.team_info()
    assert hasattr(info, "in_team")
    assert hasattr(info, "peers_pinned")

    peers = zm.peers()
    assert isinstance(peers, list)

