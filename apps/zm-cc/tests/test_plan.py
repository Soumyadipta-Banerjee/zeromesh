import json
import shlex
from pathlib import Path
from unittest import mock

import pytest

from zm_cc.plan import (
    Entry,
    PlanError,
    build_plan,
    describe,
    entries_from_command,
    entries_from_files,
    infer_workspace_root,
    load_compdb,
    resolve_remote,
)

DATA = Path(__file__).resolve().parent / "data"
FIXTURE = DATA / "compile_commands.json"


def fake_which(name: str):
    return f"/usr/bin/{name}"


@pytest.fixture
def compdb_plan(tmp_path):
    return build_plan(load_compdb(FIXTURE), tmp_path / "objs", which=fake_which)


def test_load_compdb_accepts_command_and_arguments_forms():
    entries = load_compdb(FIXTURE)
    assert len(entries) == 5
    assert entries[0].argv[0] == "cc"
    assert "-DGREETING=hi there" in entries[0].argv  # shlex strips the quotes
    assert entries[1].argv[:2] == ("c++", "-O2")  # the arguments form
    assert entries[2].directory == DATA / "src"  # relative "src" joins the compdb's own directory


def test_load_compdb_rejects_bad_files(tmp_path):
    bad = tmp_path / "cc.json"
    bad.write_text(json.dumps([{"directory": "."}]), encoding="utf-8")
    with pytest.raises(PlanError, match="no 'file'"):
        load_compdb(bad)
    bad.write_text("{}", encoding="utf-8")
    with pytest.raises(PlanError, match="expected a JSON list"):
        load_compdb(bad)
    bad.write_text(json.dumps([{"file": "a.c", "directory": "."}]), encoding="utf-8")
    with pytest.raises(PlanError, match="neither 'command' nor 'arguments'"):
        load_compdb(bad)
    with pytest.raises(PlanError, match="cannot read"):
        load_compdb(tmp_path / "missing.json")


def test_compdb_plan_splits_remote_and_local(compdb_plan):
    remote = [u.label for u in compdb_plan.remote_units]
    local = [u.label for u in compdb_plan.local_units]
    assert remote == ["src/hello.c", "src/util.cc", "main.cpp", "src/kernel.F90"]
    assert local == ["src/asm_stub.c"]
    assert "-S needs the whole command" in compdb_plan.local_units[0].reason


def test_remote_argv_is_the_full_compile_with_zm_ccs_object_and_depfile(compdb_plan):
    hello = compdb_plan.remote_units[0]
    argv = hello.remote_argv(DATA)
    assert argv[0] == "/usr/bin/cc"
    for kept in ("-O2", "-g", "-std=c11", "-Iinclude", "-DGREETING=hi there"):
        assert kept in argv
    assert "build/hello.d" not in argv and "build/hello.o" not in argv  # the command's own -MF/-o are dropped
    assert argv[argv.index("-MF") + 1] == hello.depfile_name
    assert argv[-4:] == ["-c", "src/hello.c", "-o", hello.object_name]
    assert "-x" not in compdb_plan.remote_units[3].remote_argv(DATA)  # Fortran: the extension decides


def test_local_unit_runs_unchanged_with_resolved_driver(compdb_plan):
    local = compdb_plan.local_units[0]
    assert local.local_argv() == ["/usr/bin/cc", "-S", "-O2", "src/asm_stub.c", "-o", "build/asm_stub.s"]
    assert local.object_path is None  # -S writes assembly, not an object


def test_object_names_are_unique_and_under_out_dir(compdb_plan):
    objs = [u.object_path for u in compdb_plan.units if u.object_path is not None]
    assert len(objs) == len(set(objs)) == 4
    assert all(p.parent.name == "objs" for p in objs)


def test_same_source_different_flags_is_a_collision(tmp_path):
    entries = [
        Entry(tmp_path, ("cc", "-O1", "-c", "a.c", "-o", "a.o")),
        Entry(tmp_path, ("cc", "-O2", "-c", "a.c", "-o", "a.o")),
    ]
    with pytest.raises(PlanError, match="two different commands"):
        build_plan(entries, tmp_path / "out", which=fake_which)


def test_identical_entries_are_deduplicated(tmp_path):
    entry = Entry(tmp_path, ("cc", "-O2", "-c", "a.c", "-o", "a.o"))
    plan = build_plan([entry, entry], tmp_path / "out", which=fake_which)
    assert len(plan.units) == 1


