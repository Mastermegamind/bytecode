#!/usr/bin/env bash
# Rung 9: safe variable-name obfuscation. bytecode-dump --obfuscate renames
# non-parameter local variables to _v0, _v1, ... in the dumped OPD2 blob.
# This is safe because compiled opcodes reference CVs by integer slot index,
# never by name -- op_array->vars[] is purely a name-lookup table, so
# renaming it is a pure metadata edit with zero effect on control flow.
# Confirms: (1) plain locals get renamed and execution is still correct,
# (2) a function using compact() is left untouched because
# opdump_op_array_uses_dynamic_vars() detects it, and (3) parameters are
# never renamed (they'd otherwise diverge from arg_info[i].name, which
# named-argument matching and ReflectionParameter::getName() rely on).
#
# Checks are exact byte-level matches against the OPD2 wire format's
# length-prefixed strings (u32 LE length + bytes, see w_str() in
# php/src/opdump.c) rather than a heuristic tool like strings(1), so a
# 1-3 byte name isn't missed and a substring can't produce a false match.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
SRC="$ROOT/php/src"
SO="$SRC/modules/opdump.so"
PHP_BIN="${PHP_BIN:-php8.4}"
KEY="${BYTECODE_KEY:-0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef}"
SUBJECT="$ROOT/php/tests/rung9_obfuscate_subject.php"

if [[ ! -f "$SO" ]]; then
  echo "opdump.so not built -- run: phpize8.4 && ./configure --with-php-config=/usr/bin/php-config8.4 && make" >&2
  exit 1
fi

OUT="$(mktemp -d)"
trap 'rm -rf "$OUT"' EXIT

BYTECODE_KEY="$KEY" PHP_BIN="$PHP_BIN" "$ROOT/php/bin/bytecode-dump" --raw --obfuscate "$SUBJECT" "$OUT" >/dev/null
BLOB="$OUT/rung9_obfuscate_subject.php.opd2"

# u32 LE length prefix (3 == strlen('sum'), etc.) immediately followed by the
# literal bytes, matching w_str()'s wire format exactly.
has_string() {
  local len_hex
  len_hex="$(printf '\\x%02x\\x%02x\\x%02x\\x%02x' $(( $2 & 0xff )) $(( ($2 >> 8) & 0xff )) $(( ($2 >> 16) & 0xff )) $(( ($2 >> 24) & 0xff )))"
  grep -qacP "${len_hex}$3" "$1"
}

echo "--- plain locals ('sum', 'extra') are renamed ---"
if has_string "$BLOB" 3 'sum'; then
  echo "expected 'sum' to be renamed away under --obfuscate" >&2
  exit 1
fi
if has_string "$BLOB" 5 'extra'; then
  echo "expected 'extra' to be renamed away under --obfuscate" >&2
  exit 1
fi
if ! has_string "$BLOB" 3 '_v2'; then
  echo "expected a renamed local named _v2 (first non-parameter CV in plain_locals)" >&2
  exit 1
fi

echo
echo "--- compact()-using function is left untouched ---"
if ! has_string "$BLOB" 5 'local'; then
  echo "expected 'local' to survive obfuscation: uses_compact() calls compact('local')" >&2
  exit 1
fi

echo
echo "--- parameters are never renamed ---"
if ! has_string "$BLOB" 1 'a'; then
  echo "expected parameter 'a' to survive obfuscation" >&2
  exit 1
fi
if ! has_string "$BLOB" 1 'b'; then
  echo "expected parameter 'b' to survive obfuscation" >&2
  exit 1
fi
if ! has_string "$BLOB" 4 'seed'; then
  echo "expected parameter 'seed' to survive obfuscation" >&2
  exit 1
fi

echo
echo "--- execution output is unchanged by obfuscation ---"
DECOY="$(mktemp --suffix=.php)"
printf '<?php this is not valid php syntax !!! %%%%%%\n' > "$DECOY"
output="$(OPDUMP_MODE=load OPDUMP_IN="$BLOB" "$PHP_BIN" -n -d extension="$SO" -f "$DECOY" | tail -n 2)"
rm -f "$DECOY"
expected="$(printf '10\n6')"
if [[ "$output" != "$expected" ]]; then
  echo "unexpected output: $output (expected: $expected)" >&2
  exit 1
fi

echo
echo "rung 9 obfuscate: PASS"
