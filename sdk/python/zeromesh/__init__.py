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
    "common_pb2",
    "connect",
    "default_socket_path",
]

__version__ = "0.1.0"
