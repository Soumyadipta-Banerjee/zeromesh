import pytest

from zm_cc.flags import Kind, expand_response_files, parse


def first_kind(*args: str) -> Kind:
    """Kind of the first token of ``args``, in a compile of one file."""
    return parse(["cc", *args, "-c", "a.c"]).tokens[0].kind


@pytest.mark.parametrize(
    "args, kind",
    [
        (["-O2"], Kind.KEEP),
        (["-g"], Kind.KEEP),
        (["-fPIC"], Kind.KEEP),
        (["-march=native"], Kind.KEEP),
        (["-std=c++20"], Kind.KEEP),
        (["-Wl,--as-needed"], Kind.KEEP),
        (["-pthread"], Kind.KEEP),
        (["-fopenmp"], Kind.KEEP),
        (["-Iinc"], Kind.PREPROC),
        (["-I", "inc"], Kind.PREPROC),
        (["-DNDEBUG"], Kind.PREPROC),
        (["-D", "X=1"], Kind.PREPROC),
        (["-UFOO"], Kind.PREPROC),
        (["-include", "config.h"], Kind.PREPROC),
        (["-imacros", "m.h"], Kind.PREPROC),
        (["-isystem", "third_party"], Kind.PREPROC),
        (["-iquote", "q"], Kind.PREPROC),
        (["--sysroot=/opt/sysroot"], Kind.PREPROC),
        (["--sysroot", "/opt/sysroot"], Kind.PREPROC),
        (["-Xpreprocessor", "-DX"], Kind.PREPROC),
        (["-nostdinc"], Kind.PREPROC),
        (["-MD"], Kind.DRIVER),
        (["-MMD"], Kind.DRIVER),
        (["-MP"], Kind.DRIVER),
        (["-MF", "a.d"], Kind.DRIVER),
        (["-MFa.d"], Kind.DRIVER),
        (["-MT", "a.o"], Kind.DRIVER),
        (["-Wp,-MD,a.d"], Kind.DRIVER),
        (["-o", "a.o"], Kind.OUTPUT),
        (["-oa.o"], Kind.OUTPUT),
        (["-x", "c++"], Kind.LANG),
        (["-xc++"], Kind.LANG),
        (["-S"], Kind.PASSTHROUGH),
        (["-E"], Kind.PASSTHROUGH),
        (["-M"], Kind.PASSTHROUGH),
        (["-MM"], Kind.PASSTHROUGH),
        (["-fsyntax-only"], Kind.PASSTHROUGH),
        (["-emit-pch"], Kind.PASSTHROUGH),
        (["-include-pch", "x.gch"], Kind.PASSTHROUGH),
        (["-fmodules-ts"], Kind.PASSTHROUGH),
        (["-fmodule-file=x.pcm"], Kind.PASSTHROUGH),
        (["-fprofile-use=dir"], Kind.PASSTHROUGH),
        (["--coverage"], Kind.PASSTHROUGH),
        (["-save-temps"], Kind.PASSTHROUGH),
    ],
)
def test_flag_kinds(args, kind):
    assert first_kind(*args) is kind


def test_include_is_not_read_as_include_pch():
    # Exact spellings win over glued values: -include-pch is passthrough, not -include.
    assert first_kind("-include-pch", "x.gch") is Kind.PASSTHROUGH


def test_codegen_kept_preproc_split_and_shapes():
    cmd = parse(
        ["g++", "-O2", "-std=c++20", "-Iinclude", "-I", "sys", "-DFOO=1", "-D", "BAR",
         "-MD", "-MF", "a.d", "-c", "src/a.cpp", "-o", "a.o"]
    )
    src = cmd.sources[0]
    assert src.text == "src/a.cpp" and src.lang is not None and src.lang.family == "c++"
    assert cmd.blockers == () and cmd.produces_object and cmd.output == "a.o"

    assert cmd.compile_argv(src, "/usr/bin/g++", "a.abc.o", depfile="a.abc.d") == [
        "/usr/bin/g++", "-O2", "-std=c++20", "-Iinclude", "-I", "sys", "-DFOO=1", "-D", "BAR",
        "-MD", "-MF", "a.abc.d", "-c", "src/a.cpp", "-o", "a.abc.o",
    ]


def test_glued_output_and_no_object_flags():
    cmd = parse(["cc", "-c", "a.c", "-oa.o"])
    assert cmd.output == "a.o" and cmd.produces_object
    assert not parse(["cc", "-M", "-c", "a.c"]).produces_object
    assert not parse(["cc", "-fsyntax-only", "-c", "a.c"]).produces_object


@pytest.mark.parametrize(
    "argv, blocker",
    [
        (["cc", "a.c", "-o", "a"], "not a compile-only command"),
        (["cc", "-c"], "no source file"),
        (["cc", "-c", "-", "-o", "a.o"], "reads stdin"),
        (["cc", "-c", "a.c", "-o", "-"], "-o - writes to stdout"),
        (["cc", "-c", "a.c", "-fmodules-ts"], "needs the whole command"),
    ],
)
def test_command_blockers(argv, blocker):
    cmd = parse(argv)
    assert any(blocker in b for b in cmd.blockers), cmd.blockers


