#!/usr/bin/env bash
# logrange-scan.sh — the diagnostic front door. One command, from a build
# directory to a report.
#
#   ./logrange-scan.sh [options] <build-dir | compile_commands.json>
#   ./logrange-scan.sh --check
#
# Replaces the manual two-step: rebuild the target under cc-bc.sh, run
# run_study.sh <name>, then diagnose.sh on the raw file. cc-bc.sh stays for
# builds that emit no compile database (SETUP.md in the repo root,
# "Producing a compile database").
#
# Pipeline, unchanged from METHODOLOGY.md: recompile every unit in the
# database at -O1 -g -fno-vectorize -fno-slp-vectorize -fno-unroll-loops
# -emit-llvm, run the matcher plugin over every module, render with
# diagnose.sh. Nothing in the target's own build output is used, so the
# target's compiler and flags do not have to be clang's.
#
# Options:
#   --all                      list LOW findings individually (diagnose.sh)
#   --raw FILE                 also write the raw matcher records here
#   --keep-bc DIR              keep harvested bitcode here instead of a
#                              temporary directory
#   --allow-compile-failures   accept a partial scan: units that fail to
#                              compile are counted and reported, exit code
#                              then reflects findings only
#   --rebuild                  rebuild the matcher plugin even if it looks
#                              current (it is rebuilt when older than its
#                              sources)
#   --check                    preflight the toolchain and exit
#
# Environment: CLANG, OPT (default clang, opt) and LOGRANGE_LLVM_MAJOR
# (default 21). Both tools must be that major version: the matcher is an opt
# plugin, and a plugin built against a different LLVM will not load.
#
# Exit codes:
#   0  scan completed, no HIGH findings
#   1  scan completed, at least one HIGH finding (so it can gate CI)
#   2  the scan did not complete, so its findings are not a clean bill of
#      health: bad usage, missing toolchain, unreadable compile database, no
#      bitcode produced, or a unit that failed to compile
#
# Serial by construction. Concurrent opt processes interleave records onto
# shared stderr lines and undercount (CONTRIBUTING.md, "Collect records
# serially").
set -u

HERE="$(cd "$(dirname "$0")" && pwd)"
PLUGIN="$HERE/build/SopMatcher.so"
CLANG="${CLANG:-clang}"
OPT="${OPT:-opt}"
REQ_MAJOR="${LOGRANGE_LLVM_MAJOR:-21}"

# The study's canonical flags (METHODOLOGY.md, "Pipeline"). -O1 buys the
# mem2reg/instcombine canonicalization the matcher relies on; the disables
# keep reductions in scalar recognizable form. Changing these makes the
# report incomparable with matcher/RESULTS.md.
CANON=(-O1 -g -fno-vectorize -fno-slp-vectorize -fno-unroll-loops -emit-llvm -c)

usage() {
  cat <<'EOF'
logrange-scan.sh — the diagnostic front door. One command, from a build
directory to a report of the reductions whose terms may leave range.

  ./logrange-scan.sh [options] <build-dir | compile_commands.json>
  ./logrange-scan.sh --check

  --all                      list LOW findings individually
  --raw FILE                 also write the raw matcher records here
  --keep-bc DIR              keep harvested bitcode instead of a temp dir
  --allow-compile-failures   accept a partial scan
  --rebuild                  rebuild the matcher plugin unconditionally
  --check                    preflight the toolchain and exit

Exit: 0 clean, 1 a HIGH finding, 2 the scan did not complete.
Needs Linux or WSL and LLVM 21. Setup: SETUP.md in the repo root.
EOF
}

die() { echo "logrange-scan: $*" >&2; exit 2; }

ALL=0
CHECK=0
ALLOW_FAIL=0
REBUILD=0
RAW_OUT=""
KEEP_BC=""
TARGET=""
while [ $# -gt 0 ]; do
  case "$1" in
    --all)                    ALL=1 ;;
    --check)                  CHECK=1 ;;
    --allow-compile-failures) ALLOW_FAIL=1 ;;
    --rebuild)                REBUILD=1 ;;
    --raw)     shift; [ $# -gt 0 ] || die "--raw needs a file"; RAW_OUT="$1" ;;
    --keep-bc) shift; [ $# -gt 0 ] || die "--keep-bc needs a directory"; KEEP_BC="$1" ;;
    -h|--help) usage; exit 0 ;;
    -*)        echo "logrange-scan: unknown option: $1" >&2; usage >&2; exit 2 ;;
    *)         [ -z "$TARGET" ] || die "one target at a time (got $TARGET and $1)"
               TARGET="$1" ;;
  esac
  shift
