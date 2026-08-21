#!/usr/bin/env bash
# test_diagnose.sh — gate for diagnose.sh, the report renderer.
#
#   bash matcher/test_diagnose.sh
#
# test_scan.sh drives diagnose.sh through logrange-scan.sh and gates the
# PIPELINE: compile databases, retries, dedup, provenance. The renderer's own
# branches are reached from a raw file and were untested, which mattered
# because the diagnostic is the shipped front door and was the artifact with
# the least automated coverage relative to the library behind it.
#
# What this covers, none of which test_scan.sh reaches:
#   - an unrecognized risk value: warned about, and listed under MED
#   - an old 8-column HIT line: skipped with a warning, never misread
#   - a clean input with no hits at all
#   - several raw files in one invocation
#   - an unknown reason token passing through as "flagged: <token>"
#   - long reason lists wrapping rather than running off
#   - the three exit codes, and warnings going to stderr not into the report
#
# The passthrough case is the subtle one. test_scan.sh case 11 asserts that
# every token the matcher emits renders as a sentence, i.e. that "flagged:"
# does NOT appear. Deleting the passthrough entirely would keep that green.
# This file asserts the other direction: an unknown token must still reach the
# reader, because the report must not hide a signal the matcher recorded.
#
# INDEPENDENT BY DESIGN. This gate shares no fixtures or helpers with the
# rescue instrument, needs no LLVM, no corpus and no plugin build, and must be
# green on its own.
set -u

HERE="$(cd "$(dirname "$0")" && pwd)"
DIAG="$HERE/diagnose.sh"
WORK="${WORK:-$HOME/logrange-study}/diagtest"

rm -rf "$WORK"
mkdir -p "$WORK"

fails=0
out=""
err=""
code=0

run() { # run <diagnose args...>
  out="$(bash "$DIAG" "$@" 2>"$WORK/stderr")"
  code=$?
  err="$(cat "$WORK/stderr")"
}

show() {
  printf '%s\n' "$out" | sed 's/^/    | /'
  [ -n "$err" ] && printf '%s\n' "$err" | sed 's/^/    E /'
  return 0
}

expect_code() { # expect_code <want> <label>
  if [ "$code" != "$1" ]; then
    echo "FAIL [$2]: exit $code, want $1"; show; fails=$((fails + 1))
  fi
}

expect_re() { # expect_re <extended-regex> <label>   (stdout)
  if ! printf '%s\n' "$out" | grep -qE -- "$1"; then
    echo "FAIL [$2]: stdout does not match /$1/"; show; fails=$((fails + 1))
  fi
}

reject_re() { # reject_re <extended-regex> <label>   (stdout)
  if printf '%s\n' "$out" | grep -qE -- "$1"; then
    echo "FAIL [$2]: stdout should not match /$1/"; show; fails=$((fails + 1))
  fi
}

expect_err() { # expect_err <extended-regex> <label> (stderr)
  if ! printf '%s\n' "$err" | grep -qE -- "$1"; then
    echo "FAIL [$2]: stderr does not match /$1/"; show; fails=$((fails + 1))
  fi
}

# ---------------------------------------------------------------------------
echo "== case 1: the committed fixture renders, exit 1 on its HIGH sites =="
run "$HERE/testdata/fixture-raw.txt"
expect_code 1 "case 1 exits 1 on HIGH"
expect_re "^loops examined: 12 +hits: 9 +HIGH: 3 +MED: 3 +LOW: 3$" "case 1 counts"
expect_re 'HIGH — likely to leave representable range \(3 sites\)' "case 1 HIGH header"
expect_re 'LOW: 3 shape-only sites' "case 1 summarizes LOW"

echo "== case 2: --all lists LOW individually instead of counting =="
run --all "$HERE/testdata/fixture-raw.txt"
expect_re 'LOW — shape match only, no range signal \(3 sites\)' "case 2 lists LOW"
reject_re 'pass --all to list them' "case 2 drops the count line"

echo "== case 3: no hits at all is a clean report, not an empty one =="
cat > "$WORK/nohits.txt" <<'EOF'
LOOP,src/a.c,1,f
LOOP,src/a.c,2,g
EOF
run "$WORK/nohits.txt"
expect_code 0 "case 3 exits 0"
expect_re 'No sum-of-products hits in the scanned input' "case 3 says so explicitly"
expect_re '^loops examined: 2 +hits: 0 +HIGH: 0 +MED: 0 +LOW: 0$' "case 3 counts"