@pytest.mark.parametrize(
    "argv, family",
    [
        (["cc", "-c", "a.c"], "c"),
        (["c++", "-c", "a.cc"], "c++"),
        (["c++", "-c", "a.cpp"], "c++"),
        (["c++", "-c", "a.cxx"], "c++"),
        (["c++", "-c", "a.C"], "c++"),
        (["gfortran", "-c", "k.f90"], "fortran"),
        (["gfortran", "-c", "k.F90"], "fortran"),
        (["gfortran", "-c", "k.f"], "fortran"),
        (["gfortran", "-c", "k.FOR"], "fortran"),
        (["cc", "-c", "a.i"], "c"),
        (["c++", "-c", "a.ii"], "c++"),
        (["cc", "-x", "c++", "-c", "a.c"], "c++"),
        (["cc", "-x", "none", "-c", "a.cc"], "c++"),
        (["cc", "-xc++", "-c", "a.c"], "c++"),
    ],
)
def test_language_from_extension_or_x(argv, family):
    src = parse(argv).sources[0]
    assert src.reason == "", src.reason
    assert src.lang.family == family


@pytest.mark.parametrize(
    "argv, reason",
    [
        (["cc", "-x", "c-header", "-c", "a.h"], "-x c-header is not split"),
        (["cc", "-x", "assembler-with-cpp", "-c", "a.S"], "-x assembler-with-cpp is not split"),
        (["cc", "-c", "a.s"], "unknown input type"),
        (["cc", "-c", "foo.o"], "unknown input type"),
    ],
)
def test_unsplittable_sources_carry_a_reason(argv, reason):
    src = parse(argv).sources[0]
    assert reason in src.reason


def test_fortran_compile_has_no_x_flag():
    cmd = parse(["gfortran", "-O2", "-c", "k.F90", "-o", "k.o"])
    assert cmd.compile_argv(cmd.sources[0], "/usr/bin/gfortran", "k.abc.o") == [
        "/usr/bin/gfortran", "-O2", "-c", "k.F90", "-o", "k.abc.o",
    ]


def test_compile_argv_keeps_preproc_and_rewrites_project_includes(tmp_path):
    root = tmp_path / "proj"
    src_dir = root / "src"
    inc = root / "include"
    src_dir.mkdir(parents=True)
    inc.mkdir()
    (src_dir / "a.c").write_text("int x;\n", encoding="utf-8")
    cmd = parse(["cc", "-O2", "-I../include", "-I/usr/include", "-DFOO=1", "-c", "a.c", "-o", "a.o"])
    src = cmd.sources[0]
    argv = cmd.compile_argv(src, "/usr/bin/cc", "a.hash.o", root=str(root), cwd=str(src_dir))
    assert argv[0] == "/usr/bin/cc"
    assert "-O2" in argv and "-DFOO=1" in argv
    assert "-I../include" in argv  # still relative to cwd under root
    assert "-I/usr/include" in argv  # system path untouched
    assert argv[-4:] == ["-c", "a.c", "-o", "a.hash.o"]
    assert "-MD" not in argv and "a.o" not in argv  # driver / original -o dropped


def test_compile_argv_rewrites_absolute_paths_under_root(tmp_path):
    root = tmp_path / "proj"
    (root / "inc").mkdir(parents=True)
    (root / "a.c").write_text("int x;\n", encoding="utf-8")
    abs_inc = str((root / "inc").resolve())
    abs_src = str((root / "a.c").resolve())
    cmd = parse(["cc", f"-I{abs_inc}", "-c", abs_src, "-o", "a.o"])
    src = cmd.sources[0]
    argv = cmd.compile_argv(src, "/usr/bin/cc", "a.hash.o", root=str(root.resolve()), cwd=str(root.resolve()))
    assert f"-Iinc" in argv or "-I./inc" in argv
    assert argv[-3] == "a.c"


def test_multi_source_command_keeps_order_and_isolates_each_source():
    cmd = parse(["cc", "-O2", "-x", "c++", "a.c", "-c", "b.cc", "-Idir"])
    a, b = cmd.sources
    assert cmd.argv_for(a) == ["cc", "-O2", "-x", "c++", "a.c", "-c", "-Idir"]
    assert cmd.argv_for(b) == ["cc", "-O2", "-x", "c++", "-c", "b.cc", "-Idir"]
    assert cmd.argv_for(None) == ["cc", "-O2", "-x", "c++", "a.c", "-c", "b.cc", "-Idir"]


def test_response_files_expand_recursively(tmp_path):
    (tmp_path / "inner.rsp").write_text("-DDEEP=1 -Iinc\n", encoding="utf-8")
    (tmp_path / "args.rsp").write_text('-O2 "-DMSG=hello world" @inner.rsp\n', encoding="utf-8")
    out = expand_response_files(["cc", "@args.rsp", "@missing.rsp", "-c", "a.c"], tmp_path)
    assert out == ["cc", "-O2", "-DMSG=hello world", "-DDEEP=1", "-Iinc", "@missing.rsp", "-c", "a.c"]


def test_response_file_cycle_terminates(tmp_path):
    (tmp_path / "loop.rsp").write_text("@loop.rsp -O2\n", encoding="utf-8")
    out = expand_response_files(["cc", "@loop.rsp"], tmp_path)
    assert "-O2" in out
