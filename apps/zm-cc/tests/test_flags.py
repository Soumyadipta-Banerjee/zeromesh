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

    pre = cmd.preprocess_argv(src, "g++", "/stage/a.ii")
    assert pre == [
        "g++", "-O2", "-std=c++20", "-Iinclude", "-I", "sys", "-DFOO=1", "-D", "BAR",
        "src/a.cpp", "-E", "-o", "/stage/a.ii",
    ]

    remote = cmd.remote_argv(src, "/usr/bin/g++", "a.abc.ii", "a.abc.o")
    assert remote == [
        "/usr/bin/g++", "-O2", "-std=c++20", "-x", "c++-cpp-output",
        "-c", "a.abc.ii", "-o", "out/a.abc.o",
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
    "argv, family, ext, remote_x",
    [
        (["cc", "-c", "a.c"], "c", ".i", "cpp-output"),
        (["c++", "-c", "a.cc"], "c++", ".ii", "c++-cpp-output"),
        (["c++", "-c", "a.cpp"], "c++", ".ii", "c++-cpp-output"),
        (["c++", "-c", "a.cxx"], "c++", ".ii", "c++-cpp-output"),
        (["c++", "-c", "a.C"], "c++", ".ii", "c++-cpp-output"),
        (["gfortran", "-c", "k.f90"], "fortran", ".f90", None),
        (["gfortran", "-c", "k.F90"], "fortran", ".f90", None),
        (["gfortran", "-c", "k.f"], "fortran", ".f", None),
        (["gfortran", "-c", "k.FOR"], "fortran", ".f", None),
        (["cc", "-c", "a.i"], "c", ".i", "cpp-output"),
        (["c++", "-c", "a.ii"], "c++", ".ii", "c++-cpp-output"),
        (["cc", "-x", "c++", "-c", "a.c"], "c++", ".ii", "c++-cpp-output"),
        (["cc", "-x", "none", "-c", "a.cc"], "c++", ".ii", "c++-cpp-output"),
        (["cc", "-xc++", "-c", "a.c"], "c++", ".ii", "c++-cpp-output"),
    ],
)
def test_language_from_extension_or_x(argv, family, ext, remote_x):
    cmd = parse(argv)
    src = cmd.sources[0]
    assert src.reason == "", src.reason
    assert src.lang.family == family
    assert src.lang.ext == ext
    assert src.lang.remote_x == remote_x


def test_already_preprocessed_input_is_flagged():
    assert parse(["cc", "-c", "a.i"]).sources[0].lang.preprocessed
    assert not parse(["cc", "-c", "a.c"]).sources[0].lang.preprocessed


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


def test_fortran_remote_compile_has_no_x_flag():
    cmd = parse(["gfortran", "-O2", "-c", "k.F90", "-o", "k.o"])
    src = cmd.sources[0]
    assert cmd.remote_argv(src, "/usr/bin/gfortran", "k.abc.f90", "k.abc.o") == [
        "/usr/bin/gfortran", "-O2", "-c", "k.abc.f90", "-o", "out/k.abc.o",
    ]
    assert "-cpp" in cmd.preprocess_argv(src, "gfortran", "/s/k.f90")


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
