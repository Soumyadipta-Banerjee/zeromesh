"""The low-level client: one method per ClientApi RPC, over zmd's unix socket.

Adapters normally use `zeromesh.adapter` and never touch this; it is public for tools that want
runs without the Adapter structure.
"""

import os
import socket
from typing import Iterable, Iterator, List, Optional, Sequence

import grpc

from zeromesh.errors import DaemonNotRunning, ZeroMeshError
from zeromesh.v1 import client_pb2, client_pb2_grpc, common_pb2, run_pb2


def default_socket_path() -> str:
    """Where zmd listens: $ZM_SOCKET (set by `zm` for the adapters it starts), else
    $ZM_RUNTIME_DIR/zmd.sock, $XDG_RUNTIME_DIR/zeromesh/zmd.sock or /tmp/zeromesh-UID/zmd.sock."""
    explicit = os.environ.get("ZM_SOCKET")
    if explicit:
        return explicit
    runtime_dir = os.environ.get("ZM_RUNTIME_DIR")
    if runtime_dir:
        return os.path.join(runtime_dir, "zmd.sock")
    xdg = os.environ.get("XDG_RUNTIME_DIR")
    if xdg:
        return os.path.join(xdg, "zeromesh", "zmd.sock")
    return f"/tmp/zeromesh-{os.getuid()}/zmd.sock"


def _socket_alive(path: str, timeout: float) -> bool:
    s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    try:
        s.settimeout(timeout)
        s.connect(path)
        return True
    except OSError:
        return False
    finally:
        s.close()


def _error(e: grpc.RpcError) -> ZeroMeshError:
    if e.code() == grpc.StatusCode.UNAVAILABLE:
        return DaemonNotRunning()
    return ZeroMeshError(e.details() or str(e.code()))


