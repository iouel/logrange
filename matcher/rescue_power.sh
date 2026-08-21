#!/usr/bin/env bash
# rescue_power.sh — Fisher exact test for the rescue study's tier comparison,
# and the minimum detectable effect at the pre-registered sample size.
#
#   ./rescue_power.sh p <a> <n1> <c> <n2>   one-sided p for a/n1 vs c/n2
#   ./rescue_power.sh mde <n1> <n2>         smallest detectable c for each a
#
# Why this exists. matcher/RESCUE.md's stopping question asks whether HIGH
# carries a "materially higher" rate of rescue-worthy failure than MED and LOW.
# At n=20 per tier that phrase is not self-evident: 3/20 against 6/20 looks
# like a ranking and is not distinguishable from noise. The test and the
# detectability limit are therefore fixed here, before R3 runs, and the same
# script produces the p-values R3 reports.
#
# One-sided Fisher exact, right tail, margins fixed:
#
#   p(a) = C(n1,a) C(n2,K-a) / C(n1+n2,K),  K = a + c
#   p    = sum over a' >= a
#
# Log factorials, summed directly: n <= 40 here, so nothing needs lgamma.
set -u

usage() {
  echo "usage: rescue_power.sh p <a> <n1> <c> <n2>"
  echo "       rescue_power.sh mde <n1> <n2>"
}

fisher() { # fisher <a> <n1> <c> <n2>  -> one-sided p on stdout
  awk -v a="$1" -v n1="$2" -v c="$3" -v n2="$4" '
  function lfact(n,   i, s) { s = 0; for (i = 2; i <= n; i++) s += log(i); return s }
  function lchoose(n, k) {
    if (k < 0 || k > n) return -1e308
    return lfact(n) - lfact(k) - lfact(n - k)
  }
  BEGIN {
    K = a + c; N = n1 + n2
    hi = (n1 < K) ? n1 : K
    p = 0
    for (x = a; x <= hi; x++) {
      lp = lchoose(n1, x) + lchoose(n2, K - x) - lchoose(N, K)
      if (lp > -700) p += exp(lp)
    }
    printf "%.6f\n", p
  }'
}

case "${1:-}" in
p)
  [ "$#" -eq 5 ] || { usage >&2; exit 2; }
  fisher "$2" "$3" "$4" "$5"
  ;;
mde)
  [ "$#" -eq 3 ] || { usage >&2; exit 2; }
  n1="$2"; n2="$3"
  echo "Minimum detectable effect, one-sided Fisher exact, alpha = 0.05"
  echo "n1 = $n1 (higher tier), n2 = $n2 (lower tier)"
  echo
  printf '%8s  %8s  %10s  %s\n' "lower" "higher" "difference" "p"
  # For each rate in the LOWER tier, the smallest count in the HIGHER tier
  # that separates them at alpha = 0.05.
  for c in $(seq 0 "$n2"); do
    found=""
    for a in $(seq "$c" "$n1"); do
      pv=$(fisher "$a" "$n1" "$c" "$n2")
      if awk -v p="$pv" 'BEGIN { exit !(p < 0.05) }'; then found="$a:$pv"; break; fi
    done
    if [ -n "$found" ]; then
      a="${found%%:*}"; pv="${found##*:}"
      d=$(awk -v a="$a" -v n1="$n1" -v c="$c" -v n2="$n2" \
            'BEGIN { printf "%.0f", 100*(a/n1 - c/n2) }')
      printf '%8s  %8s  %9s%%  %s\n' "$c/$n2" "$a/$n1" "$d" "$pv"
    else
      printf '%8s  %8s  %10s  %s\n' "$c/$n2" "none" "-" "not separable at any count"
    fi
  done
  ;;
*)
  usage >&2; exit 2
  ;;
esac
