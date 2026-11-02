import shutil
from pathlib import Path

import pytest

from zm_cc import toolchain
from zm_cc.plan import build_plan, entries_from_files

pytestmark = pytest.mark.skipif(not shutil.which("cc"), reason="needs cc")


def units(tmp_path: Path):
    (tmp_path / "a.c").write_text("int a;\n")
    (tmp_path / "b.c").write_text("int b;\n")
    return build_plan(entries_from_files("cc", "-O2", ["a.c", "b.c"], tmp_path), tmp_path / "o").remote_units


def test_host_env_names_each_compiler_once_as_an_exact_build(tmp_path):
    env = toolchain.host_env(units(tmp_path))
    assert [(p.name, p.version) for p in env.programs] == [(shutil.which("cc"), "")]


def test_capture_probes_one_compile_per_compiler_and_language_and_keys_stably(tmp_path):
    first = toolchain.capture_env(units(tmp_path), tmp_path)
    assert len(first.probes) == 1  # a.c and b.c share the compiler and the language
    probe = first.probes[0]
    assert probe[:2] == ("/bin/sh", "-c") and f"cd {tmp_path}" in probe[2] and " -c a.c -o " in probe[2]
    assert any(d.startswith("/usr/") for d in first.paths)  # the compiler's own header search list
    # The probe's command keys the capture: the same project must give the same spec every time.
    assert toolchain.capture_env(units(tmp_path), tmp_path) == first
