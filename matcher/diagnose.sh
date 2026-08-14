#!/usr/bin/env bash
# diagnose.sh — the diagnostic lint (intent v0.3, "Fallback Product").
#
#   ./diagnose.sh [--all] <raw-file>...
#
# Turns raw matcher scan output into a human report: which reductions may
# leave representable range, why, and what to do about it. This is a
# source-shape lint, not a range proof — see DIAGNOSTIC.md for scope.
#
# Input: raw scan files whose relevant lines are
#   LOOP,<file>,<line>,<function>
#   HIT,<file>,<line>,<function>,<trip>,<depth>,<nmul>,<transcendental|plain>,<risk>,<reasons>
# where trip is constant|runtime|unknown, risk is HIGH|MED|LOW, and reasons
# is a semicolon-joined token list (exp-chain, log-chain, deep-chain,
# unknown-trip) or "none". Anything else on a line is ignored, so raw files
# can carry other output. Old 8-column HIT lines (no risk/reasons) are
# skipped with a warning rather than misread.
#
# Exit code: 1 if any HIGH finding, else 0 — so it can gate CI.
# LOW findings are counted but not listed unless --all is passed.
set -u

usage() {
  echo "usage: diagnose.sh [--all] <raw-file>..."
  echo "  --all   list LOW findings individually instead of a count"
}

ALL=0
files=()
for arg in "$@"; do
  case "$arg" in
    --all)      ALL=1 ;;
    -h|--help)  usage; exit 0 ;;
    -*)         echo "diagnose.sh: unknown option: $arg" >&2; usage >&2; exit 2 ;;
    *)          files+=("$arg") ;;
  esac
done
if [ "${#files[@]}" -eq 0 ]; then usage >&2; exit 2; fi
for f in "${files[@]}"; do
  if [ ! -r "$f" ]; then echo "diagnose.sh: cannot read: $f" >&2; exit 2; fi
done

awk -v ALL="$ALL" '
# One plain-English sentence per reason token. Unknown tokens pass through
# verbatim rather than being dropped — the report must not hide a signal
# the matcher thought worth recording.
function sentence(tok) {
  if (tok == "exp-chain")
    return "a term is computed through exp(), so its magnitude is unbounded and the sum can underflow/overflow silently"
  if (tok == "log-chain")
    return "a term is computed through log(), which is unbounded near zero and can inject extreme magnitudes into the sum"
  if (tok == "deep-chain")
    return "each term multiplies a long factor chain, so magnitudes compound and can leave range even from moderate inputs"
  if (tok == "unknown-trip")
    return "the trip count cannot be bounded from the source, so accumulated growth is unbounded too"
  if (tok == "none")
    return "matched the sum-of-products shape; no additional range signal"
  return "flagged: " tok
}

function trip_text(t) {
  if (t == "constant") return "constant trip count"
  if (t == "runtime")  return "trip count set at runtime"
  return "trip count unknown"
}

# Naive word wrap at 78 columns with a hanging indent. Enough for a
# terminal report; not typesetting.
function wrap(text, indent,   words, i, k, line, out) {
  k = split(text, words, /[ \t]+/)
  out = ""; line = ""
  for (i = 1; i <= k; i++) {
    if (line == "") line = indent words[i]
    else if (length(line) + 1 + length(words[i]) > 78) {
      out = out line "\n"; line = indent words[i]
    } else line = line " " words[i]
  }
  return out line
}

# Build the full report block for one site.
function block(file, line, fn, trip, depth, nmul, kind, reasons,
               parts, i, k, joined, ctx) {
  k = split(reasons, parts, ";")
  joined = ""
  for (i = 1; i <= k; i++) {
    if (parts[i] == "") continue
    joined = joined (joined == "" ? "" : "; ") sentence(parts[i])
  }
  if (joined == "") joined = sentence("none")
  ctx = trip_text(trip) "; chain depth " depth ", " nmul \
        (nmul == 1 ? " multiply" : " multiplies") \
        (kind == "transcendental" ? ", transcendental chain" : "")
  return "  " file ":" line " (" fn ")\n" \
         wrap(joined ".", "      ") "\n" \
         "      [" ctx "]"
}

BEGIN { FS = ","; nhigh = nmed = nlow = 0; loops = hits = skipped = 0 }

{ sub(/\r$/, "") }                       # tolerate CRLF input

$1 == "LOOP" { loops++; next }

$1 == "HIT" {
  if (NF < 10) { skipped++; next }       # old 8-column format — refuse to guess
  hits++
  b = block($2, $3, $4, $5, $6, $7, $8, $10)
  if      ($9 == "HIGH") high[++nhigh] = b
  else if ($9 == "LOW")  low[++nlow]   = b
  else {                                 # MED, or anything unrecognized
    if ($9 != "MED")
      printf "diagnose.sh: warning: unrecognized risk \"%s\" at %s:%s — listed under MED\n", \
             $9, $2, $3 > "/dev/stderr"
    med[++nmed] = b
  }
  next
}

END {
  print "logrange diagnose — range lint for floating-point sum-of-products reductions"
  print "Flags reductions whose intermediate terms may leave representable range."
  print ""

  if (hits == 0) {
    print "No sum-of-products hits in the scanned input."
  }

  if (nhigh > 0) {
    printf "HIGH — likely to leave representable range (%d site%s)\n", nhigh, nhigh == 1 ? "" : "s"
    for (i = 1; i <= nhigh; i++) { print high[i]; print "" }
  }
  if (nmed > 0) {
    printf "MED — range risk plausible (%d site%s)\n", nmed, nmed == 1 ? "" : "s"
    for (i = 1; i <= nmed; i++) { print med[i]; print "" }
  }
  if (nlow > 0) {
    if (ALL) {
      printf "LOW — shape match only, no range signal (%d site%s)\n", nlow, nlow == 1 ? "" : "s"
      for (i = 1; i <= nlow; i++) { print low[i]; print "" }
    } else {
      printf "LOW: %d shape-only site%s with no range signal — pass --all to list them\n\n", \
             nlow, nlow == 1 ? "" : "s"
    }
  }

  if (skipped > 0)
    printf "diagnose.sh: warning: skipped %d HIT line(s) with fewer than 10 fields (old-format raw file?)\n", \
           skipped > "/dev/stderr"

  print "----------------------------------------------------------------------------"
  printf "loops examined: %d    hits: %d    HIGH: %d  MED: %d  LOW: %d\n", \
         loops, hits, nhigh, nmed, nlow
  print ""
  print wrap("Suggested fix: accumulate in the log domain — include/logrange/log_math.h provides pos_accum (positive terms) and rp_accum (signed) with stated error bounds. See BENCHMARKS.md for cost.", "")

  exit (nhigh > 0 ? 1 : 0)
}
' "${files[@]}"
