Build a minimal Zend extension that proves PHP opcode dump/reload is viable, as
Phase 0 of a custom PHP bytecode encoder (ionCube-style) targeting PHP
8.1–8.5. This is a go/no-go feasibility spike, not the real product — scope it
tight and stop as soon as the question is answered either way.

## Environment (already verified present, do not reinstall)

- Target dev machine: this one. PHP 8.1–8.5 are all installed side by side
  with dev headers: `php8.1`..`php8.5`, `phpize8.1`..`phpize8.5`,
  `php-config8.1`..`php-config8.5` all resolve. Zend headers live under
  `/usr/include/php/<api-version>/Zend/`.
- Start with **PHP 8.4 only** (`phpize8.4` / `php-config8.4`). Do not touch
  8.1/8.2/8.3/8.5 until 8.4 works end-to-end — this is Phase 2's job, not
  Phase 0's.
- Build in `/var/www/bytecode/php/` (the `bytecode` repo's PHP backend —
  this has nothing to do with the exam.test app, that project is unrelated).

## What "done" looks like for this spike

A shared object (`opdump.so`) loadable via `zend_extension=` that supports two
modes:

1. **Dump mode**: given a `.php` file, compile it normally (let
   `zend_compile_file` run as usual), then serialize the resulting
   `zend_op_array` to a binary blob on disk — no encryption yet, that's
   Phase 1. Walk the struct by hand: opcodes array, literals table (as
   `zend_string*` — must re-derive raw bytes + length, don't dump pointers),
   variable names table, `arg_info`, `try_catch_array`, doc comment, function
   name/flags. Do NOT attempt a generic/automatic serializer — hand-write the
   walk for exactly the fields a real script exercises, and extend field by
   field as tests demand it.
2. **Load mode**: given a dumped blob, hook `zend_compile_file` so that
   instead of parsing source text, it reconstructs a `zend_op_array` in the
   process's own memory (via `zend_arena_alloc` / the same allocator paths
   normal compilation uses — do not just `malloc` raw structs, the engine's
   GC and refcounting assume its own allocator), re-interns every string via
   `zend_new_interned_string` (critical — a `zend_string*` from a different
   process is garbage; strings must be re-created against *this* process's
   interned string table), and returns it from the hook exactly as
   `zend_compile_file` normally would, so the executor can't tell the
   difference.

## Test ladder (each must pass before moving to the next)

1. `<?php echo "hello world";` — proves the dump/reload/execute loop works
   at all.
2. Add a function definition + call with a typed parameter and a default
   value — proves `arg_info` and default-value handling round-trip.
3. Add a class with a constructor, a typed property, and a method call on
   `$this` — proves class table / method table reconstruction.
4. Add a `try`/`catch` block — proves `try_catch_array` round-trips.
5. **Reflection check**: for the class from step 3, compare
   `(new ReflectionMethod($class,'method'))->getParameters()[0]->isDefaultValueAvailable()`
   (and `getType()`, `allowsNull()`) between the source-compiled version and
   the loader-reconstructed version. This exact API is what silently broke
   under ionCube and is why the earlier attempt at commercial encoding got
   abandoned for this project — if it's already wrong at this tiny scale,
   that's the go/no-go answer right there, stop and report it rather than
   pushing forward.

## Explicitly out of scope for this spike (do not build yet)

- Encryption/key management (Phase 1)
- PHP 8.1/8.2/8.3/8.5 support (Phase 2)
- OPcache interaction (Phase 4)
- Any CLI packaging, installer, or licensing model
- Any integration with the real exam.test codebase

## Deliverable

A short written report: does the dump/reload/execute loop work for all 5 test
rungs above, what had to be hand-coded per struct field, what broke and why,
and an honest go/no-go recommendation on whether Phase 2 (the 8.1–8.5 matrix,
which is the actual multi-month bulk of this project) is worth starting. Keep
the C source in this directory so the recommendation can be checked against
actual code, not just claims.
