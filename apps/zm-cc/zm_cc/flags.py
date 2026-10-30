"""Parse a compiler command line and decide what travels where in a split build.

A split build runs the preprocessor locally (``cc -E``), ships the self-contained
output to a donor, and compiles it there with the codegen flags only. Every
option on the command line therefore has one of these kinds:

  KEEP         codegen (-O2, -g, -std=..., -W..., -m..., -f...): kept on both sides
  PREPROC      preprocessor-only (-I, -D, -include, -isystem, ...): local -E only
  DRIVER       dependency files and listings (-MD, -MF, -P, ...): dropped from both
  OUTPUT       -o VALUE: replaced by zm-cc's own object name
  LANG         -x LANG: local -E only; the remote compile sets its own
  PASSTHROUGH  cannot be split (-S, -E, -M, PCH, modules, profiles...): the whole
               command runs locally, unchanged

``_RULES`` is the whole policy. To teach the classifier a new flag, add a row.
Nothing here knows about CMake, Meson or any one project.
"""

from __future__ import annotations

import os
import shlex
from dataclasses import dataclass
from enum import Enum
from pathlib import PurePath
from typing import Optional, Sequence


class Kind(Enum):
    KEEP = "keep"
    PREPROC = "preproc"
    DRIVER = "driver"
    OUTPUT = "output"
    LANG = "lang"
    PASSTHROUGH = "passthrough"
    SOURCE = "source"


@dataclass(frozen=True)
class Rule:
    name: str
    kind: Kind
    takes_value: bool = False  # "-I dir" or "-Idir"; "--sysroot=dir" or "--sysroot dir"
    prefix: bool = False  # matches any token that starts with name; takes no value


_RULES: tuple[Rule, ...] = (
    # Output, language and mode.
    Rule("-o", Kind.OUTPUT, takes_value=True),
    Rule("-x", Kind.LANG, takes_value=True),
    Rule("-c", Kind.DRIVER),
    Rule("-E", Kind.PASSTHROUGH),
    Rule("-S", Kind.PASSTHROUGH),
    Rule("-M", Kind.PASSTHROUGH),
    Rule("-MM", Kind.PASSTHROUGH),
    Rule("-dM", Kind.PASSTHROUGH),
    Rule("-fsyntax-only", Kind.PASSTHROUGH),
    Rule("-###", Kind.PASSTHROUGH),
    Rule("-emit-pch", Kind.PASSTHROUGH),
    Rule("-emit-ast", Kind.PASSTHROUGH),
    Rule("-include-pch", Kind.PASSTHROUGH, takes_value=True),
    Rule("-fmodule", Kind.PASSTHROUGH, prefix=True),  # -fmodules, -fmodule-file=...
    Rule("-fprofile-", Kind.PASSTHROUGH, prefix=True),  # profile data is path-dependent
    Rule("-ftest-coverage", Kind.PASSTHROUGH),
    Rule("-coverage", Kind.PASSTHROUGH),
    Rule("--coverage", Kind.PASSTHROUGH),
    Rule("-save-temps", Kind.PASSTHROUGH, prefix=True),
    # Dependency files and driver listings: no effect on the object file.
    Rule("-MD", Kind.DRIVER),
    Rule("-MMD", Kind.DRIVER),
    Rule("-MP", Kind.DRIVER),
    Rule("-MG", Kind.DRIVER),
    Rule("-MF", Kind.DRIVER, takes_value=True),
    Rule("-MT", Kind.DRIVER, takes_value=True),
    Rule("-MQ", Kind.DRIVER, takes_value=True),
    Rule("-Wp,", Kind.DRIVER, prefix=True),  # -Wp,-MD,deps: preprocessor pass-through
    Rule("-C", Kind.DRIVER),
    Rule("-CC", Kind.DRIVER),
    Rule("-P", Kind.DRIVER),
    Rule("-dD", Kind.DRIVER),
    Rule("-H", Kind.DRIVER),
    Rule("-fpreprocessed", Kind.DRIVER),
    # Preprocessor-only: the -E output already has these applied.
    Rule("-I", Kind.PREPROC, takes_value=True),
    Rule("-D", Kind.PREPROC, takes_value=True),
    Rule("-U", Kind.PREPROC, takes_value=True),
    Rule("-F", Kind.PREPROC, takes_value=True),
    Rule("-include", Kind.PREPROC, takes_value=True),
    Rule("-imacros", Kind.PREPROC, takes_value=True),
    Rule("-isystem", Kind.PREPROC, takes_value=True),
    Rule("-iquote", Kind.PREPROC, takes_value=True),
    Rule("-idirafter", Kind.PREPROC, takes_value=True),
    Rule("-iprefix", Kind.PREPROC, takes_value=True),
    Rule("-iwithprefix", Kind.PREPROC, takes_value=True),
    Rule("-iwithprefixbefore", Kind.PREPROC, takes_value=True),
    Rule("-iframework", Kind.PREPROC, takes_value=True),
    Rule("-isysroot", Kind.PREPROC, takes_value=True),
    Rule("--sysroot", Kind.PREPROC, takes_value=True),
    Rule("-Xpreprocessor", Kind.PREPROC, takes_value=True),
    Rule("-nostdinc", Kind.PREPROC),
    Rule("-nostdinc++", Kind.PREPROC),
    Rule("-undef", Kind.PREPROC),
    Rule("-traditional-cpp", Kind.PREPROC),
    Rule("-fdirectives-only", Kind.PREPROC),
)

