"""The standard run report (`<out>/report.json`, written by zmd when a run ends) and the words used
for it everywhere: the same as `zm run` and `zm report` print."""

import re
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


_ANSI = re.compile(r"\x1b\[[0-9;]*[A-Za-z]")


def last_error_line(result: run_pb2.ItemResult) -> str:
    """What the item's stderr says went wrong (colours removed): its last line mentioning an error,
    else its last line; "" when it wrote nothing. Compilers end with source excerpts and a summary
    ("compilation terminated."), so the last line alone rarely says why."""
    text = _ANSI.sub("", result.failure.stderr_tail.decode("utf-8", errors="replace"))
    lines = [ln.strip() for ln in text.splitlines() if ln.strip()]
    errors = [ln for ln in lines if "error" in ln.lower()]
    return (errors or lines or [""])[-1]


def describe(result: run_pb2.ItemResult) -> str:
    """"<id>: <kind>[: <message>][: <last stderr line>]" for an item that did not succeed."""
    f = result.failure
    word = KIND_WORDS.get(f.kind, "failed")
    text = f"{result.id}: {word}"
    if f.message and f.message != word:
        text += f": {f.message}"
    if f.kind == run_pb2.FAILURE_EXIT:
        why = last_error_line(result)
        if why:
            text += f": {why}"
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
