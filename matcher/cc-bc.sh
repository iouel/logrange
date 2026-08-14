#!/usr/bin/env bash
# cc-bc.sh — clang wrapper for the matcher study. Performs the real
# compilation unchanged, and for every -c compile also drops a .bc (LLVM
# bitcode) sibling under $BC_DIR, mirroring the source path. This lets the
# target codebases' own build systems run untouched (CC=cc-bc.sh) while we
# harvest per-TU bitcode at the study's canonical flags (METHODOLOGY.md).
#
# The harvest recompile keeps every original argument (includes, defines,
# language options) and only overrides: output, optimization level, and the
# canonicalization flags.
set -u
: "${CLANG:=clang}"
: "${BC_DIR:?set BC_DIR to the bitcode output directory}"

"$CLANG" "$@"
status=$?
[ $status -ne 0 ] && exit $status

# Only harvest ordinary compiles.
is_compile=0
src=""
for a in "$@"; do
  case "$a" in
    -c) is_compile=1 ;;
    *.c|*.cc|*.cpp|*.cxx) src="$a" ;;
  esac
done
[ $is_compile -eq 1 ] && [ -n "$src" ] || exit 0

# Rebuild the argument list minus "-o <file>" and any -O<level>.
args=()
skip=0
for a in "$@"; do
  if [ $skip -eq 1 ]; then skip=0; continue; fi
  case "$a" in
    -o) skip=1 ;;
    -O*) ;;
    *) args+=("$a") ;;
  esac
done

mkdir -p "$BC_DIR"
bc="$BC_DIR/$(echo "$src" | tr '/' '_').bc"
"$CLANG" "${args[@]}" -O1 -g -fno-vectorize -fno-slp-vectorize \
  -fno-unroll-loops -emit-llvm -o "$bc" 2>/dev/null || rm -f "$bc"
exit 0
