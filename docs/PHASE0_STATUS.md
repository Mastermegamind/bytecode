# Phase 0 status

Rung 1 (`echo "hello world";`, no functions/classes) — **PASS**, verified,
not assumed: `php/tests/run-rung1.sh` dumps the compiled op_array, then
reloads and executes it while pointing the interpreter at a decoy file
containing deliberately invalid PHP syntax. Output still matches, proving
the loader path never parses or even reads real source text — only the
binary blob drives execution.

Rungs 2–5 (function w/ default arg, class + method, try/catch, Reflection
parity) are not started yet.

## Real bugs hit and fixed getting rung 1 working

These are kept here because they're exactly the kind of non-obvious,
version-specific trap this whole project exists to map out — useful record
for whoever tackles rungs 2–5 or the 8.1/8.2/8.3/8.5 matrix later.

1. **`php.h` does not auto-include `config.h`.** Without
   `#ifdef HAVE_CONFIG_H #include "config.h" #endif` at the top of the file,
   `COMPILE_DL_OPDUMP` was never defined at the point `ZEND_GET_MODULE()` was
   evaluated, so no `get_module()` symbol was ever emitted and PHP reported
   "Invalid library (maybe not a PHP library)" despite the `.so` building
   without error.

2. **`init_op_array()` segfaults if `CG(compiled_filename)` is NULL.** It
   unconditionally does `filename = zend_string_copy(zend_get_compiled_filename())`.
   The real `zend_compile_file()` sets that compiler-global itself before
   ever calling `init_op_array()`; a hook that bypasses the real compile
   path entirely (as this one does in load mode) must call
   `zend_set_compiled_filename()` / `zend_restore_compiled_filename()`
   around the `init_op_array()` call itself, or it crashes inside
   `zend_string_copy(NULL)`.

3. **Constant operands are not offsets into `op_array->literals`.** On
   64-bit builds (`ZEND_USE_ABS_CONST_ADDR == 0`), `RT_CONSTANT(opline, node)`
   computes a literal's address as `(char*)opline + node.constant` — an
   offset *relative to that specific opline's own address*, not relative to
   the literals array base. This only resolves correctly if opcodes and
   literals live in one contiguous allocation, laid out exactly the way
   `pass_two()` (`Zend/zend_opcode.c`) does it: the literals array starts
   immediately after the 16-byte-aligned opcodes array, in the *same*
   `emalloc()` block. Allocating opcodes and literals as two independent
   blocks (the obvious first attempt) silently produces garbage constant
   reads — no crash, just wrong/empty output, which is a much nastier
   failure mode to debug than a segfault.

None of this was discoverable from the public header comments alone —
all three were found by pulling the real PHP 8.4 engine source
(`Zend/zend_opcode.c`, `Zend/zend_compile.c`, `Zend/zend.c` from the
`PHP-8.4` branch of php/php-src) and reading what the real compiler does at
each step, then matching it exactly. Expect the same kind of archaeology
for every rung and every PHP version still ahead.
