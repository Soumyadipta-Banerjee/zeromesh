"""Errors raised by the ZeroMesh Python SDK."""

from typing import Any, Optional

NOT_RUNNING_MESSAGE = "zmd is not running (start it with: zmd)"


class ZeroMeshError(Exception):
    """Base exception for all ZeroMesh SDK errors."""


class DaemonNotRunning(ZeroMeshError):
    """Raised when the zmd daemon is not reachable over its UNIX domain socket."""

    def __init__(self, message: str = NOT_RUNNING_MESSAGE):
        super().__init__(message)


class TaskFailed(ZeroMeshError):
    """Raised when a task finishes with a non-zero exit code or error."""

    def __init__(
        self,
        task_name: str,
        exit_code: int,
        stderr_head: bytes = b"",
        result: Optional[Any] = None,
        message: Optional[str] = None,
    ):
        self.task_name = task_name
        self.exit_code = exit_code
        self.stderr_head = stderr_head
        self.result = result

        if message is None:
            err_text = stderr_head.decode("utf-8", errors="replace").strip()
            if err_text:
                message = f"Task '{task_name}' failed with exit code {exit_code}: {err_text}"
            else:
                message = f"Task '{task_name}' failed with exit code {exit_code}"
        super().__init__(message)


class JobCancelled(ZeroMeshError):
    """Raised when a job or task was cancelled."""


class JobTimeout(ZeroMeshError):
    """Raised when a task or job timed out."""
