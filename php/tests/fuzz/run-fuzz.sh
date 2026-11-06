#!/usr/bin/env bash
#
# Drive afl-fuzz (or a single reproduction run) against the bytecode
# deserializer. The input file is the target: in auto mode the opdump hook
# compiles any OPD2/BYTC-magic file through opdump_read(), so AFL mutating @@
# feeds mutated bytecode straight into opdump_read_raw_stream() and everything
# it calls.
#
# Expected, NON-crash outcomes (these are the guards doing their job):
#   * fatal "opdump: ... exceeds limit" / "truncated or malformed blob"
#     -> clean exit, AFL records it as a normal (non-crashing) path.
# Real findings:
#   * ASan/UBSan abort (SIGABRT) -> AFL files it under crashes/ -- a memory or
#     UB bug in the deserializer. That is the whole point.
#
# Usage:
#   run-fuzz.sh fuzz  <opdump.so> [seconds]      # time-boxed afl-fuzz campaign
#   run-fuzz.sh once  <opdump.so> <input-file>   # reproduce one input, verbose
set -euo pipefail

mode="${1:?usage: run-fuzz.sh fuzz|once ...}"
so="${2:?path to instrumented opdump.so}"
so="$(cd "$(dirname "$so")" && pwd)/$(basename "$so")"   # zend_extension needs absolute
here="$(cd "$(dirname "$0")" && pwd)"
php_bin="${PHP_BIN:-php8.5}"

# Exercise the deserializer's parse + teardown path only; do NOT execute the
# reconstructed bytecode. Executing a semantically-malformed op_array is unsafe
# by design (same trust model as OPcache), so it is out of scope for parser
# fuzzing and would otherwise drown real parser bugs in execute_ex noise.
export OPDUMP_FUZZ_PARSE_ONLY=1
# ASan sees emalloc() only when Zend's pool allocator is disabled.
export USE_ZEND_ALLOC=0
# PHP's shutdown leak reports are noise here; we care about overflows/UB.
# verify_asan_link_order=0 because php (the host) is not itself asan-built.
export ASAN_OPTIONS="${ASAN_OPTIONS:-abort_on_error=1:detect_leaks=0:allocator_may_return_null=1:verify_asan_link_order=0}"
export UBSAN_OPTIONS="${UBSAN_OPTIONS:-print_stacktrace=1:halt_on_error=1}"

# The shared ASan runtime must load before php; preload it. afl-fuzz forwards
# AFL_PRELOAD to the target as LD_PRELOAD, so use that in fuzz mode.
asanrt="${ASAN_RT:-$(clang -print-file-name=libclang_rt.asan-x86_64.so 2>/dev/null)}"

case "$mode" in
  once)
    input="${3:?usage: run-fuzz.sh once <opdump.so> <input-file>}"
    exec env OPDUMP_MODE=auto LD_PRELOAD="$asanrt" \
        "$php_bin" -n -d zend_extension="$so" "$input"
    ;;
  fuzz)
    seconds="${3:-0}"   # 0 = run until interrupted
    corpus="$here/corpus"
    findings="$here/findings"
    [ -d "$corpus" ] && [ -n "$(ls -A "$corpus" 2>/dev/null)" ] || {
        echo "no seed corpus; run make-seeds.sh first" >&2; exit 2; }
    mkdir -p "$findings"
    command -v afl-fuzz >/dev/null 2>&1 || { echo "afl-fuzz not found" >&2; exit 2; }

    export AFL_SKIP_CPUFREQ=1 AFL_I_DONT_CARE_ABOUT_MISSING_CRASHES=1
    export AFL_PRELOAD="$asanrt"          # forwarded to the target as LD_PRELOAD
    export AFL_USE_ASAN=1
    export AFL_IGNORE_UNKNOWN_ENVS=1      # USE_ZEND_ALLOC is ours, not AFL's
    # The instrumentation + fork server live in the dlopened .so, not in the php
    # binary AFL inspects, so skip the static binary check and trust the
    # runtime fork server the .so's constructor starts.
    export AFL_SKIP_BIN_CHECK=1
    # afl-fuzz refuses a custom ASAN_OPTIONS without symbolize=0 (symbolizing in
    # the child wastes time); keep symbols on only for `once` reproduction.
    export ASAN_OPTIONS="$ASAN_OPTIONS:symbolize=0"
    args=(-i "$corpus" -o "$findings" -m none)   # -m none: ASan needs a huge VM
    [ "$seconds" -gt 0 ] && args+=(-V "$seconds")
    # @@ is the mutated input file, loaded in auto mode as the compiled script.
    exec env OPDUMP_MODE=auto afl-fuzz "${args[@]}" -- \
        "$php_bin" -n -d zend_extension="$so" @@
    ;;
  *)
    echo "unknown mode: $mode (expected fuzz|once)" >&2
    exit 2
    ;;
esac
