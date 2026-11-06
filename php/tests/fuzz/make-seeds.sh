#!/usr/bin/env bash
#
# Generate a seed corpus of valid plaintext OPD2 bytecode blobs for the
# deserializer fuzzer. Each seed is produced by dumping a small PHP snippet
# through the opdump "dump" hook (OPDUMP_MODE=dump writes OPD2, unencrypted),
# which is exactly the format opdump_read_raw_stream() parses. Good coverage of
# the parser comes from seeds that hit different structural paths: literals,
# closures/dynamic funcs, classes with properties/constants/methods, interfaces
# and traits, try/catch, typed args.
#
# Usage: php/tests/fuzz/make-seeds.sh <opdump.so> [out-dir]
set -euo pipefail

so="${1:?usage: make-seeds.sh <opdump.so> [out-dir]}"
so="$(cd "$(dirname "$so")" && pwd)/$(basename "$so")"   # zend_extension needs absolute
here="$(cd "$(dirname "$0")" && pwd)"
out="${2:-$here/corpus}"
php_bin="${PHP_BIN:-php8.5}"

# The ASan runtime must be preloaded ahead of the uninstrumented php, and the
# link-order check disabled because php itself is not asan-built.
asanrt="${ASAN_RT:-$(clang -print-file-name=libclang_rt.asan-x86_64.so 2>/dev/null)}"

mkdir -p "$out"
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT

emit() { # name  php-body
    printf '<?php\n%s\n' "$2" > "$work/$1.php"
}

emit scalars      '$a = 1 + 2; $b = "x" . PHP_EOL; $c = [1, 2.5, "three", null, true];'
emit closure      'function f(int $x, ?string $y = null): array { return [$x, $y]; } $g = fn($z) => $z * 2;'
emit class        'class C extends ArrayObject implements Countable { const K = 7; public int $n = 0; private ?string $s = "a"; public static array $t = []; public function m(self $o): int { return $this->n; } }'
emit trait        'trait T { public function h(): void {} } class U { use T; }'
emit control      'function w($a) { try { foreach ($a as $k => $v) { if ($v) throw new RuntimeException("x"); } } catch (Throwable $e) { return $e->getMessage(); } finally { $a = null; } return 0; }'
emit match        '$v = match(random_int(0,2)) { 0 => "a", 1, 2 => "b", default => "c" }; $k = ["x" => 1, "y" => [2, 3]];'
emit args         'function f(int $a, ?string $b = "x", float ...$rest): array { return $rest; } const C = [1 << 3, PHP_INT_MAX, ["n" => PHP_EOL ?: "x"]];'

n=0
for php in "$work"/*.php; do
    base="$(basename "$php" .php)"
    seed="$out/$base.opd2"
    USE_ZEND_ALLOC=0 LD_PRELOAD="$asanrt" \
        ASAN_OPTIONS=detect_leaks=0:verify_asan_link_order=0 OPDUMP_MODE=dump OPDUMP_OUT="$seed" \
        "$php_bin" -n -d zend_extension="$so" "$php" >/dev/null 2>&1 || true
    if [ ! -s "$seed" ]; then
        echo "warning: no OPD2 emitted for $base (serializer gap?); skipping" >&2
        rm -f "$seed"
        continue
    fi
    # Keep only seeds that actually round-trip (load cleanly). A seed the loader
    # rejects as truncated/malformed is a serializer gap, not a useful corpus
    # entry, and would start the fuzzer from an already-"crashing" input.
    if OPDUMP_FUZZ_PARSE_ONLY=1 USE_ZEND_ALLOC=0 LD_PRELOAD="$asanrt" \
        ASAN_OPTIONS=detect_leaks=0:verify_asan_link_order=0 OPDUMP_MODE=auto \
        "$php_bin" -n -d zend_extension="$so" "$seed" >/dev/null 2>&1; then
        n=$((n + 1))
    else
        echo "warning: $base.opd2 does not round-trip; skipping" >&2
        rm -f "$seed"
    fi
done

echo "wrote $n validated seed(s) to $out"
[ "$n" -gt 0 ]
