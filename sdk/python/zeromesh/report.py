"""The standard run report (`<out>/report.json`, written by zmd when a run ends) and the words used
for it everywhere: the same as `zm run` and `zm report` print."""

from typing import List

from google.protobuf import json_format

from zeromesh.v1 import run_pb2

KIND_WORDS = {
    run_pb2.FAILURE_NONE: "",
    run_pb2.FAILURE_EXIT: "exit",
    run_pb2.FAILURE_MISSING_OUTPUT: "missing output",
    run_pb2.FAILURE_OOM: "out of memory",
    run_pb2.FAILURE_TIMEOUT: "timeout",
    run_pb2.FAILURE_HUNG: "hung",
    run_pb2.FAILURE_ENV_UNAVAILABLE: "software unavailable",
    run_pb2.FAILURE_TOOLCHAIN_MISMATCH: "toolchain mismatch",
    run_pb2.FAILURE_SANDBOX: "sandbox",
    run_pb2.FAILURE_DONOR_LEFT: "donor left",
    run_pb2.FAILURE_CANCELLED: "cancelled",
    run_pb2.FAILURE_DEPENDENCY: "dependency failed",
}


def load(path: str) -> run_pb2.Report:
    """Reads a report.json."""
    with open(path) as f:
        return json_format.Parse(f.read(), run_pb2.Report(), ignore_unknown_fields=True)


def describe(result: run_pb2.ItemResult) -> str:
    """"<id>: <kind>[: <message>]" for an item that did not succeed."""
    f = result.failure
    word = KIND_WORDS.get(f.kind, "failed")
    text = f"{result.id}: {word}"
    if f.message and f.message != word:
        text += f": {f.message}"
    return text


def node_of(result: run_pb2.ItemResult) -> str:
    """Where the item's last attempt ran ("" for a cache hit that never ran)."""
    return result.attempts[-1].hostname if result.attempts else ""


def summary(status: run_pb2.RunStatus) -> str:
    """One line: how many items succeeded (cached), failed and were cancelled."""
    return (f"{status.succeeded} succeeded ({status.cached} cached), {status.failed} failed, "
            f"{status.cancelled} cancelled")


def failed(report: run_pb2.Report) -> List[run_pb2.ItemResult]:
    """Items that did not succeed, in report order."""
    return [r for r in report.items if r.state != run_pb2.ITEM_SUCCEEDED]