echo "== case 4: an unrecognized risk value warns and lands under MED =="
cat > "$WORK/badrisk.txt" <<'EOF'
LOOP,src/a.c,10,f
HIT,src/a.c,10,f,runtime,2,1,plain,SEVERE,none
EOF
run "$WORK/badrisk.txt"
expect_code 0 "case 4 exits 0 (not HIGH)"
expect_err 'unrecognized risk "SEVERE" at src/a.c:10' "case 4 names the value and the site"
expect_re 'MED — range risk plausible \(1 site\)' "case 4 lists it under MED"
expect_re '^loops examined: 1 +hits: 1 +HIGH: 0 +MED: 1 +LOW: 0$' "case 4 counts it as MED"
reject_re 'unrecognized risk' "case 4 keeps the warning out of the report body"

echo "== case 5: an old 8-column HIT is skipped, not misread =="
cat > "$WORK/oldfmt.txt" <<'EOF'
LOOP,src/a.c,10,f
HIT,src/a.c,10,f,runtime,2,1,plain
LOOP,src/a.c,20,g
HIT,src/a.c,20,g,runtime,2,1,transcendental,HIGH,exp-chain
EOF
run "$WORK/oldfmt.txt"
expect_code 1 "case 5 still exits 1 for the well-formed HIGH"
expect_err 'skipped 1 HIT line\(s\) with fewer than 10 fields' "case 5 warns about the skip"
expect_re '^loops examined: 2 +hits: 1 +HIGH: 1 +MED: 0 +LOW: 0$' "case 5 does not count the skipped line"

echo "== case 6: several raw files in one invocation aggregate =="
run "$WORK/badrisk.txt" "$WORK/oldfmt.txt"
expect_re '^loops examined: 3 +hits: 2 +HIGH: 1 +MED: 1 +LOW: 0$' "case 6 aggregates across files"
expect_code 1 "case 6 exit reflects the union"

echo "== case 7: an unknown reason token still reaches the reader =="
cat > "$WORK/newtoken.txt" <<'EOF'
LOOP,src/a.c,10,f
HIT,src/a.c,10,f,runtime,2,1,plain,MED,brand-new-signal
EOF
run "$WORK/newtoken.txt"
expect_re 'flagged: brand-new-signal' "case 7 passes an unrendered token through"
expect_code 0 "case 7 exits 0"

echo "== case 8: a long reason list wraps instead of running off =="
cat > "$WORK/wrap.txt" <<'EOF'
LOOP,src/a.c,10,f
HIT,src/a.c,10,f,unknown,9,4,transcendental,HIGH,exp-chain;exp-sum;log-chain;deep-chain;unknown-trip
EOF
run "$WORK/wrap.txt"
expect_code 1 "case 8 exits 1"
longest=$(printf '%s\n' "$out" | awk '{ print length }' | sort -n | tail -1)
if [ "$longest" -gt 100 ]; then
  echo "FAIL [case 8 wraps]: longest line $longest chars, want <= 100"; show; fails=$((fails + 1))
fi
expect_re '^      ' "case 8 indents the wrapped body"

echo "== case 9: exit 2 is reserved for 'this is not a verdict' =="
run "$WORK/does-not-exist.txt"
expect_code 2 "case 9 unreadable input exits 2"
expect_err 'cannot read' "case 9 says what is wrong"

run
expect_code 2 "case 9 no arguments exits 2"
expect_err 'usage: diagnose.sh' "case 9 prints usage to stderr"

run --nonsense "$HERE/testdata/fixture-raw.txt"
expect_code 2 "case 9 unknown option exits 2"
expect_err 'unknown option: --nonsense' "case 9 names the bad option"

run --help
expect_code 0 "case 9 --help exits 0"

echo "== case 10: CRLF input is tolerated =="
printf 'LOOP,src/a.c,10,f\r\nHIT,src/a.c,10,f,runtime,2,1,transcendental,HIGH,exp-chain\r\n' \
  > "$WORK/crlf.txt"
run "$WORK/crlf.txt"
expect_code 1 "case 10 exits 1"
expect_re '^loops examined: 1 +hits: 1 +HIGH: 1 ' "case 10 parses CRLF records"

# ---------------------------------------------------------------------------
echo
if [ "$fails" -eq 0 ]; then
  echo "test_diagnose: PASS"
else
  echo "test_diagnose: FAIL ($fails)"
fi
exit $((fails > 0))
