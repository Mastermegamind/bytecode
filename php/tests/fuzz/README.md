# Deserializer fuzzing

The native loader reconstructs live Zend `op_array`/class structures in C from
a decrypted container payload ([`php/src/opdump.c`](../../src/opdump.c),
`opdump_read` → `opdump_read_raw_stream` → `opdump_read_op_array` /
`opdump_read_class_entry` / `opdump_read_ast` / ...). That path is the highest
severity surface in the project: a count, length, or index bug there turns a
crafted container into memory corruption. Every input-derived count and length
is bounded by an `OPDUMP_MAX_*` guard and truncation fails closed — this
harness exists to keep that true under mutation, not just by inspection.

## Threat framing

Crypto is **not** the target. A valid `BYTC` container can only be produced by
someone with the key, but (a) a license holder is semi-trusted and can craft
containers, and (b) the parser must stay memory-safe on any input. So we fuzz
the **plaintext `OPD2`** layer directly: the loader compiles any file whose
first four bytes are `OPD2` (or `BYTC`) through `opdump_read()`, so an `OPD2`
seed exercises the full deserializer with no key involved.

- A guard firing (`... exceeds limit`, `truncated or malformed blob`,
  `null ... key`, `unknown opcode`) is a **clean exit** — the defense working,
  not a crash.
- An ASan/UBSan abort is a **real finding**: a memory-safety or UB bug.

### Scope: parse + teardown, not execution

The harness runs with `OPDUMP_FUZZ_PARSE_ONLY=1` (set by `run-fuzz.sh`), which
makes the loader deserialize a container and then tear it down
(`destroy_op_array`) **without executing** the reconstructed bytecode.

That boundary is deliberate. Executing a *structurally* well-formed but
*semantically* malformed op_array — opcodes whose operands point at
out-of-range CVs/literals, a jump off the end — is not memory-safe and cannot
be made so without a full bytecode verifier. This is the **same trust model as
OPcache**, which executes its cache file without re-verifying it: the container
is authenticated (GCM + vendor seal), and authenticated input is trusted to be
well-formed. What the loader *must* guarantee is that **parsing and freeing**
any byte string is memory-safe — that is what this harness enforces. Fuzzing
through execution instead would bury real parser bugs under an unbounded supply
of `execute_ex` crashes that all reduce to "we ran garbage bytecode".

## Run it

```bash
cd php/tests/fuzz

# 1. Build an ASan+UBSan, AFL-instrumented loader (NOT for production).
so=$(./build-fuzzer.sh php-config | tail -1)

# 2. Generate a seed corpus of valid OPD2 blobs.
./make-seeds.sh "$so"

# 3a. Time-boxed campaign (e.g. 300s), findings land in ./findings/.
./run-fuzz.sh fuzz "$so" 300

# 3b. Reproduce a single input with full sanitizer output.
./run-fuzz.sh once "$so" findings/default/crashes/id:000000,*
```

Requirements: `afl++` (`afl-clang-fast`, `afl-fuzz`), `clang`, PHP dev headers
(`phpize`, `php-config`). Without AFL the build falls back to plain clang with
sanitizers so `run-fuzz.sh once` still reproduces inputs.

`USE_ZEND_ALLOC=0` is exported by the run script so `emalloc()` goes through
`malloc()` and ASan can see heap overflows instead of them hiding in Zend's
pool allocator.

## CI

[`.github/workflows/fuzz.yml`](../../../.github/workflows/fuzz.yml) runs a short
smoke campaign on pushes/PRs that touch `php/src/`, and uploads any `crashes/`
as an artifact. It is intentionally short (minutes); for real coverage run a
longer campaign locally or on a dedicated host and keep an evolving corpus.

## Findings fixed with this harness

The first campaigns found, and the parser was hardened against, the following
parse/teardown memory-safety bugs — all reachable from a malformed _plaintext_
`OPD2` payload (authenticated input in production, so key-holder / encoder-bug
class, not unauthenticated RCE, but "the deserializer stays memory-safe on any
input" is the invariant). Shared root cause: a payload can satisfy the
`OPDUMP_MAX_*` count guards yet be internally inconsistent.

| Symptom (ASan) | Root cause | Fix in `opdump.c` |
|----------------|-----------|-------------------|
| SEGV in `zend_hash_update` (array / static-var key) | `r_zstr` returns NULL for a serialized "absent" string; a NULL used as a hash key dereferences | `r_zstr_req()` + explicit NULL checks on every required key/name |
| SEGV in `zend_vm_set_opcode_handler` | invalid opcode byte / operand type indexes off the handler tables | `opdump_guard_opline()` validates opcode (`zend_get_opcode_name`) + operand types before the handler loop |
| SEGV in `destroy_op_array` (NULL CV name) | `op_array->vars[i]` left NULL; teardown releases every entry | reject NULL compiled-variable names |
| bad-free in `destroy_op_array` | container `fn_flags` with `ZEND_ACC_DONE_PASS_TWO` clear makes Zend free the interior `literals` pointer of our single combined opcode/literal block | force `ZEND_ACC_DONE_PASS_TWO` on load to match our always-combined layout |
| stack overflow via nested array literals | array↔literal↔AST recursion had no shared depth bound | thread one depth through all three, capped at `OPDUMP_MAX_AST_DEPTH` |

| fixed AST node allocated to a serialized child count that disagrees with the kind | Zend reads `kind >> ZEND_AST_NUM_CHILDREN_SHIFT` children regardless, so a smaller allocation is walked off | require the serialized count to equal the kind's inherent count and bound it (`OPDUMP_MAX_AST_FIXED_CHILDREN`) |

Any change here must keep passing `run-version-matrix.sh` and the
Reflection-parity rungs (valid containers must still load identically).

### Reviewed and found safe

- **`arg_info[-1]` return-type slot.** The allocation is `num_args +
  return_slots`, `arg_info` points `return_slots` past the base, and
  `return_slots` is derived from the same `ZEND_ACC_HAS_RETURN_TYPE` flag
  `destroy_op_array` uses to recover the base — so alloc, the write side, and
  teardown agree. The variadic parameter is already counted in `num_args` (no
  extra slot). Verified ASan-clean for variadic + return-type + default-bearing
  signatures; the invariant is documented at the allocation site and covered by
  the `args` seed.
