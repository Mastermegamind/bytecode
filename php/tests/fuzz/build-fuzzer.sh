#!/usr/bin/env bash
#
# Build the opdump extension instrumented for coverage-guided fuzzing with
# AddressSanitizer + UndefinedBehaviorSanitizer. The resulting .so is NOT for
# production -- it is only loaded by run-fuzz.sh against the native bytecode
# deserializer (opdump_read / opdump_read_raw_stream and everything it calls).
#
# Why this shape:
#   * The deserializer has no userland entry point; the loader hooks
#     zend_compile_file. In "auto" mode it compiles any file whose first four
#     bytes are the OPD2 (plaintext) or BYTC (encrypted) magic through
#     opdump_read(). So a fuzz input file IS the target: AFL's @@ becomes the
#     script php compiles, and a raw OPD2 blob drives the full deserializer
#     without needing any key (crypto is deliberately NOT the fuzz target).
#   * Built with afl-clang-fast so AFL gets edge coverage + a fork server.
#   * USE_ZEND_ALLOC=0 at run time routes emalloc() through malloc() so ASan
#     actually sees heap overflows instead of them landing inside Zend's pool.
#
# Usage: php/tests/fuzz/build-fuzzer.sh [php-config]
set -euo pipefail

here="$(cd "$(dirname "$0")" && pwd)"
src_dir="$(cd "$here/../../src" && pwd)"
php_config="${1:-${PHP_CONFIG:-php-config}}"

cc="${CC:-afl-clang-fast}"
if ! command -v "$cc" >/dev/null 2>&1; then
    echo "warning: $cc not found; falling back to clang (no AFL instrumentation)" >&2
    cc="clang"
fi

# ASan/UBSan. AFL_USE_ASAN lets afl-clang-fast add the right flags; we also set
# them explicitly so a plain-clang fallback still gets sanitizers.
san="-fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=all -g -O1"

export CC="$cc"
export AFL_USE_ASAN=1
export CFLAGS="${CFLAGS:-} $san"
export LDFLAGS="${LDFLAGS:-} $san"

# afl-clang-fast does NOT link the AFL runtime into a -shared object (it expects
# the main executable to carry it), and libtool silently drops a raw .o passed
# through LDFLAGS. Without the runtime the .so fails to load under the
# uninstrumented host php with `undefined symbol: __afl_area_ptr`. So we let the
# normal build produce the instrumented PIC object, then relink it ourselves
# below with the runtime object linked in. Its constructor starts the fork
# server on dlopen and no-ops when run outside afl-fuzz, so `once` reproduction
# under a plain php still works.
afl_rt=""
if [ "$cc" = "afl-clang-fast" ]; then
    for cand in /usr/lib/afl/afl-compiler-rt.o /usr/lib/afl/afl-compiler-rt-64.o \
                "$(afl-config --libdir 2>/dev/null)/afl-compiler-rt.o"; do
        [ -f "$cand" ] && { afl_rt="$cand"; break; }
    done
    [ -n "$afl_rt" ] || echo "warning: afl-compiler-rt.o not found; .so may not load under php" >&2
fi

phpize_bin="$("$php_config" --prefix)/bin/phpize"
[ -x "$phpize_bin" ] || phpize_bin="phpize"

cd "$src_dir"
echo "==> phpize ($phpize_bin)"
"$phpize_bin" --clean >/dev/null 2>&1 || true
"$phpize_bin" >/dev/null

echo "==> configure (CC=$CC)"
./configure --with-php-config="$php_config" >/dev/null

echo "==> make"
make -j"$(nproc)" >/dev/null

so="$src_dir/modules/opdump.so"

# Relink the instrumented PIC object ourselves, because the extension is
# dlopen()ed by an UNinstrumented host php:
#   * Plain `clang` (not afl-clang-fast) for the link -- afl-clang-fast filters
#     an explicitly-passed afl runtime object back out of a -shared link.
#   * `-shared-libasan` so the ASan runtime is a shared dependency we can
#     LD_PRELOAD ahead of php (static libasan can't init in a non-asan host).
#   * The AFL runtime object linked in (defines __afl_area_ptr + the fork-server
#     constructor); omitted gracefully if AFL isn't installed.
#   * Undefined Zend/PHP symbols are resolved by php at load, so NO -z defs.
echo "==> relink (clang -shared-libasan${afl_rt:+ + AFL rt})"
ssl_libs="$(pkg-config --libs openssl 2>/dev/null || echo '-lssl -lcrypto')"
clang -shared -fsanitize=address,undefined -shared-libasan -g -O1 \
    "$src_dir/.libs/opdump.o" $afl_rt $ssl_libs \
    -o "$so"

echo "built: $so"
echo "$so"
