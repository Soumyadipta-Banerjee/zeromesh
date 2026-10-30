import json
from types import SimpleNamespace as NS

from zm_cc.report import UnitRecord, explain, task_failure, who, who_did_what, write_report


def res(**kw):
    base = dict(exit_code=0, timed_out=False, cancelled=False, error="", stderr_head=b"", executed_by="")
    base.update(kw)
    return NS(**base)


def test_task_failure_cases():
    assert task_failure(res()) == ""
    assert task_failure(None) == "no task result from daemon"
    assert task_failure(res(cancelled=True)) == "cancelled"
    assert task_failure(res(timed_out=True)) == "timed out"
    assert task_failure(res(exit_code=1, stderr_head=b"a\n\nb\nc\nd\n")) == "exit 1: b | c | d"
    assert task_failure(res(exit_code=2)) == "exit code 2"
    assert task_failure(res(error="refused")) == "refused"


def test_explain_adds_known_hints_only():
    busy = explain("owner busy: donated work is held at comfort level 3")
    assert "max_step = 0" in busy and "ZMF_NO_GOVERNOR=1" in busy
    toolchain = explain("toolchain for 'g++' differs from the requester's")
    assert "--env mirror" in toolchain and "mkroot" in toolchain
    assert explain("exit 1: plain error") == "exit 1: plain error"


def test_peer_names_maps_full_and_short_ids():
    from zm_cc.report import peer_names

    class Client:
        def peers(self):
            return [NS(node_id="0123456789ab", hostname="donor")]

    names = peer_names(Client())
    assert names["0123456789ab"] == "donor" and names["01234567"] == "donor"
    assert who("0123456789ab", names) == "donor (01234567)"
    assert who("ffffffffffff", {}) == "ffffffff"
    assert who("", {}) == "?"


def test_write_report_is_compile_commands_style(tmp_path):
    ok = UnitRecord(index=0, source="a.c", mode="remote", command="cc -c a.i", directory="/w",
                    output="/o/a.o", status="ok", node="donor (01234567)")
    bad = UnitRecord(index=1, source="b.c", mode="local", command="cc -c b.c", directory="/w",
                     status="failed", error="exit 1: nope")
    path = tmp_path / "zm-cc-report.json"
    write_report(path, [ok, bad], total_s=1.5, meta={"env": "host"})
    doc = json.loads(path.read_text(encoding="utf-8"))
    assert doc["tool"] == "zm-cc" and doc["env"] == "host" and doc["failed"] == 1
    assert doc["total_wall_s"] == 1.5
    assert doc["objects"] == {"/o/a.o": "a.c"}
    assert doc["units"][0]["directory"] == "/w" and doc["units"][0]["command"] == "cc -c a.i"


def test_who_did_what_groups_ok_units_by_node():
    recs = [
        UnitRecord(index=0, source="a.c", mode="remote", command="", directory="", status="ok", node="n1"),
        UnitRecord(index=1, source="b.c", mode="remote", command="", directory="", status="ok", node="n1"),
        UnitRecord(index=2, source="c.S", mode="local", command="", directory="", status="ok"),
        UnitRecord(index=3, source="d.c", mode="remote", command="", directory="", status="failed", node="n2"),
    ]
    assert who_did_what(recs) == [("local", ["c.S"]), ("n1", ["a.c", "b.c"])]
