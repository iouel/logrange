#!/usr/bin/env bash
# test_scan.sh — gate for logrange-scan.sh, the diagnostic front door.
#
#   bash matcher/test_scan.sh
#
# Ten cases. The front door must consume a real cmake-generated
# compile_commands.json and a hand-written one, reproduce the matcher's
# labeled ground truth through the whole pipeline (selftest.c: 7 loops,
# 5 hits, 2 HIGH), keep diagnose.sh's exit-code contract, and refuse rather
# than under-report when a translation unit does not compile.
#
# The last point is the one worth a gate. A scan that silently drops
# translation units prints "no findings" and reads as a clean bill of health.
set -u

HERE="$(cd "$(dirname "$0")" && pwd)"
SCAN="$HERE/logrange-scan.sh"
WORK="${WORK:-$HOME/logrange-study}/scantest"

rm -rf "$WORK"
mkdir -p "$WORK"

fails=0
out=""
code=0

run() { # run <scan args...>
  out="$(bash "$SCAN" "$@" 2>&1)"
  code=$?
}

show() { printf '%s\n' "$out" | sed 's/^/    | /'; }

expect_code() { # expect_code <want> <label>
  if [ "$code" != "$1" ]; then
    echo "FAIL [$2]: exit $code, want $1"; show; fails=$((fails + 1))
  fi
}

expect_re() { # expect_re <extended-regex> <label>
  if ! printf '%s\n' "$out" | grep -qE -- "$1"; then
    echo "FAIL [$2]: output does not match /$1/"; show; fails=$((fails + 1))
  fi
}

reject_re() { # reject_re <extended-regex> <label>
  if printf '%s\n' "$out" | grep -qE -- "$1"; then
    echo "FAIL [$2]: output should not match /$1/"; show; fails=$((fails + 1))
  fi
}

# The ground truth, once. selftest.c is the matcher's labeled corpus
# (run_study.sh selftest asserts the same 5/7/2 through the raw records);
# this asserts the front door delivers it through compile, scan and render.
GROUND='^loops examined: 7 +hits: 5 +HIGH: 2 +MED: 0 +LOW: 3$'

# Writes a compile_commands.json in the "arguments" array form that bear and
# ninja emit, as opposed to the "command" string form cmake's Makefile
# generator emits. Both forms must work.
write_db() { # write_db <out.json> <directory> <source> [extra-arg...]
  local out="$1" dir="$2" src="$3"; shift 3
  rm -f "$out"
  DB_OUT="$out" DB_DIR="$dir" DB_SRC="$src" DB_EXTRA="$*" python3 - <<'PY'
import json, os
extra = os.environ["DB_EXTRA"].split()
src = os.environ["DB_SRC"]
entry = {
    "directory": os.environ["DB_DIR"],
    "file": src,
    "arguments": ["gcc", "-Wall", "-O2", "-DNDEBUG"] + extra + ["-c", src, "-o", "x.o"],
}
with open(os.environ["DB_OUT"], "w") as f:
    json.dump([entry], f, indent=1)
PY
  # A test whose fixture silently failed to appear reports a tool bug.
  [ -s "$out" ] || { echo "FAIL: write_db produced no $out"; exit 1; }
}

echo "== case 1: cmake build directory =="
# The path a stranger takes: configure with the documented flag, point the
# tool at the build directory, read the report.
cmake -S "$HERE/testdata/scanproj" -B "$WORK/build" \
      -DCMAKE_EXPORT_COMPILE_COMMANDS=ON >"$WORK/cmake.log" 2>&1 \
  || { echo "FAIL [case 1]: fixture cmake configure failed"; sed 's/^/    | /' "$WORK/cmake.log"; fails=$((fails + 1)); }
run "$WORK/build"
expect_code 1 "case 1"
expect_re "$GROUND" "case 1 ground truth"
expect_re 'mixture_likelihood' "case 1 names the HIGH site"
expect_re 'softmax_denom' "case 1 names the second HIGH site"
expect_re '^translation units: 1 compiled' "case 1 provenance"

echo "== case 2: compile_commands.json path directly =="
run "$WORK/build/compile_commands.json"
expect_code 1 "case 2"
expect_re "$GROUND" "case 2 ground truth"

echo "== case 3: hand-written database, arguments form, benign source =="
# benign.c has one shape hit with no range signal, so the exit code must be 0.
# This is the half of the contract a HIGH-only test cannot check.
write_db "$WORK/benign.json" "$WORK" "$HERE/testdata/benign.c"
run "$WORK/benign.json"
expect_code 0 "case 3"
expect_re '^loops examined: 2 +hits: 1 +HIGH: 0 +MED: 0 +LOW: 1$' "case 3 counts"
expect_re 'LOW: 1 shape-only site' "case 3 summarizes LOW"

echo "== case 4: --all lists the LOW site =="
run --all "$WORK/benign.json"
expect_code 0 "case 4"
expect_re 'LOW — shape match only' "case 4 lists LOW"

echo "== case 5: nonexistent target =="
run "$WORK/does-not-exist"
expect_code 2 "case 5"
expect_re 'no such' "case 5 says what is wrong"

echo "== case 6: directory with no compile database =="
mkdir -p "$WORK/empty-dir"
run "$WORK/empty-dir"
expect_code 2 "case 6"
expect_re 'CMAKE_EXPORT_COMPILE_COMMANDS' "case 6 says how to produce one"

