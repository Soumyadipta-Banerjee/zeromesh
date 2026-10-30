from pathlib import Path

from zm_cc.cli import main

FIXTURE = Path(__file__).resolve().parent / "data" / "compile_commands.json"


def test_dry_run_compdb_prints_plan_and_exits_zero(capsys):
    rc = main(["--dry-run", "--compdb", str(FIXTURE)])
    out = capsys.readouterr().out
    assert rc == 0
    assert "zm-cc plan: 5 unit(s): 4 remote in 4 task(s)" in out
    assert "local   src/asm_stub.c" in out
    assert "(dry run: nothing executed; 4 remote, 1 local)" in out


def test_dry_run_explicit_files(capsys):
    rc = main(["--dry-run", "--cc", "g++", "--flags", "-O2 -Iinc", "a.cpp", "b.cpp"])
    out = capsys.readouterr().out
    assert rc == 0
    assert "remote  a.cpp" in out and "remote  b.cpp" in out
    assert "-x c++-cpp-output -c" in out


def test_dry_run_command_form_honours_o_when_out_not_given(tmp_path, capsys):
    target = tmp_path / "foo.o"
    rc = main(["--dry-run", "--", "gcc", "-O2", "-c", "foo.c", "-o", str(target)])
    out = capsys.readouterr().out
    assert rc == 0
    assert f"remote  foo.c  ->  {target}" in out


def test_dry_run_command_form_with_out_uses_out_dir(tmp_path, capsys):
    rc = main(["--dry-run", "--out", str(tmp_path / "objs"), "--", "gcc", "-c", "foo.c", "-o", "foo.o"])
    out = capsys.readouterr().out
    assert rc == 0
    assert str(tmp_path / "objs") in out


def test_dry_run_shows_chunked_tasks(capsys):
    rc = main(["--dry-run", "--chunk", "2", "--compdb", str(FIXTURE)])
    out = capsys.readouterr().out
    assert rc == 0
    assert "task cc_0000: src/hello.c, src/util.cc" in out


def test_usage_errors_return_2(tmp_path):
    assert main([]) == 2  # no input form at all
    assert main(["--compdb", str(FIXTURE), "a.c"]) == 2  # two input forms
    assert main(["--dry-run", "--"]) == 2  # nothing after --
    assert main(["--dry-run", "--chunk", "0", "--", "cc", "-c", "a.c"]) == 2
    assert main(["--dry-run", "--env", "mirror", "--", "cc", "-c", "a.c"]) == 2  # mirror needs --mirror
    assert main(["--dry-run", "--compdb", str(tmp_path / "missing.json")]) == 2


def test_empty_compdb_is_a_usage_error(tmp_path):
    empty = tmp_path / "cc.json"
    empty.write_text("[]", encoding="utf-8")
    assert main(["--dry-run", "--compdb", str(empty)]) == 2