# Passthrough options after which the command does not write an object file.
_NO_OBJECT = frozenset({"-E", "-S", "-M", "-MM", "-dM", "-fsyntax-only", "-###", "-emit-pch", "-emit-ast"})


@dataclass(frozen=True)
class Lang:
    """What the splitter needs to know about one source language."""

    family: str  # "c", "c++" or "fortran"
    preprocessed: bool  # input is already preprocessor output: no local -E
    ext: str  # extension of the staged, preprocessed file
    remote_x: Optional[str]  # -x for the remote compile; None lets the extension decide


_LANGS: dict[str, Lang] = {
    # gcc and clang spell preprocessed C "cpp-output" (not "c-cpp-output"); C++ is "c++-cpp-output".
    "c": Lang("c", False, ".i", "cpp-output"),
    "c++": Lang("c++", False, ".ii", "c++-cpp-output"),
    "cpp-output": Lang("c", True, ".i", "cpp-output"),
    "c++-cpp-output": Lang("c++", True, ".ii", "c++-cpp-output"),
    "f95": Lang("fortran", False, ".f90", None),
    "f95-cpp-input": Lang("fortran", False, ".f90", None),
    "f77": Lang("fortran", False, ".f", None),
    "f77-cpp-input": Lang("fortran", False, ".f", None),
}

# Extension -> key in _LANGS. Upper-case Fortran extensions mean "run cpp first".
_EXT_LANGS: dict[str, str] = {
    ".c": "c",
    ".i": "cpp-output",
    ".cc": "c++",
    ".cpp": "c++",
    ".cxx": "c++",
    ".c++": "c++",
    ".C": "c++",
    ".ii": "c++-cpp-output",
    ".f90": "f95",
    ".f95": "f95",
    ".f03": "f95",
    ".f08": "f95",
    ".F90": "f95",
    ".F95": "f95",
    ".F03": "f95",
    ".F08": "f95",
    ".f": "f77",
    ".for": "f77",
    ".ftn": "f77",
    ".F": "f77",
    ".FOR": "f77",
    ".FTN": "f77",
}


@dataclass(frozen=True)
class Token:
    text: str
    kind: Kind


@dataclass(frozen=True)
class Source:
    """One input file. ``reason`` is non-empty when the file cannot be split."""

    text: str
    lang: Optional[Lang]
    reason: str = ""


@dataclass(frozen=True)
class Command:
    driver: str
    tokens: tuple[Token, ...]
    sources: tuple[Source, ...]
    output: Optional[str]
    compile_only: bool
    blockers: tuple[str, ...]  # command-wide reasons the command runs locally
    produces_object: bool

    def reasons_for(self, src: Optional[Source]) -> list[str]:
        out = list(self.blockers)
        if src is not None and src.reason:
            out.append(src.reason)
        return out

    def argv_for(self, src: Optional[Source]) -> list[str]:
        """The original command with only ``src`` as input (all tokens when src is None)."""
        return [self.driver] + [t.text for t in self.tokens if not _is_other_source(t, src)]

    def preprocess_argv(self, src: Source, driver: str, out: str) -> list[str]:
        """Local ``-E`` for one source: preprocessor and codegen options, no -c or -o."""
        args = [driver]
        for t in self.tokens:
            if t.kind in (Kind.KEEP, Kind.PREPROC, Kind.LANG) or (t.kind is Kind.SOURCE and t.text == src.text):
                args.append(t.text)
        args.append("-E")
        if src.lang is not None and src.lang.family == "fortran":
            args += ["-cpp", "-P"]  # gfortran: force cpp; no line markers in free/fixed form
        return args + ["-o", out]

    def remote_argv(self, src: Source, driver: str, stage_name: str, object_name: str) -> list[str]:
        """Remote compile of the preprocessed file: codegen options only."""
        if src.lang is None:
            raise ValueError(f"{src.text}: unsplittable input has no remote compile")
        args = [driver] + [t.text for t in self.tokens if t.kind is Kind.KEEP]
        if src.lang.remote_x:
            # *-cpp-output tells gcc and clang the input is already preprocessed
            # (this is what -fpreprocessed would otherwise say).
            args += ["-x", src.lang.remote_x]
        return args + ["-c", stage_name, "-o", f"out/{object_name}"]


