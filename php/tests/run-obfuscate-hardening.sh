#!/usr/bin/env bash
# Bytecode-layer obfuscation hardening (all behaviour-preserving metadata edits):
#   --strip-line-info   zero line numbers (backtrace lines blanked)
#   --obfuscate-keep    leave named locals un-scrambled
#   --obfuscate-seed    deterministic, reproducible scrambled names
#   --watermark         embed an opaque per-build marker
#   --obfuscate-profile paranoid  (hexa names + strip-line-info + strip-paths)
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
root="$(cd "$here/../.." && pwd)"
so="$root/php/src/modules/opdump.so"
php_bin="${PHP_BIN:-php}"
dump="$root/php/bin/bytecode-dump"
key="${BYTECODE_KEY:-0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef}"
[ -f "$so" ] || { echo "FAIL: build php/src first"; exit 1; }

work="$(mktemp -d)"; trap 'rm -rf "$work"' EXIT
cat > "$work/t.php" <<'PHP'
<?php
function calc(int $n): int { $keepMe = 1; $secretTmp = 2; return $n + $keepMe + $secretTmp; }
function boom(): void { throw new RuntimeException('x'); }
PHP

strings_has() { grep -aqF "$2" "$1"; }

# --- keep-list + watermark (raw blob inspection) ---
BYTECODE_KEY="$key" PHP_BIN="$php_bin" "$php_bin" -n "$dump" --raw --obfuscate \
    --obfuscate-keep keepMe --watermark "trace-77" "$work/t.php" "$work/raw" >/dev/null 2>&1
blob="$work/raw/t.php.opd2"
strings_has "$blob" keepMe    || { echo "FAIL: --obfuscate-keep did not preserve keepMe"; exit 1; }
strings_has "$blob" secretTmp && { echo "FAIL: secretTmp should have been scrambled"; exit 1; }
strings_has "$blob" "trace-77" || { echo "FAIL: watermark not embedded"; exit 1; }

# --- seed determinism: same seed => identical bytes; different seed => differ ---
for s in A A B; do
    BYTECODE_KEY="$key" PHP_BIN="$php_bin" "$php_bin" -n "$dump" --raw --obfuscate \
        --obfuscate-seed "seed-$s" "$work/t.php" "$work/seed-$s-$RANDOM" >/dev/null 2>&1
done
h1=""; h2=""; h3=""
BYTECODE_KEY="$key" "$php_bin" -n "$dump" --raw --obfuscate --obfuscate-seed S1 "$work/t.php" "$work/s1" >/dev/null 2>&1
BYTECODE_KEY="$key" "$php_bin" -n "$dump" --raw --obfuscate --obfuscate-seed S1 "$work/t.php" "$work/s1b" >/dev/null 2>&1
BYTECODE_KEY="$key" "$php_bin" -n "$dump" --raw --obfuscate --obfuscate-seed S2 "$work/t.php" "$work/s2" >/dev/null 2>&1
h1="$(sha256sum "$work/s1/t.php.opd2"  | cut -d' ' -f1)"
h2="$(sha256sum "$work/s1b/t.php.opd2" | cut -d' ' -f1)"
h3="$(sha256sum "$work/s2/t.php.opd2"  | cut -d' ' -f1)"
[ "$h1" = "$h2" ] || { echo "FAIL: same --obfuscate-seed produced different output"; exit 1; }
[ "$h1" != "$h3" ] || { echo "FAIL: different --obfuscate-seed produced identical output"; exit 1; }

# --- paranoid profile: hexa-style names, no original locals, behaviour intact ---
BYTECODE_KEY="$key" PHP_BIN="$php_bin" "$php_bin" -n "$dump" --raw --obfuscate-profile paranoid \
    "$work/t.php" "$work/par" >/dev/null 2>&1
strings_has "$work/par/t.php.opd2" secretTmp && { echo "FAIL: paranoid left secretTmp"; exit 1; }

# --- --strip-line-info: exception line becomes 0, behaviour preserved ---
BYTECODE_KEY="$key" PHP_BIN="$php_bin" "$php_bin" -n "$dump" --strip-line-info \
    "$work/t.php" "$work/enc" >/dev/null 2>&1
cp "$work/enc/t.php" "$work/t.php"
out="$(T="$work/t.php" BYTECODE_KEY="$key" "$php_bin" -n -d zend_extension="$so" -r '
require getenv("T");
echo calc(10);
try { boom(); } catch (Throwable $e) { echo "|line=", $e->getLine(); }
' 2>/dev/null | grep -avE "opdump|without parsing" | tr -d '\n')"
if [ "$out" = "13|line=0" ]; then
    echo "PASS: hardening flags (keep/seed/watermark/paranoid/strip-line) all correct"
    exit 0
fi
echo "FAIL: unexpected output [$out] (want 13|line=0)"
exit 1
