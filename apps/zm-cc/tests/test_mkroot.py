import os
from pathlib import Path

import pytest

from zm_cc.mkroot import Stager, parse_include_dirs, parse_ldd, parse_strace, probe_for


LDD_TEXT = """\tlinux-vdso.so.1 (0x00007ffd3f5f9000)
\tlibstdc++.so.6 => /usr/lib/libstdc++.so.6 (0x00007f0c1e000000)
\tlibc.so.6 => /usr/lib/libc.so.6 (0x00007f0c1dc00000)
\t/lib64/ld-linux-x86-64.so.2 (0x00007f0c1e5c0000)
"""

CC_V_STDERR = """Using built-in specs.
ignoring nonexistent directory "/usr/local/include"
#include "..." search starts here:
#include <...> search starts here:
 /usr/include/c++/16
 /usr/include/c++/16/x86_64-linux-gnu
 /usr/lib/gcc/x86_64-linux-gnu/16/include
 /usr/include (framework directory)
End of search list.
"""

STRACE_LOG = """1234 openat(AT_FDCWD, "/usr/lib/gcc/x86_64-linux-gnu/16/cc1plus", O_RDONLY|O_CLOEXEC) = 3
1234 openat(AT_FDCWD, "/etc/ld.so.cache", O_RDONLY|O_CLOEXEC) = 3
1234 openat(AT_FDCWD, "/usr/include/stdio.h", O_RDONLY) = -1 ENOENT (No such file or directory)
1234 execve("/usr/bin/g++", ["g++", "-c", "probe.cc"], 0x7ffd) = 0
1234 openat(AT_FDCWD, "/home/me/private.h", O_RDONLY) = 4
"""


def test_parse_ldd_finds_absolute_paths_only():
    assert parse_ldd(LDD_TEXT) == {
        "/usr/lib/libstdc++.so.6",
        "/usr/lib/libc.so.6",
        "/lib64/ld-linux-x86-64.so.2",
    }


def test_parse_include_dirs_stops_at_end_and_strips_framework_note():
    assert parse_include_dirs(CC_V_STDERR) == [
        "/usr/include/c++/16",
        "/usr/include/c++/16/x86_64-linux-gnu",
        "/usr/lib/gcc/x86_64-linux-gnu/16/include",
        "/usr/include",
    ]


def test_parse_strace_keeps_successful_system_paths_only():
    assert parse_strace(STRACE_LOG) == {
        "/usr/lib/gcc/x86_64-linux-gnu/16/cc1plus",
        "/etc/ld.so.cache",
        "/usr/bin/g++",
    }


def test_probe_language_follows_driver_name():
    assert probe_for("/usr/bin/g++")[0] == "probe.cc"
    assert probe_for("/usr/bin/clang++")[0] == "probe.cc"
    assert probe_for("/usr/bin/gcc")[0] == "probe.c"
    assert probe_for("/usr/bin/gfortran")[0] == "probe.f90"


@pytest.fixture
def fake_host(tmp_path):
    """A merged-usr host in miniature: /bin, /lib and /usr/lib64 are links; sh -> dash."""
    host = tmp_path / "host"
    (host / "usr" / "lib").mkdir(parents=True)
    (host / "usr" / "bin").mkdir(parents=True)
    (host / "usr" / "lib" / "libx.so.1").write_bytes(b"libx")
    (host / "usr" / "lib" / "libx.so").symlink_to("libx.so.1")
    (host / "usr" / "bin" / "dash").write_bytes(b"sh-bytes")
    (host / "usr" / "bin" / "sh").symlink_to("dash")
    (host / "bin").symlink_to("usr/bin")
    (host / "lib").symlink_to("usr/lib")
    (host / "usr" / "lib64").symlink_to("lib")
    return host


def test_stager_copies_real_content_once_and_recreates_links(tmp_path, fake_host):
    root = tmp_path / "root"
    root.mkdir()
    stager = Stager(root, host=fake_host)
    stager.add("/bin/sh")          # through the /bin link, then the sh -> dash link
    stager.add("/usr/lib/libx.so")  # a link to a versioned file
    stager.add_standard_links()
    copied, links = stager.flush()

    assert (root / "bin").is_symlink() and os.readlink(root / "bin") == "usr/bin"
    assert (root / "usr" / "bin" / "sh").is_symlink()
    assert (root / "usr" / "bin" / "dash").read_bytes() == b"sh-bytes"
    assert (root / "bin" / "sh").exists()  # resolves inside the root
    assert (root / "usr" / "lib" / "libx.so.1").read_bytes() == b"libx"
    assert (root / "usr" / "lib" / "libx.so").is_symlink()
    assert (root / "lib").is_symlink()
    assert (root / "usr" / "lib64").is_symlink() and os.readlink(root / "usr" / "lib64") == "lib"
    # Content is copied at its real path only; no duplicate under a link name.
    assert not (root / "bin" / "dash").exists() or (root / "bin" / "dash").resolve() == (
        root / "usr" / "bin" / "dash"
    ).resolve()
    assert sorted(p.name for p in (root / "usr" / "bin").iterdir()) == ["dash", "sh"]
    assert copied >= 2 and links >= 4


def test_stager_records_missing_paths(tmp_path, fake_host):
    stager = Stager(tmp_path / "root", host=fake_host)
    stager.add("/usr/bin/nope")
    assert stager.missing == ["/usr/bin/nope"]


def test_stager_skips_paths_that_escape_the_host(tmp_path, fake_host):
    outside = tmp_path / "outside.so"
    outside.write_bytes(b"x")
    (fake_host / "usr" / "lib" / "escape.so").symlink_to(outside)  # absolute link out of the host tree
    root = tmp_path / "root"
    root.mkdir()
    stager = Stager(root, host=fake_host)
    stager.add("/usr/lib/escape.so")
    copied, _links = stager.flush()
    assert copied == 0
    assert (root / "usr" / "lib" / "escape.so").is_symlink()  # the link is still recreated
