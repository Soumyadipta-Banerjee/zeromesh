"""Errors raised by the ZeroMesh Python SDK."""

NOT_RUNNING_MESSAGE = "zmd is not running (start it with: zmd)"


class ZeroMeshError(Exception):
    """Base class of every SDK error."""


class DaemonNotRunning(ZeroMeshError):
    """The zmd socket does not answer."""

    def __init__(self, message: str = NOT_RUNNING_MESSAGE):
        super().__init__(message)


class AdapterError(ZeroMeshError):
    """A user mistake an adapter reports (missing file, bad flag value). `main` prints the message
    without a traceback and exits with status 2."""