def test_command_form_honours_output_for_one_tu(tmp_path):
    entries = entries_from_command(["gcc", "-O2", "-c", "foo.c", "-o", "foo.o"], tmp_path)
    plan = build_plan(entries, tmp_path / "zm-objs", honour_output=True, which=fake_which)
    assert plan.units[0].object_path == (tmp_path / "foo.o")


def test_command_form_without_honour_uses_out_dir(tmp_path):
    entries = entries_from_command(["gcc", "-c", "foo.c", "-o", "foo.o"], tmp_path)
    plan = build_plan(entries, tmp_path / "zm-objs", which=fake_which)
    assert plan.units[0].object_path.parent == tmp_path / "zm-objs"


def test_command_form_link_is_local_with_no_object(tmp_path):
    plan = build_plan(entries_from_command(["gcc", "a.o", "b.o", "-o", "prog"], tmp_path), tmp_path / "o",
                      which=fake_which)
    (unit,) = plan.units
    assert not unit.remote and unit.object_path is None
    assert "no -c" in unit.reason


def test_explicit_files_build_one_unit_per_file(tmp_path):
    entries = entries_from_files("g++", "-O2 -std=c++20 -Iinclude", ["a.cpp", "b.cpp"], tmp_path)
    assert entries[0].argv == ("g++", "-O2", "-std=c++20", "-Iinclude", "-c", "a.cpp")
    plan = build_plan(entries, tmp_path / "o", which=fake_which)
    assert [u.label for u in plan.remote_units] == ["a.cpp", "b.cpp"]
    assert plan.remote_units[0].driver_remote == "/usr/bin/g++"


def test_remote_argv_rewrites_project_paths_relative_to_the_units_directory(tmp_path):
    sub = tmp_path / "src"
    (tmp_path / "include").mkdir()
    sub.mkdir()
    (sub / "a.c").write_text("int a;\n", encoding="utf-8")
    entries = entries_from_files("cc", "-O2 -I../include -isystem /usr/include/x", [str(sub / "a.c")], sub)
    plan = build_plan(entries, tmp_path / "o", which=fake_which)
    root = infer_workspace_root(plan.remote_units)
    assert root == tmp_path.resolve()  # the sibling include directory is part of the project
    u = plan.remote_units[0]
    assert u.working_dir_under(root) == "src"
    argv = u.remote_argv(root)
    assert "-I../include" in argv  # inside the uploaded tree: relative, so it resolves on the donor
    assert argv[argv.index("-isystem") + 1] == "/usr/include/x"  # the donor's own system headers
    assert argv[argv.index("-c") + 1] == "a.c"


def test_infer_workspace_root_explicit_and_common_ancestor(tmp_path):
    a = tmp_path / "a"
    b = tmp_path / "b"
    a.mkdir()
    b.mkdir()
    (a / "x.c").write_text("int x;\n", encoding="utf-8")
    (b / "y.c").write_text("int y;\n", encoding="utf-8")
    entries = entries_from_files("cc", "", ["x.c"], a) + entries_from_files("cc", "", ["y.c"], b)
    plan = build_plan(entries, tmp_path / "o", which=fake_which)
    assert infer_workspace_root(plan.remote_units) == tmp_path.resolve()
    assert infer_workspace_root(plan.remote_units, tmp_path) == tmp_path.resolve()


def test_a_unit_outside_the_root_is_an_error(tmp_path):
    (tmp_path / "a").mkdir()
    plan = build_plan(entries_from_files("cc", "", ["x.c"], tmp_path / "a"), tmp_path / "o", which=fake_which)
    with pytest.raises(PlanError, match="outside --root"):
        plan.remote_units[0].working_dir_under(tmp_path / "elsewhere")


def test_resolve_remote_prefers_absolute_then_path_then_usr_bin():
    assert resolve_remote("/opt/x/gcc", which=lambda n: None) == "/opt/x/gcc"
    assert resolve_remote("gcc", which=lambda n: "/home/me/bin/gcc") == "/home/me/bin/gcc"
    assert resolve_remote("gcc", which=lambda n: None) == "/usr/bin/gcc"


def test_describe_lists_every_unit_and_needs_no_daemon(compdb_plan):
    with mock.patch("subprocess.run", side_effect=AssertionError("dry run must not execute")):
        text = describe(compdb_plan, DATA)
    assert "mesh   src/hello.c" in text
    assert "here   src/asm_stub.c" in text
    assert shlex.join(compdb_plan.remote_units[0].remote_argv(DATA)) in text
