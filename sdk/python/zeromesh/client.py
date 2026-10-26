"""Client connection to the ZeroMesh daemon (zmd) via ClientApi."""

import os
import socket
import time
from typing import Any, Dict, Iterable, List, Optional, Union

import grpc
from zeromesh.errors import DaemonNotRunning, ZeroMeshError
from zeromesh.job import Job
from zeromesh.v1 import client_pb2, client_pb2_grpc, common_pb2


def default_socket_path() -> str:
    """Resolves the default UNIX domain socket path for zmd."""
    runtime_dir = os.environ.get("ZM_RUNTIME_DIR")
    if runtime_dir:
        return os.path.join(runtime_dir, "zmd.sock")
    xdg = os.environ.get("XDG_RUNTIME_DIR")
    if xdg:
        return os.path.join(xdg, "zeromesh", "zmd.sock")
    return f"/tmp/zeromesh-{os.getuid()}/zmd.sock"


def is_socket_alive(path: str, timeout: float = 0.5) -> bool:
    """Checks whether a UNIX domain socket exists and accepts connections."""
    if not os.path.exists(path):
        return False
    s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    try:
        s.settimeout(timeout)
        s.connect(path)
        return True
    except OSError:
        return False
    finally:
        s.close()


class Client:
    """High-level client for the ZeroMesh daemon."""

    def __init__(self, channel: grpc.Channel, socket_path: str):
        self._channel = channel
        self._socket_path = socket_path
        self._stub = client_pb2_grpc.ClientApiStub(self._channel)

    @property
    def stub(self) -> client_pb2_grpc.ClientApiStub:
        """Access the underlying low-level ClientApi gRPC stub."""
        return self._stub

    @property
    def socket_path(self) -> str:
        """The UNIX socket path connected to."""
        return self._socket_path

    def close(self) -> None:
        """Closes the gRPC channel."""
        self._channel.close()

    def __enter__(self) -> "Client":
        return self

    def __exit__(self, exc_type, exc_val, exc_tb) -> None:
        self.close()

    def upload(
        self,
        path: Union[str, os.PathLike],
        force_large: bool = False,
        timeout: float = 60.0,
    ) -> common_pb2.Digest:
        """Uploads a local directory tree to the CAS via UploadTree.

        Returns:
            common_pb2.Digest: The root Merkle digest of the uploaded tree.
        """
        abs_path = os.path.abspath(str(path))
        if not os.path.exists(abs_path):
            raise FileNotFoundError(f"Path does not exist: {abs_path}")

        req = client_pb2.UploadTreeRequest(path=abs_path, force_large=force_large)
        try:
            reply = self._stub.UploadTree(req, timeout=timeout)
            return reply.root
        except grpc.RpcError as e:
            if e.code() == grpc.StatusCode.UNAVAILABLE:
                raise DaemonNotRunning() from e
            raise

    def submit(
        self,
        job_spec: common_pb2.JobSpec,
        timeout: float = 10.0,
    ) -> Job:
        """Submits a JobSpec to zmd."""
        try:
            reply = self._stub.SubmitJob(job_spec, timeout=timeout)
            return Job(client=self, job_id=reply.job_id, name=job_spec.name, spec=job_spec)
        except grpc.RpcError as e:
            if e.code() == grpc.StatusCode.UNAVAILABLE:
                raise DaemonNotRunning() from e
            raise

    def map(
        self,
        argv: List[str],
        items: Iterable[Any],
        input_root: Optional[Union[common_pb2.Digest, str]] = None,
        outputs: Optional[List[str]] = None,
        env: str = "host",
        env_params: Optional[Dict[str, str]] = None,
        mirror_paths: Optional[List[str]] = None,
        cores: int = 1,
        mem_bytes: int = 0,
        arch: str = "",
        may_run_local: Optional[bool] = None,
        may_run_remote: Optional[bool] = None,
        max_retries: Optional[int] = None,
        timeout_s: Optional[int] = None,
        allow_network: Optional[bool] = None,
        preemptible: Optional[bool] = None,
        name: str = "map",
    ) -> Job:
        """Maps an execution command across a sequence of items.

        Each item replaces '{}' in argv elements. Creates one task per item.
        """
        digest_root = None
        if isinstance(input_root, common_pb2.Digest):
            digest_root = input_root
        elif isinstance(input_root, str):
            digest_root = common_pb2.Digest(hash=input_root)

        policy_kwargs = {}
        if may_run_local is not None:
            policy_kwargs["may_run_local"] = may_run_local
        if may_run_remote is not None:
            policy_kwargs["may_run_remote"] = may_run_remote
        if max_retries is not None:
            policy_kwargs["max_retries"] = max_retries
        if timeout_s is not None:
            policy_kwargs["timeout_s"] = timeout_s
        if allow_network is not None:
            policy_kwargs["allow_network"] = allow_network
        if preemptible is not None:
            policy_kwargs["preemptible"] = preemptible
        policy = common_pb2.Policy(**policy_kwargs)

        resources = common_pb2.Resources(cores=cores, mem_bytes=mem_bytes, arch=arch)
        params = dict(env_params or {})
        if mirror_paths:
            params["mirror_paths"] = "\n".join(mirror_paths)
        env_spec = common_pb2.EnvSpec(name=env, params=params)
        output_list = list(outputs) if outputs else []

        tasks = []
        for idx, item in enumerate(items):
            item_str = str(item)
            task_argv = [arg.replace("{}", item_str) for arg in argv]
            task_outputs = [out.replace("{}", item_str) for out in output_list]
            task_name = f"task_{idx}"
            task = common_pb2.TaskSpec(
                name=task_name,
                argv=task_argv,
                input_root=digest_root,
                outputs=task_outputs,
                env_spec=env_spec,
                resources=resources,
                policy=policy,
            )
            tasks.append(task)

        job_spec = common_pb2.JobSpec(name=name, tasks=tasks)
        return self.submit(job_spec)

    def peers(self, timeout: float = 10.0) -> List[common_pb2.PeerInfo]:
        """Lists connected and discovered team peers."""
        try:
            reply = self._stub.ListPeers(common_pb2.Empty(), timeout=timeout)
            return list(reply.peers)
        except grpc.RpcError as e:
            if e.code() == grpc.StatusCode.UNAVAILABLE:
                raise DaemonNotRunning() from e
            raise

    def team_info(self, timeout: float = 10.0) -> client_pb2.TeamInfoReply:
        """Returns information about this node's team membership."""
        try:
            return self._stub.TeamInfo(common_pb2.Empty(), timeout=timeout)
        except grpc.RpcError as e:
            if e.code() == grpc.StatusCode.UNAVAILABLE:
                raise DaemonNotRunning() from e
            raise

    def team_create(self, timeout: float = 10.0) -> str:
        """Creates a new team and returns the secret passphrase."""
        try:
            reply = self._stub.TeamCreate(common_pb2.Empty(), timeout=timeout)
            return reply.phrase
        except grpc.RpcError as e:
            if e.code() == grpc.StatusCode.UNAVAILABLE:
                raise DaemonNotRunning() from e
            raise

    def team_join(self, phrase: str, timeout: float = 10.0) -> None:
        """Joins an existing team with passphrase."""
        try:
            self._stub.TeamJoin(client_pb2.TeamJoinRequest(phrase=phrase), timeout=timeout)
        except grpc.RpcError as e:
            if e.code() == grpc.StatusCode.UNAVAILABLE:
                raise DaemonNotRunning() from e
            raise

    def team_leave(self, timeout: float = 10.0) -> None:
        """Leaves the current team."""
        try:
            self._stub.TeamLeave(common_pb2.Empty(), timeout=timeout)
        except grpc.RpcError as e:
            if e.code() == grpc.StatusCode.UNAVAILABLE:
                raise DaemonNotRunning() from e
            raise

    def find_missing(
        self,
        digests: List[common_pb2.Digest],
        timeout: float = 10.0,
    ) -> List[common_pb2.Digest]:
        """Queries the CAS for missing digests."""
        req = common_pb2.Digests(digests=digests)
        try:
            reply = self._stub.FindMissing(req, timeout=timeout)
            return list(reply.digests)
        except grpc.RpcError as e:
            if e.code() == grpc.StatusCode.UNAVAILABLE:
                raise DaemonNotRunning() from e
            raise


def connect(
    socket_path: Optional[str] = None,
    timeout: float = 2.0,
) -> Client:
    """Connects to the local ZeroMesh daemon (zmd).

    Args:
        socket_path: Optional explicit path to zmd.sock. Defaults to standard
                     environment paths ($ZM_RUNTIME_DIR/zmd.sock, etc.).
        timeout: Connection check timeout in seconds.

    Returns:
        Client: Connected client instance.

    Raises:
        DaemonNotRunning: If zmd is not running or socket cannot be reached.
    """
    path = socket_path or default_socket_path()
    if not is_socket_alive(path, timeout=timeout):
        raise DaemonNotRunning()

    channel = grpc.insecure_channel(f"unix:{path}")
    try:
        grpc.channel_ready_future(channel).result(timeout=timeout)
    except grpc.FutureTimeoutError as e:
        channel.close()
        raise DaemonNotRunning() from e
    except Exception as e:
        channel.close()
        raise DaemonNotRunning() from e

    return Client(channel=channel, socket_path=path)
