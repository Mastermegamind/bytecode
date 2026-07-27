#!/usr/bin/env bash
# Verifies the YAKPro-PO-inspired local-variable scrambler:
#   * original local names never appear in the compiled bytecode,
#   * the predictable _v0/_v1 sequence is gone (names are randomized),
#   * property doc-comments are stripped under --obfuscate,
#   * and none of this changes runtime behaviour.
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
root="$(cd "$here/../.." && pwd)"
so="$root/php/src/modules/opdump.so"
php_bin="${PHP_BIN:-php}"
[ -f "$so" ] || { echo "FAIL: build php/src first"; exit 1; }

work="$(mktemp -d)"; trap 'rm -rf "$work"' EXIT
cat > "$work/t.php" <<'PHP'
<?php
function calc(): int {
    $accumulator = 0; $iterationLimit = 6;
    for ($stepIndex = 0; $stepIndex < $iterationLimit; $stepIndex++) { $accumulator += $stepIndex; }
    return $accumulator;
}
class Widget {
    /** confidential finance tally */
    private int $secretBalance = 42;
    public function reveal(): int { return $this->secretBalance; }
}
PHP

# --- raw (unencrypted) dump so we can inspect the actual bytecode ---
"$php_bin" -n "$root/php/bin/bytecode-dump" --raw --obfuscate "$work/t.php" "$work/raw" >/dev/null 2>&1
blob="$work/raw/t.php.opd2"
for n in accumulator iterationLimit stepIndex; do
    if grep -aqF "$n" "$blob"; then echo "FAIL: original local '$n' leaked into bytecode"; exit 1; fi
done
for n in _v0 _v1 _v2; do
    if grep -aqF "$n" "$blob"; then echo "FAIL: predictable name '$n' present (not randomized)"; exit 1; fi
done
if grep -aqF "confidential finance tally" "$blob"; then echo "FAIL: doc-comment not stripped"; exit 1; fi

# --- encrypted encode + run: behaviour and reflection ---
key="$("$php_bin" "$root/php/bin/bytecode-keygen")"
BYTECODE_KEY="$key" PHP_BIN="$php_bin" "$php_bin" -n "$root/php/bin/bytecode-dump" --obfuscate "$work/t.php" "$work/enc" >/dev/null 2>&1
cp "$work/enc/t.php" "$work/t.php"
out="$(T="$work/t.php" BYTECODE_KEY="$key" "$php_bin" -n -d zend_extension="$so" -r '
require getenv("T");
$r = new ReflectionProperty(Widget::class, "secretBalance");
echo calc(), "|", (new Widget)->reveal(), "|", var_export($r->getDocComment(), true);
' 2>/dev/null | grep -avE "opdump|without parsing" | tr -d '\n')"

if [ "$out" = "15|42|false" ]; then
    echo "PASS: scrambled locals + stripped doc-comment, behaviour unchanged"
    exit 0
fi
echo "FAIL: unexpected output [$out] (want 15|42|false)"
exit 1