class Client:
    """A connection to the local zmd. Every method raises ZeroMeshError with zmd's message when the
    call fails, and DaemonNotRunning when zmd went away."""

    def __init__(self, channel: grpc.Channel, socket_path: str):
        self._channel = channel
        self.socket_path = socket_path
        self.stub = client_pb2_grpc.ClientApiStub(channel)

    def close(self) -> None:
        self._channel.close()

    def __enter__(self) -> "Client":
        return self

    def __exit__(self, *exc) -> None:
        self.close()

    def _call(self, rpc, request, timeout: Optional[float] = 60.0):
        try:
            return rpc(request, timeout=timeout)
        except grpc.RpcError as e:
            raise _error(e) from None

    # ---- workspace and environments ----------------------------------------------------------

    def snapshot(self, path: str, force_large: bool = False, list_largest: bool = False,
                 exclude: Sequence[str] = ()) -> client_pb2.SnapshotReply:
        """Snapshots a directory into the store (only changed files are read again). `exclude` holds
        .zmignore-style patterns; the directory's own .zmignore can still re-include ("!...")."""
        req = client_pb2.SnapshotRequest(path=os.path.abspath(path), force_large=force_large, list=list_largest,
                                         exclude=list(exclude))
        return self._call(self.stub.Snapshot, req, timeout=None)

    def capture_env(self, probes: Sequence[Sequence[str]] = (), paths: Sequence[str] = (),
                    cwd: Optional[str] = None) -> client_pb2.CaptureReply:
        """Captures an environment: what each probe command reads and runs, plus `paths` whole.
        Probes run in `cwd` (default: the current directory) and nothing under it is captured."""
        req = client_pb2.CaptureRequest(
            probes=[client_pb2.Probe(argv=list(p)) for p in probes],
            paths=[os.path.abspath(p) for p in paths],
            cwd=os.path.abspath(cwd or os.getcwd()),
        )
        return self._call(self.stub.CaptureEnv, req, timeout=None)

    def get_object(self, digest: common_pb2.Digest) -> bytes:
        """The bytes of one blob in the store (an item's logs, an output kept in the store)."""
        data = bytearray()
        try:
            for chunk in self.stub.GetObjects(common_pb2.Digests(digests=[digest]), timeout=300):
                data += chunk.data
        except grpc.RpcError as e:
            raise _error(e) from None
        return bytes(data)

    # ---- runs --------------------------------------------------------------------------------

    def submit(self, spec: run_pb2.RunSpec) -> str:
        """Starts a run; returns its id."""
        return self._call(self.stub.SubmitRun, spec).run_id

    def add_items(self, run_id: str, items: Iterable[run_pb2.Item]) -> None:
        self._call(self.stub.AddItems, client_pb2.AddItemsRequest(run_id=run_id, items=list(items)))

    def close_run(self, run_id: str) -> None:
        """No more items for an open run (a session)."""
        self._call(self.stub.CloseRun, client_pb2.RunRef(run_id=run_id))

    def watch(self, run_id: str, from_start: bool = True) -> Iterator[run_pb2.RunEvent]:
        """The run's events until its last one (`finished`). With `from_start`, what already happened
        is replayed first."""
        try:
            yield from self.stub.WatchRun(client_pb2.WatchRequest(run_id=run_id, from_start=from_start))
        except grpc.RpcError as e:
            raise _error(e) from None

    def get_run(self, run_id: str) -> run_pb2.RunStatus:
        return self._call(self.stub.GetRun, client_pb2.RunRef(run_id=run_id))

    def get_report(self, run_id: str) -> run_pb2.Report:
        return self._call(self.stub.GetReport, client_pb2.RunRef(run_id=run_id))

    def cancel(self, run_id: str) -> None:
        """Stops a run; finished items keep their results."""
        self._call(self.stub.CancelRun, client_pb2.RunRef(run_id=run_id))

    def resume(self, run_id: str) -> str:
        """Submits a past run again; finished items come from the cache. Returns the new run's id."""
        return self._call(self.stub.ResumeRun, client_pb2.RunRef(run_id=run_id)).run_id

    def list_runs(self) -> List[run_pb2.RunStatus]:
        return list(self._call(self.stub.ListRuns, common_pb2.Empty()).runs)

    def forget_cache(self, run_id: str = "", all: bool = False, team: bool = False) -> int:
        """Forgets cached results (of one run's items, or all); returns how many."""
        req = client_pb2.ForgetRequest(run_id=run_id, all=all, team=team)
        return self._call(self.stub.ForgetCache, req).forgotten

    # ---- node and team -----------------------------------------------------------------------

    def peers(self) -> List[common_pb2.PeerInfo]:
        return list(self._call(self.stub.ListPeers, common_pb2.Empty()).peers)

    def doctor(self) -> List[client_pb2.Finding]:
        return list(self._call(self.stub.Doctor, common_pb2.Empty(), timeout=None).findings)

    def team_info(self) -> client_pb2.TeamInfoReply:
        return self._call(self.stub.TeamInfo, common_pb2.Empty())

    def team_create(self) -> str:
        """Creates a team; returns its join phrase."""
        return self._call(self.stub.TeamCreate, common_pb2.Empty()).phrase

    def team_join(self, phrase: str) -> None:
        self._call(self.stub.TeamJoin, client_pb2.TeamJoinRequest(phrase=phrase))

    def team_leave(self) -> None:
        self._call(self.stub.TeamLeave, common_pb2.Empty())


def connect(socket_path: Optional[str] = None, timeout: float = 3.0) -> Client:
    """Connects to zmd at `socket_path` (default: default_socket_path()). Raises DaemonNotRunning when
    nothing answers there."""
    path = socket_path or default_socket_path()
    if not _socket_alive(path, min(timeout, 1.0)):
        raise DaemonNotRunning()
    channel = grpc.insecure_channel(f"unix:{path}")
    try:
        grpc.channel_ready_future(channel).result(timeout=timeout)
    except Exception:
        channel.close()
        raise DaemonNotRunning() from None
    return Client(channel, path)