echo "== case 7: empty database =="
echo '[]' > "$WORK/empty.json"
run "$WORK/empty.json"
expect_code 2 "case 7"
expect_re 'no translation units' "case 7 refuses to report on nothing"

echo "== case 8: a translation unit that does not compile =="
# The failure mode this gate exists for: 1 of 2 units fails, the surviving
# unit has no HIGH finding, so a silent tool would exit 0 and read as clean.
printf 'this is not C.\n' > "$WORK/broken.c"
BENIGN="$HERE/testdata/benign.c" BROKEN="$WORK/broken.c" DIR="$WORK" python3 - <<'PY' > "$WORK/mixed.json"
import json, os
d = os.environ["DIR"]
print(json.dumps([
    {"directory": d, "file": os.environ["BENIGN"],
     "arguments": ["cc", "-c", os.environ["BENIGN"], "-o", "b.o"]},
    {"directory": d, "file": os.environ["BROKEN"],
     "arguments": ["cc", "-c", os.environ["BROKEN"], "-o", "k.o"]},
], indent=1))
PY
run "$WORK/mixed.json"
expect_code 2 "case 8"
expect_re 'broken\.c' "case 8 names the failed unit"
expect_re '1 failed' "case 8 counts the failure"
expect_re '^loops examined: 2 ' "case 8 still reports what it did scan"

echo "== case 9: --allow-compile-failures downgrades it =="
run --allow-compile-failures "$WORK/mixed.json"
expect_code 0 "case 9"
expect_re '1 failed' "case 9 still counts the failure"

echo "== case 10: gcc-only flags, duplicate entries, --check =="
# -ftree-loop-distribution is a real gcc flag clang rejects outright. It is
# deliberately one the filter in ccjson_bc.py does NOT know: the filter can
# never know them all, which is why the second attempt exists. A unit dropped
# for a flag it did not need is a hole in the scan.
write_db "$WORK/gccflags.json" "$WORK" "$HERE/testdata/benign.c" -ftree-loop-distribution
run "$WORK/gccflags.json"
expect_code 0 "case 10 retry"
expect_re '1 recovered' "case 10 reports the retry"
expect_re '^loops examined: 2 ' "case 10 scanned it after the retry"

# Two targets compiling one source is ordinary in cmake. Counting it twice
# would inflate every number in the report.
BENIGN="$HERE/testdata/benign.c" DIR="$WORK" python3 - <<'PY' > "$WORK/dup.json"
import json, os
e = {"directory": os.environ["DIR"], "file": os.environ["BENIGN"],
     "arguments": ["cc", "-c", os.environ["BENIGN"], "-o", "b.o"]}
print(json.dumps([e, dict(e, arguments=e["arguments"][:-1] + ["b2.o"])], indent=1))
PY
run "$WORK/dup.json"
expect_code 0 "case 10 duplicates"
expect_re '^loops examined: 2 +hits: 1' "case 10 counts the source once"
expect_re '1 duplicate' "case 10 reports the duplicate"

run --check
expect_code 0 "case 10 --check"
expect_re 'clang' "case 10 --check names the compiler"
reject_re '^loops examined' "case 10 --check does not scan"

echo "== case 11: every matcher reason token renders as a sentence =="
# Found by pointing the front door at this repo's own build: the report said
# "flagged: exp-sum", diagnose.sh's passthrough for a token it has no
# sentence for. exp-sum was added to the matcher on 2026-08-15 and the
# renderer was never taught it. testdata/fixture-raw.txt claimed to exercise
# every report branch and had no runner, so nothing noticed.
out="$(bash "$HERE/diagnose.sh" --all "$HERE/testdata/fixture-raw.txt" 2>&1)"
code=$?
expect_code 1 "case 11 fixture has HIGH findings"
reject_re 'flagged:' "case 11 renders every token"
for tok in $(grep -oE 'Reasons\.push_back\("[a-z-]+"\)' "$HERE/SumOfProductsMatcher.cpp" \
             | sed 's/.*("//; s/")//'); do
  grep -q -- "$tok" "$HERE/testdata/fixture-raw.txt" \
    || { echo "FAIL [case 11]: fixture-raw.txt does not cover token '$tok'"
         fails=$((fails + 1)); }
done

echo "== case 12: the worked example in SETUP.md =="
# SETUP.md tells a stranger to configure this repository and scan it. Guard
# the two structural claims, not the counts: the counts track the repo's own
# sources and are expected to move.
if cmake -S "$HERE/.." -B "$WORK/self" -DCMAKE_EXPORT_COMPILE_COMMANDS=ON \
     > "$WORK/self-cmake.log" 2>&1; then
  run "$WORK/self"
  expect_code 1 "case 12"
  expect_re '^translation units: [0-9]+ compiled, 0 failed' "case 12 scans the repo clean"
  expect_re 'k_linear_sum' "case 12 finds the benchmark's underflowing kernel"
  expect_re 'bench_main\.cpp' "case 12 names the file SETUP.md names"
else
  echo "FAIL [case 12]: cmake could not configure the repository"
  sed 's/^/    | /' "$WORK/self-cmake.log" | tail -5
  fails=$((fails + 1))
fi

echo
if [ "$fails" = 0 ]; then
  echo "test_scan: PASS (12 cases)"
else
  echo "test_scan: FAIL ($fails assertion(s))"
  exit 1
fi
