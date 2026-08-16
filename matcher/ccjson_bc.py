#!/usr/bin/env python3
"""ccjson_bc.py — turn a compile_commands.json into per-unit bitcode plans.

Argument policy lives here; logrange-scan.sh runs the plans and owns the
study's canonical flags (METHODOLOGY.md). Split that way because the policy
needs a JSON parser and shell-safe quoting, and the flags need to sit beside
the other scripts that hardcode them.

  ccjson_bc.py <compile_commands.json>

Emits TAB-separated records on stdout, one per line:

  #TU    <file>  <quoted dir>  <bitcode name>  <quoted argv>  <quoted fallback>
  #SKIP  <file>  <reason>
  #DUP   <file>

argv is the entry's own command with the compiler and everything that would
fight the harvest removed: the output, the optimization level, debug level,
dependency generation, -Werror, and gcc-only flags clang rejects. The
fallback is preprocessor, language and target flags only, for entries whose
first attempt dies on a flag this filter does not know about; a gcc-built
project reaches clang through one of the two.

Exit: 0 with at least one #TU, 2 on unreadable/unparseable/empty input.
"""

import json
import os
import shlex
import sys

# Launchers that precede the real compiler in a compile database.
LAUNCHERS = {"ccache", "distcc", "sccache", "icecc", "icerun", "gomacc"}

# Extensions the matcher study covers. Everything else is skipped by name
# rather than attempted, so the failure count stays meaningful.
C_LIKE = {".c", ".cc", ".cpp", ".cxx", ".c++", ".cp", ".C", ".m", ".mm"}

DROP_EXACT = {
    "-c", "-S", "-E", "-M", "-MM", "-MD", "-MMD", "-MP", "-MG",
    "-pipe", "-pg", "-flto", "-fwhole-program", "-fsyntax-only",
}
DROP_TAKES_ARG = {"-o", "-MF", "-MT", "-MQ", "--param", "-Xassembler", "-aux-info"}
DROP_PREFIX = (
    "-O", "-g", "-o", "-Werror", "-flto=", "-flto-", "--param=",
    "-fdump", "-fprofile", "-fvar-tracking", "-fno-var-tracking",
    "-frecord-gcc-switches", "-fcompare-debug", "-fconserve-stack",
    "-fira-", "-fsched-", "-fno-sched-", "-fplugin", "-fanalyzer",
    "-mpreferred-stack-boundary", "-Wa,",
)

# The fallback keeps only what changes the meaning of the source.
KEEP_EXACT = {"-nostdinc", "-nostdinc++", "-pthread", "-ansi", "-m32", "-m64", "-undef"}
KEEP_TAKES_ARG = {
    "-D", "-U", "-I", "-isystem", "-iquote", "-idirafter", "-include",
    "-imacros", "-isysroot", "-target", "-x", "-F", "-iframework",
}
KEEP_PREFIX = ("-D", "-U", "-I", "-std=", "--std=", "--sysroot=", "--target=", "-x")


def filtered(args):
    """The entry's own flags, minus what would fight the harvest."""
    out, skip = [], False
    for a in args:
        if skip:
            skip = False
            continue
        if a in DROP_TAKES_ARG:
            skip = True
            continue
        if a in DROP_EXACT or a.startswith(DROP_PREFIX):
            continue
        out.append(a)
    return out


def minimal(args, source):
    """Preprocessor, language and target flags only, plus the source."""
    out, pending = [], None
    for a in args:
        if pending is not None:
            out += [pending, a]
            pending = None
            continue
        if a in KEEP_TAKES_ARG:
            pending = a
        elif a in KEEP_EXACT or a.startswith(KEEP_PREFIX):
            out.append(a)
    return out + [source]


def main():
    if len(sys.argv) != 2:
        sys.exit("ccjson_bc.py: usage: ccjson_bc.py <compile_commands.json>")
    path = sys.argv[1]
    try:
        with open(path, "r", encoding="utf-8", errors="replace") as f:
            db = json.load(f)
    except OSError as e:
        sys.exit("ccjson_bc.py: cannot read %s: %s" % (path, e))
    except ValueError as e:
        sys.exit("ccjson_bc.py: %s is not valid JSON: %s" % (path, e))
    if not isinstance(db, list):
        sys.exit("ccjson_bc.py: %s is not a compile database (expected a list)" % path)

    seen, emitted = set(), 0
    for i, e in enumerate(db):
        if not isinstance(e, dict) or "file" not in e:
            print("#SKIP\tentry %d\tno file field" % i)
            continue
        src = e["file"]
        directory = e.get("directory") or os.getcwd()
        key = os.path.normpath(os.path.join(directory, src))

        if os.path.splitext(src)[1] not in C_LIKE:
            print("#SKIP\t%s\tnot a C/C++ source" % src)
            continue
        if "conftest" in os.path.basename(src):
            print("#SKIP\t%s\tconfigure probe" % src)
            continue
        # One source compiled by two targets is ordinary in cmake. Scanning
        # both would count every loop in it twice.
        if key in seen:
            print("#DUP\t%s" % src)
            continue

        if isinstance(e.get("arguments"), list):
            argv = [str(a) for a in e["arguments"]]
        elif isinstance(e.get("command"), str):
            argv = shlex.split(e["command"])
        else:
            print("#SKIP\t%s\tentry has neither command nor arguments" % src)
            continue
        if not argv:
            print("#SKIP\t%s\tempty command" % src)
            continue

        j = 0
        while j < len(argv) - 1 and os.path.basename(argv[j]) in LAUNCHERS:
            j += 1
        rest = argv[j + 1:]  # drop the compiler itself

        seen.add(key)
        emitted += 1
        # Index-prefixed so two same-named sources in different directories
        # cannot collide, which would silently drop one of them.
        stem = "".join(ch if ch.isalnum() else "_" for ch in os.path.basename(src))
        print("#TU\t%s\t%s\t%04d_%s.bc\t%s\t%s" % (
            src, shlex.quote(directory), emitted, stem,
            " ".join(shlex.quote(a) for a in filtered(rest)),
            " ".join(shlex.quote(a) for a in minimal(rest, src)),
        ))

    if emitted == 0:
        sys.exit("ccjson_bc.py: no translation units to scan in %s" % path)


if __name__ == "__main__":
    main()
