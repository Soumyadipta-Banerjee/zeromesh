"""Sessions: a run whose items are added while it runs (for tools that learn the next work from
earlier results).

    with Session("zm-explore", workspace=".", stages=[Stage(kind="solver")]) as s:
        s.add(Item(id="a", argv=["solver", "a"], outputs=[Out("a.out")]))
        for result in s.as_completed():
            if needs_more(result):
                s.add(Item(...))
            elif done():
                s.close()       # no more items: as_completed() ends when the rest are done

Leaving the `with` block closes the session and waits for its items; an exception inside it
cancels the run instead. A session watches its run from the start: if its process dies without
either, zmd cancels the run a minute later, unless a restarted process attaches to it first.
"""

import os
from typing import Dict, Iterator, Optional, Sequence

from zeromesh.adapter import Env, Item, Policy, Stage
from zeromesh.client import Client, connect
from zeromesh.v1 import run_pb2


class Session:
    def __init__(self, name: str, workspace: str = ".", stages: Sequence[Stage] = (Stage(),),
                 policy: Policy = Policy(), out: str = "zm-out", client: Optional[Client] = None,
                 stream_logs: bool = False):
        self.client = client or connect()
        self._own_client = client is None
        self.out_dir = os.path.abspath(out)
        os.makedirs(self.out_dir, exist_ok=True)
        ws_dir = os.path.abspath(workspace)
        snap = self.client.snapshot(ws_dir)
        spec = run_pb2.RunSpec(
            name=name, workspace=snap.root, workspace_path=ws_dir, policy=policy.to_proto(), open=True,
            report_path=os.path.join(self.out_dir, "report.json"), stream_logs=stream_logs,
            stages=[s.to_proto((s.env or Env()).resolve(self.client, ws_dir)) for s in stages])
        self.id = self.client.submit(spec)
        self._init_state()

    @classmethod
    def attach(cls, run_id: str, client: Optional[Client] = None, out: str = "zm-out") -> "Session":
        """Follows a run that is already going (e.g. after this process restarted)."""
        s = cls.__new__(cls)
        s.client = client or connect()
        s._own_client = client is None
        s.out_dir = os.path.abspath(out)
        s.id = run_id
        s._init_state()
        return s

    def _init_state(self) -> None:
        self.results: Dict[str, run_pb2.ItemResult] = {}
        self.status: Optional[run_pb2.RunStatus] = None
        self._closed = False
        # Held from the start, whether or not anything is read yet: zmd keeps an open run only while
        # a client watches it.
        self._events = self.client.watch(self.id, from_start=True)

    def add(self, *items: Item) -> None:
        self.client.add_items(self.id, [it.to_proto(self.out_dir) for it in items])

    def close(self) -> None:
        """No more items; the run ends once the ones added are done."""
        if not self._closed:
            self.client.close_run(self.id)
            self._closed = True

    def cancel(self) -> None:
        self.client.cancel(self.id)

    def as_completed(self) -> Iterator[run_pb2.ItemResult]:
        """Each item's result as it ends (also those that ended before this call), until the run
        finishes. Items may be added while iterating."""
        for ev in self._events:
            if ev.HasField("result") and ev.result.id not in self.results:
                self.results[ev.result.id] = ev.result
                yield ev.result
            elif ev.HasField("finished"):
                self.status = ev.finished
                return

    def wait(self) -> run_pb2.RunStatus:
        """Closes the session and returns the final status once every item is done."""
        self.close()
        for _ in self.as_completed():
            pass
        return self.status

    def report(self) -> run_pb2.Report:
        return self.client.get_report(self.id)

    def __enter__(self) -> "Session":
        return self

    def __exit__(self, exc_type, exc, tb) -> None:
        try:
            if exc_type is None:
                self.wait()
            else:
                self.cancel()
        finally:
            if self._own_client:
                self.client.close()
