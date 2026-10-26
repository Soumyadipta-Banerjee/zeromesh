"""Job representation and control for ZeroMesh."""

import os
import time
from typing import Any, Iterator, List, Optional, Union

import grpc
from zeromesh.errors import DaemonNotRunning, JobCancelled, JobTimeout, TaskFailed
from zeromesh.v1 import client_pb2, common_pb2


class Job:
    """A submitted ZeroMesh job comprising one or more tasks."""

    def __init__(
        self,
        client: Any,
        job_id: str,
        name: str = "",
        spec: Optional[common_pb2.JobSpec] = None,
    ):
        self.client = client
        self.job_id = job_id
        self.name = name
        self.spec = spec
        self._last_status: Optional[common_pb2.JobStatus] = None

    def __repr__(self) -> str:
        return f"<zeromesh.Job id={self.job_id!r} name={self.name!r}>"

    def events(self, timeout: Optional[float] = None) -> Iterator[common_pb2.JobEvent]:
        """Streams events for this job until all tasks finish.

        If interrupted by KeyboardInterrupt, cancels the job and re-raises.
        """
        ref = client_pb2.JobRef(job_id=self.job_id)
        try:
            stream = self.client._stub.WatchJob(ref, timeout=timeout)
            for event in stream:
                yield event
        except KeyboardInterrupt:
            self.cancel()
            raise
        except grpc.RpcError as e:
            if e.code() == grpc.StatusCode.UNAVAILABLE:
                raise DaemonNotRunning() from e
            if e.code() == grpc.StatusCode.CANCELLED:
                return
            raise

    def status(self, timeout: float = 10.0) -> common_pb2.JobStatus:
        """Fetches the latest JobStatus from the daemon."""
        ref = client_pb2.JobRef(job_id=self.job_id)
        try:
            st = self.client._stub.GetJob(ref, timeout=timeout)
            self._last_status = st
            return st
        except grpc.RpcError as e:
            if e.code() == grpc.StatusCode.UNAVAILABLE:
                raise DaemonNotRunning() from e
            raise

    def cancel(self, timeout: float = 10.0) -> None:
        """Cancels this job immediately on the daemon."""
        ref = client_pb2.JobRef(job_id=self.job_id)
        try:
            self.client._stub.CancelJob(ref, timeout=timeout)
        except grpc.RpcError as e:
            if e.code() == grpc.StatusCode.UNAVAILABLE:
                raise DaemonNotRunning() from e
            if e.code() != grpc.StatusCode.NOT_FOUND:
                raise

    def wait(
        self,
        timeout: Optional[float] = None,
        check: bool = True,
        poll_interval: float = 0.05,
    ) -> common_pb2.JobStatus:
        """Waits for the job to complete.

        Args:
            timeout: Maximum seconds to wait. None for unlimited.
            check: If True (default), raises TaskFailed if any task failed with
                   a non-zero exit code.
            poll_interval: Polling sleep duration if watching stream closes early.

        Returns:
            The final JobStatus.
        """
        deadline = (time.monotonic() + timeout) if timeout is not None else None

        try:
            ref = client_pb2.JobRef(job_id=self.job_id)
            stream_timeout = timeout
            stream = self.client._stub.WatchJob(ref, timeout=stream_timeout)
            for _ in stream:
                if deadline and time.monotonic() > deadline:
                    raise TimeoutError(f"Job {self.job_id} timed out after {timeout}s")
        except KeyboardInterrupt:
            self.cancel()
            raise
        except grpc.RpcError as e:
            if e.code() == grpc.StatusCode.UNAVAILABLE:
                raise DaemonNotRunning() from e
            if e.code() not in (grpc.StatusCode.CANCELLED, grpc.StatusCode.DEADLINE_EXCEEDED):
                pass

        st = self.status()
        while not st.done:
            if deadline and time.monotonic() > deadline:
                raise TimeoutError(f"Job {self.job_id} timed out after {timeout}s")
            time.sleep(poll_interval)
            st = self.status()

        if check:
            for task_name, res in st.results.items():
                if res.exit_code != 0:
                    raise TaskFailed(
                        task_name=task_name,
                        exit_code=res.exit_code,
                        stderr_head=res.stderr_head,
                        result=res,
                    )
                if res.timed_out:
                    raise JobTimeout(f"Task '{task_name}' timed out")
                if res.cancelled:
                    raise JobCancelled(f"Task '{task_name}' was cancelled")
                if res.error:
                    raise TaskFailed(
                        task_name=task_name,
                        exit_code=res.exit_code if res.exit_code != 0 else 1,
                        stderr_head=res.stderr_head,
                        result=res,
                        message=f"Task '{task_name}' error: {res.error}",
                    )

        return st

    def fetch(
        self,
        task: Union[str, common_pb2.TaskSpec, common_pb2.TaskResult],
        dest_path: Union[str, os.PathLike],
        only_changed: bool = False,
        timeout: float = 30.0,
    ) -> List[str]:
        """Fetches outputs declared by a completed task to a local directory.

        Args:
            task: Task name string, TaskSpec, or TaskResult.
            dest_path: Directory where outputs should be saved (created if needed).
            only_changed: If True, only fetch files modified during task run.
            timeout: gRPC call timeout in seconds.

        Returns:
            List of paths written to disk.
        """
        if isinstance(task, str):
            task_name = task
        elif hasattr(task, "name") and task.name:
            task_name = task.name
        elif hasattr(task, "task_key") and task.task_key:
            task_name = task.task_key
        else:
            task_name = str(task)

        dest_abs = os.path.abspath(str(dest_path))
        os.makedirs(dest_abs, exist_ok=True)

        req = client_pb2.FetchRequest(
            job_id=self.job_id,
            task_name=task_name,
            dest_path=dest_abs,
            only_changed=only_changed,
        )
        try:
            reply = self.client._stub.FetchOutputs(req, timeout=timeout)
            return list(reply.written)
        except grpc.RpcError as e:
            if e.code() == grpc.StatusCode.UNAVAILABLE:
                raise DaemonNotRunning() from e
            raise