done

# ---------------------------------------------------------------- preflight

tool_major() { # tool_major <cmd> — LLVM major version, empty if unreadable
  "$1" --version 2>/dev/null | sed -n 's/.*version \([0-9][0-9]*\).*/\1/p' | head -1
}

preflight() { # prints a status line per requirement; returns 1 if unusable
  local bad=0 path major
  echo "logrange-scan preflight (wanted: LLVM $REQ_MAJOR)"

  case "$(uname -s 2>/dev/null)" in
    Linux) printf '  %-10s %s\n' platform "$(uname -sr)" ;;
    *)     printf '  %-10s %s: the LLVM tooling runs under WSL or Linux, not\n' \
             platform "$(uname -s 2>/dev/null || echo unknown)"
           printf '  %-10s %s\n' "" "Git Bash or MSYS. See SETUP.md in the repo root."
           bad=1 ;;
  esac

  for tool in "$CLANG" "$OPT"; do
    path="$(command -v "$tool" 2>/dev/null)"
    if [ -z "$path" ]; then
      printf '  %-10s MISSING: install clang-%s / llvm-%s, or set CLANG= and OPT=\n' \
        "$tool" "$REQ_MAJOR" "$REQ_MAJOR"
      bad=1
      continue
    fi
    major="$(tool_major "$tool")"
    if [ "$major" != "$REQ_MAJOR" ]; then
      printf '  %-10s %s is LLVM %s, wanted %s. The plugin will not load.\n' \
        "$tool" "$path" "${major:-?}" "$REQ_MAJOR"
      bad=1
    else
      printf '  %-10s LLVM %s at %s\n' "$tool" "$major" "$path"
    fi
  done

  for tool in python3 cmake; do
    path="$(command -v "$tool" 2>/dev/null)"
    if [ -z "$path" ]; then
      printf '  %-10s MISSING\n' "$tool"; bad=1
    else
      printf '  %-10s %s\n' "$tool" "$("$tool" --version 2>&1 | head -1)"
    fi
  done

  printf '  %-10s %s\n' plugin \
    "$([ -f "$PLUGIN" ] && echo "$PLUGIN" || echo "built on demand")"
  [ "$bad" = 0 ] || echo "Setup instructions: SETUP.md in the repo root."
  return "$bad"
}

if [ "$CHECK" = 1 ]; then
  preflight || exit 2
  exit 0
fi
[ -n "$TARGET" ] || { usage >&2; exit 2; }
preflight >/dev/null 2>&1 || { preflight >&2; exit 2; }

# ------------------------------------------------------------------- inputs

if [ -d "$TARGET" ]; then
  DB="$TARGET/compile_commands.json"
  [ -f "$DB" ] || die "no compile_commands.json in $TARGET
  Configure the build with -DCMAKE_EXPORT_COMPILE_COMMANDS=ON (cmake),
  or run the build under bear (make), or harvest bitcode with cc-bc.sh
  instead. SETUP.md in the repo root has all three."
elif [ -f "$TARGET" ]; then
  DB="$TARGET"
else
  die "no such build directory or compile database: $TARGET"
fi

