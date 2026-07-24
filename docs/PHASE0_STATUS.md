# Phase 0 status

Rung 1 (`echo "hello world";`, no functions/classes) — **PASS**, verified,
not assumed: `php/tests/run-rung1.sh` dumps the compiled op_array, then
reloads and executes it while pointing the interpreter at a decoy file
containing deliberately invalid PHP syntax. Output still matches, proving
the loader path never parses or even reads real source text — only the
binary blob drives execution.

Rung 2 (function definition + call with typed parameter default) — **PASS**,
verified with `php/tests/run-rung2.sh` on PHP 8.4. The blob now carries
top-level user functions compiled from the same filename, registers them into
the fresh process's executor function table before execution, and round-trips
the function op_array, CV variable names, parameter names, simple scalar
parameter types, opcodes, and literals. The default value itself is preserved
through the function body's `ZEND_RECV_INIT` opcode/literal pair; the
`arg_info.default_value` pointer is intentionally not persisted yet because it
was not a valid standalone `zend_string*` in the compiled user function on this
PHP 8.4 build.

Rung 3 (class with constructor, typed property, and method call on `$this`) —
**PASS**, verified with `php/tests/run-rung3.sh` on PHP 8.4. The blob now
carries user classes compiled from the same filename, class method op_arrays,
default property slots, and typed property metadata. The first implementation
bug here was instructive: opcode handlers must be assigned after the entire
opcode array is read, because handlers such as `ZEND_ASSIGN_OBJ` inspect the
following `OP_DATA` opcode.

Rung 4 (try/catch/finally) — **PASS**, verified with
`php/tests/run-rung4.sh` on PHP 8.4. The blob now carries
`try_catch_array` entries so exception jumps behave the same after reload.

Rung 5 (Reflection parity) — **PASS**, verified with
`php/tests/run-rung5-reflection.sh` on PHP 8.4. The test snapshots
`ReflectionClass`, constructor `ReflectionParameter` metadata, named and
nullable types, optional defaults, promoted readonly properties, method return
types, and a real method call. Return metadata is persisted through
`arg_info[-1]`, matching Zend's own layout.

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

4. **Top-level function declarations are not `dynamic_func_defs`.** For rung 2,
   the main op_array had no dynamic function definitions; the compiled user
   function was already present in `CG(function_table)` after
   `zend_compile_file()` returned. The loader must therefore serialize matching
   user functions from the compiler function table and add them back to
   `EG(function_table)` in the fresh process before execution, or the first
   userland function call crashes/looks up nothing useful.

5. **`arg_info.default_value` is not a portable blob field here.** The typed
   default value executed correctly only after preserving the function opcodes
   and literals; directly treating `zend_arg_info.default_value` as a
   `zend_string*` crashed during dump. Reflection parity may still require a
   more precise reconstruction later, but rung 2 execution does not.

6. **Linked user classes must remain linked.** For simple top-level classes,
   PHP 8.4 may early-link the class during compilation and omit a
   `ZEND_DECLARE_CLASS` opcode. Reconstructing the class entry but clearing
   `ZEND_ACC_LINKED` leaves it present in `EG(class_table)` while
   `zend_fetch_class_by_name()` still rejects it as not found.

7. **Some VM handlers depend on neighboring opcodes.** `ZEND_ASSIGN_OBJ`
   chooses a specialized handler using the following `OP_DATA` opcode. Setting
   opcode handlers while reading opcodes one by one produced
   `Invalid opcode 24/0/1`; setting handlers only after all opcodes are present
   fixed constructor property assignment.

8. **Return types live before the first parameter.** User functions with
   return types allocate one extra `zend_arg_info` slot and then point
   `op_array->arg_info` after it. The return metadata is therefore
   `arg_info[-1]`. Losing that slot makes return type checks and
   `ReflectionMethod::getReturnType()` wrong.

None of this was discoverable from the public header comments alone —
these were found by pulling the real PHP 8.4 engine source
(`Zend/zend_opcode.c`, `Zend/zend_compile.c`, `Zend/zend.c` from the
`PHP-8.4` branch of php/php-src) and reading what the real compiler does at
each step, then matching it exactly. Expect the same kind of archaeology
for every rung and every PHP version still ahead.