def _is_other_source(t: Token, src: Optional[Source]) -> bool:
    return t.kind is Kind.SOURCE and src is not None and t.text != src.text


def _lookup(tok: str) -> Optional[tuple[Rule, Optional[str]]]:
    """Find the rule for ``tok``; for a glued value (-Idir, --sysroot=dir) also return the value.

    Exact spellings win over glued values, so ``-include-pch`` is not read as ``-include``.
    """
    for rule in _RULES:
        if tok == rule.name:
            return rule, None
    for rule in _RULES:
        if rule.takes_value and tok.startswith(rule.name) and len(tok) > len(rule.name):
            glued = tok[len(rule.name):]
            if rule.name.startswith("--"):
                if not glued.startswith("="):
                    continue
                glued = glued[1:]
            return rule, glued
    for rule in _RULES:
        if rule.prefix and tok.startswith(rule.name):
            return rule, None
    return None


def _language(text: str, x_lang: Optional[str]) -> tuple[Optional[Lang], str]:
    """Language of one input file: the -x in effect, else its extension."""
    if x_lang is not None and x_lang != "none":
        lang = _LANGS.get(x_lang)
        return lang, ("" if lang is not None else f"-x {x_lang} is not split")
    key = _EXT_LANGS.get(PurePath(text).suffix)
    if key is None:
        return None, f"unknown input type {text!r}"
    return _LANGS[key], ""


def parse(argv: Sequence[str]) -> Command:
    """Classify every token of a compiler command line (``argv[0]`` is the driver)."""
    if not argv:
        raise ValueError("empty compiler command")
    driver, args = argv[0], list(argv[1:])
    tokens: list[Token] = []
    sources: list[Source] = []
    blockers: list[str] = []
    output: Optional[str] = None
    compile_only = False
    no_object = False
    x_lang: Optional[str] = None

    i = 0
    while i < len(args):
        tok = args[i]
        i += 1
        hit = _lookup(tok)
        if hit is None:
            if tok == "-":
                tokens.append(Token(tok, Kind.PASSTHROUGH))
                blockers.append("reads stdin")
            elif tok.startswith("-"):
                tokens.append(Token(tok, Kind.KEEP))
            else:
                lang, reason = _language(tok, x_lang)
                sources.append(Source(tok, lang, reason))
                tokens.append(Token(tok, Kind.SOURCE))
            continue

        rule, value = hit
        tokens.append(Token(tok, rule.kind))
        if rule.takes_value and value is None:
            if i >= len(args):
                blockers.append(f"{tok} is missing its value")
                break
            value = args[i]
            i += 1
            tokens.append(Token(value, rule.kind))

        if rule.kind is Kind.OUTPUT:
            output = value
        elif rule.kind is Kind.LANG:
            x_lang = value
        elif rule.name == "-c":
            compile_only = True
        elif rule.kind is Kind.PASSTHROUGH:
            blockers.append(f"{tok} needs the whole command (local)")
            if tok in _NO_OBJECT:
                no_object = True

    if not compile_only:
        blockers.append("not a compile-only command (no -c)")
    if not sources:
        blockers.append("no source file")
    if output == "-":
        blockers.append("-o - writes to stdout")

    return Command(
        driver=driver,
        tokens=tuple(tokens),
        sources=tuple(sources),
        output=output,
        compile_only=compile_only,
        blockers=tuple(blockers),
        produces_object=compile_only and not no_object,
    )


def expand_response_files(argv: Sequence[str], base: os.PathLike | str, _depth: int = 0) -> list[str]:
    """Replace ``@file`` tokens with the words in ``file`` (relative to ``base``), as gcc does."""
    out: list[str] = []
    for tok in argv:
        if tok.startswith("@") and len(tok) > 1 and _depth < 8:
            path = os.path.join(os.fspath(base), tok[1:])
            if os.path.isfile(path):
                with open(path, encoding="utf-8") as fh:
                    out.extend(expand_response_files(shlex.split(fh.read()), base, _depth + 1))
                continue
        out.append(tok)
    return out