TMP="$(mktemp -d)" || die "cannot create a temporary directory"
trap 'rm -rf "$TMP"' EXIT
if [ -n "$KEEP_BC" ]; then
  mkdir -p "$KEEP_BC" || die "cannot create $KEEP_BC"
  BCDIR="$KEEP_BC"
  # Bitcode left by an earlier scan would be scanned again and counted again.
  rm -f "$BCDIR"/*.bc
else
  BCDIR="$TMP/bc"
  mkdir -p "$BCDIR"
fi
RAW="$TMP/raw.txt"
PLAN="$TMP/plan.txt"
FAILERR="$TMP/failures.txt"
: > "$FAILERR"

# Build the plugin only when it is missing or older than its sources. A cold
# cmake configure plus build costs seconds per scan; over a test run that is
# most of the wall clock.
stale=0
[ -f "$PLUGIN" ] || stale=1
for f in "$HERE/SumOfProductsMatcher.cpp" "$HERE/CMakeLists.txt"; do
  [ "$f" -nt "$PLUGIN" ] && stale=1
done
if [ "$stale" = 1 ] || [ "$REBUILD" = 1 ]; then
  # A failed build must not leave the previous plugin on disk to be silently
  # scanned with (CONTRIBUTING.md, "A failed build leaves the old binary").
  rm -f "$PLUGIN"
  if ! { cmake -S "$HERE" -B "$HERE/build" -DCMAKE_BUILD_TYPE=Release \
           && cmake --build "$HERE/build"; } > "$TMP/plugin.log" 2>&1; then
    sed 's/^/  /' "$TMP/plugin.log" >&2
    die "could not build the matcher plugin (output above)"
  fi
  [ -f "$PLUGIN" ] || die "plugin build reported success but $PLUGIN is missing"
fi

python3 "$HERE/ccjson_bc.py" "$DB" > "$PLAN" || exit 2

# ------------------------------------------------------------------ harvest

compiled=0; failed=0; skipped=0; dups=0; recovered=0
while IFS=$'\t' read -r tag f1 f2 f3 f4 f5; do
  case "$tag" in
    '#SKIP') skipped=$((skipped + 1)) ;;
    '#DUP')  dups=$((dups + 1)) ;;
    '#TU')
      eval "dir=$f2"
      bc="$BCDIR/$f3"
      eval "set -- $f4"
      if ( cd "$dir" && exec "$CLANG" "$@" "${CANON[@]}" -o "$bc" ) \
           > "$TMP/cc-err" 2>&1; then
        compiled=$((compiled + 1))
      else
        # Second attempt with preprocessor and language flags only. Every
        # gcc-built project carries flags clang rejects outright, and a unit
        # dropped for a flag it did not need is a hole in the scan.
        eval "set -- $f5"
        if ( cd "$dir" && exec "$CLANG" "$@" "${CANON[@]}" -o "$bc" ) \
             > "$TMP/cc-err" 2>&1; then
          compiled=$((compiled + 1)); recovered=$((recovered + 1))
        else
          rm -f "$bc"
          failed=$((failed + 1))
          { echo "  $f1"; sed -n '1,4p' "$TMP/cc-err" | sed 's/^/      /'; } >> "$FAILERR"
        fi
      fi ;;
  esac
done < "$PLAN"

[ "$compiled" -gt 0 ] || die "no bitcode produced from $DB ($failed unit(s) failed to compile)
$(cat "$FAILERR")"

# --------------------------------------------------------------------- scan

: > "$RAW"
modules=0
for bc in "$BCDIR"/*.bc; do
  [ -e "$bc" ] || continue
  # loop-simplify and lcssa are unstated preconditions of the matcher's
  # RecurrenceDescriptor recognizer, and canonicalization a real mid-pipeline
  # pass would already have. See CANON_PASSES in run_study.sh; the two scripts
  # must apply the same pipeline or a diagnostic run and a study run disagree.
  "$OPT" -load-pass-plugin="$PLUGIN" -passes=loop-simplify,lcssa,sop-matcher \
    -disable-output "$bc" 2>> "$RAW" || true
  modules=$((modules + 1))
done
[ -z "$RAW_OUT" ] || cp "$RAW" "$RAW_OUT"

# ------------------------------------------------------------------- report

tally="translation units: $compiled compiled, $failed failed"
[ "$skipped" = 0 ]   || tally="$tally, $skipped skipped (not C/C++)"
[ "$dups" = 0 ]      || tally="$tally, $dups duplicate entr$([ "$dups" = 1 ] && echo y || echo ies)"
[ "$recovered" = 0 ] || tally="$tally, $recovered recovered on retry"
echo "logrange scan of $DB"
echo "$tally"
echo "modules scanned: $modules at ${CANON[*]}"
echo

if [ "$ALL" = 1 ]; then bash "$HERE/diagnose.sh" --all "$RAW"; else bash "$HERE/diagnose.sh" "$RAW"; fi
rc=$?

if [ "$failed" -gt 0 ]; then
  echo
  echo "FAILED TO COMPILE ($failed):"
  cat "$FAILERR"
  if [ "$ALLOW_FAIL" = 0 ]; then
    echo "logrange-scan: the scan is incomplete, so the report above is not a" >&2
    echo "clean bill of health. Re-run with --allow-compile-failures to accept" >&2
    echo "a partial scan." >&2
    exit 2
  fi
  echo "(accepted under --allow-compile-failures: these units were not scanned)"
fi
exit "$rc"
