#!/usr/bin/env bash
# Broader class-shape parity sweep. Each supported case is executed once from
# plaintext and once from a BYTC container replacing that plaintext; enum cases
# are expected to fail closed at dump time until enum object persistence lands.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
SRC="$ROOT/php/src"
SO="$SRC/modules/opdump.so"
PHP_BIN="${PHP_BIN:-php8.4}"
KEY="${BYTECODE_KEY:-0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef}"

if [[ ! -f "$SO" ]]; then
  echo "opdump.so not built -- run: phpize8.4 && ./configure --with-php-config=/usr/bin/php-config8.4 && make" >&2
  exit 1
fi

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

run_case() {
  local name="$1"
  local code="$2"
  local call="$3"
  local d="$TMP/$name"
  local out="$TMP/$name.out"
  mkdir -p "$d" "$out"
  printf '%s' "$code" > "$d/t.php"

  local base
  base="$("$PHP_BIN" -n -r "require '$d/t.php'; $call")"

  BYTECODE_KEY="$KEY" PHP_BIN="$PHP_BIN" "$PHP_BIN" -n "$ROOT/php/bin/bytecode-dump" "$d/t.php" "$out" >/dev/null
  cp "$out/t.php" "$d/t.php"

  local encoded
  encoded="$(
    BYTECODE_KEY="$KEY" "$PHP_BIN" -n -d zend_extension="$SO" \
      -r "require '$d/t.php'; $call" 2>&1 |
      grep -avE 'without parsing|Notice|opdump: loaded'
  )"

  if [[ "$encoded" != "$base" ]]; then
    echo "$name: output mismatch" >&2
    echo "base=[$base]" >&2
    echo "enc=[$encoded]" >&2
    exit 1
  fi
  printf '%-24s PASS\n' "$name:"
}

run_dump_fail_case() {
  local name="$1"
  local code="$2"
  local d="$TMP/$name"
  local out="$TMP/$name.out"
  mkdir -p "$d" "$out"
  printf '%s' "$code" > "$d/t.php"

  local dump_output
  if dump_output="$(BYTECODE_KEY="$KEY" PHP_BIN="$PHP_BIN" "$PHP_BIN" -n "$ROOT/php/bin/bytecode-dump" "$d/t.php" "$out" 2>&1)"; then
    echo "$name: expected dump to fail closed, but it succeeded" >&2
    exit 1
  fi
  if ! grep -qiE 'enum cases are not yet supported|unsupported value type|refusing|dump failed for' <<<"$dump_output"; then
    echo "$name: dump failed, but not with an expected fail-closed diagnostic" >&2
    echo "$dump_output" >&2
    exit 1
  fi
  printf '%-24s FAIL-CLOSED\n' "$name:"
}

run_case 'iface-const' \
  '<?php interface I { const V = 7; } class A implements I {}' \
  'echo A::V;'

run_case 'static-factory' \
  '<?php class A { public int $v = 0; static function make(int $x): static { $o = new static; $o->v = $x; return $o; } }' \
  'echo A::make(5)->v;'

run_case 'closure-in-method' \
  '<?php class A { function run(): array { $f = fn($x) => $x * 2; return array_map($f, [1, 2, 3]); } }' \
  'echo implode(",", (new A)->run());'

run_case 'match-expr' \
  '<?php class A { function k($x): string { return match ($x) { 1 => "one", 2 => "two", default => "?" }; } }' \
  'echo (new A)->k(2);'

run_case 'generator' \
  '<?php class A { function gen(): iterable { yield 1; yield 2; yield 3; } }' \
  'echo implode(",", iterator_to_array((new A)->gen()));'

run_case 'first-class-callable' \
  '<?php class A { function m($x) { return $x + 1; } function get(): Closure { return $this->m(...); } }' \
  '$c = (new A)->get(); echo $c(9);'

run_case 'private-promoted-readonly' \
  '<?php final readonly class A { public function __construct(private string $logger) {} public function run(): string { return $this->logger; } }' \
  'echo (new A("ok"))->run();'

run_case 'namespaced-promoted-iface' \
  '<?php namespace App\MessageHandlers; interface LoggerInterface { public function info(): string; } final class Logger implements LoggerInterface { public function info(): string { return "ok"; } } final readonly class RequestAuditHandler { public function __construct(private LoggerInterface $logger) {} public function run(): string { return $this->logger->info(); } }' \
  'echo (new App\MessageHandlers\RequestAuditHandler(new App\MessageHandlers\Logger()))->run();'

run_dump_fail_case 'enum-backed' \
  '<?php enum Suit: string { case H = "h"; case S = "s"; function lbl(): string { return $this->value; } }'

echo "class shapes: PASS"
