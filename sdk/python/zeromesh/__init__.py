"""ZeroMesh Python SDK.

    zeromesh.adapter   Adapter, Item, Out, Env, Res, Policy, Stage, Workspace, Depfile, main
    zeromesh.session   Session: a run that takes items while it runs
    zeromesh.cli       the flags every adapter shares
    zeromesh.report    the standard report and its words
    zeromesh.client    the low-level client (one method per ClientApi RPC)
"""

from zeromesh import report
from zeromesh.adapter import (Adapter, Changed, Depfile, Env, Item, Out, Policy, Program, Res, Run, Stage,
                              Workspace, main)
from zeromesh.client import Client, connect, default_socket_path
from zeromesh.errors import AdapterError, DaemonNotRunning, ZeroMeshError
from zeromesh.session import Session

__all__ = [
    "Adapter", "AdapterError", "Changed", "Client", "DaemonNotRunning", "Depfile", "Env", "Item", "Out", "Policy",
    "Program", "Res", "Run", "Session", "Stage", "Workspace", "ZeroMeshError", "connect", "default_socket_path",
    "main", "report",
]

__version__ = "0.2.0"
