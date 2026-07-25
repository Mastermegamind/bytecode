# Zend Internals Notes

These are the engine invariants learned during the PHP 8.4 Phase 0 spike.
Treat them as porting checks when adapting `opdump.c` to another PHP minor.

## Compile Hook Setup

- Include `config.h` before `php.h` when `HAVE_CONFIG_H` is defined, or
  `COMPILE_DL_OPDUMP` may not be visible when `ZEND_GET_MODULE()` is expanded.
- If the loader bypasses `zend_compile_file()`, it must set and restore
  `CG(compiled_filename)` around `init_op_array()`. The normal compiler path
  does this before initializing the op_array.

## Opcode/Literal Memory Layout

- Constant operands are relative to the owning opline on 64-bit builds where
  `ZEND_USE_ABS_CONST_ADDR == 0`.
- Opcodes and literals must live in the same allocation block using the layout
  produced by Zend `pass_two()`: aligned opcodes first, literals immediately
  after them.
- Assign opcode handlers only after the complete opcode array has been read.
  Some handlers, such as `ZEND_ASSIGN_OBJ`, inspect neighboring opcodes.

## Functions

- Top-level user functions compiled from a file may already be present in
  `CG(function_table)` after compilation rather than attached to the main
  op_array as dynamic function definitions.
- The loader must serialize matching user functions from the compile-time
  function table and register them into `EG(function_table)` before executing
  loader-reconstructed code.
- Return type metadata is stored in the slot before the first parameter:
  `op_array->arg_info[-1]`. Dropping that slot breaks return type checks and
  `ReflectionMethod::getReturnType()`.
- Parameter default execution can be preserved through `ZEND_RECV_INIT` opcodes
  and literals, but Reflection parity still needs explicit validation for
  `ReflectionParameter::isDefaultValueAvailable()` and default values.

## Classes

- Some top-level classes are early-linked during compilation. Reconstructed
  linked classes must remain linked, or lookups through
  `zend_fetch_class_by_name()` can reject entries that are visibly present in
  `EG(class_table)`.
- Method op_arrays, default property slots, property type metadata, promoted
  property flags, readonly flags, and constructor parameter metadata all affect
  Reflection-driven containers.

## Version Porting Checklist

For each PHP minor:

- rebuild against that version's `phpize` and `php-config`
- inspect `zend_op_array`, `zend_arg_info`, `zend_class_entry`, property-info,
  and type-info layout changes
- run the full rung suite and `run-reflection-parity.sh`
- record any version-specific layout or handler differences in `PHASE2.md`
- ensure blobs are guarded by `PHP_VERSION_ID` before execution
