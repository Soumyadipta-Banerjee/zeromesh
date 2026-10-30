"""ZeroMesh Python SDK.

Simple, high-performance distributed computing client for ZeroMesh.
"""

from zeromesh.client import Client, connect, default_socket_path
from zeromesh.errors import (
    DaemonNotRunning,
    JobCancelled,
    JobTimeout,
    TaskFailed,
    ZeroMeshError,
)
from zeromesh.job import Job
from zeromesh.units import commit_unit, committed_units, is_committed, unit_result
from zeromesh.v1 import client_pb2, common_pb2

__all__ = [
    "Client",
    "DaemonNotRunning",
    "Job",
    "JobCancelled",
    "JobTimeout",
    "TaskFailed",
    "ZeroMeshError",
    "client_pb2",
    "commit_unit",
    "committed_units",
    "common_pb2",
    "connect",
    "default_socket_path",
    "is_committed",
    "unit_result",
]

__version__ = "0.1.0"
