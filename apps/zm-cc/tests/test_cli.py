from pathlib import Path

import pytest

from zm_cc.cli import main

FIXTURE = Path(__file__).resolve().parent / "data" / "compile_commands.json"


def zmcc(*argv: str) -> int:
    with pytest.raises(SystemExit) as e:
        main(list(argv))
    return e.value.code


def test_dry_run_compdb_prints_plan_and_needs_no_daemon(capsys, tmp_path):
    assert zmcc("--dry-run", "--socket", str(tmp_path / "none.sock"), "--compdb", str(FIXTURE)) == 0
    out = capsys.readouterr().out
    assert "zm-cc: 5 unit(s): 4 on the mesh, 1 here" in out
    assert "here   src/asm_stub.c" in out


def test_dry_run_explicit_files_compile_in_full(capsys):
    assert zmcc("--dry-run", "--cc", "g++", "--flags", "-O2 -Iinc", "a.cpp", "b.cpp") == 0
    out = capsys.readouterr().out
    assert "mesh   a.cpp" in out and "mesh   b.cpp" in out
    assert "-Iinc" in out and " -c a.cpp -o a." in out
    assert "-x c++-cpp-output" not in out


def test_dry_run_command_form_honours_o_when_out_not_given(tmp_path, capsys):
    target = tmp_path / "foo.o"
    assert zmcc("--dry-run", "--", "gcc", "-O2", "-c", "foo.c", "-o", str(target)) == 0
    assert f"mesh   foo.c  ->  {target}" in capsys.readouterr().out


def test_dry_run_command_form_with_out_uses_out_dir(tmp_path, capsys):
    assert zmcc("--dry-run", "--out", str(tmp_path / "objs"), "--", "gcc", "-c", "foo.c", "-o", "foo.o") == 0
    assert str(tmp_path / "objs") in capsys.readouterr().out


def test_usage_errors_return_2(tmp_path):
    assert zmcc() == 2  # no input form at all
    assert zmcc("--compdb", str(FIXTURE), "a.c") == 2  # two input forms
    assert zmcc("--dry-run", "--") == 2  # nothing after --
    assert zmcc("--dry-run", "--compdb", str(tmp_path / "missing.json")) == 2
    assert zmcc("--dry-run", "--link-ninja", "build", "a.c") == 2  # needs --compdb and targets
    empty = tmp_path / "cc.json"
    empty.write_text("[]", encoding="utf-8")
    assert zmcc("--dry-run", "--compdb", str(empty)) == 2
