# Roadmap

## Phase 0 — Feasibility spike (PHP 8.4 only)

Prove the core trick works at all: a minimal Zend extension that hooks
`zend_compile_file`, dumps a compiled `zend_op_array` to a binary blob,
reloads it in a fresh process, and executes it identically to the source.
Hard go/no-go gate — see [`PHASE0.md`](PHASE0.md) for the exact spec and test
ladder. If this doesn't work cleanly, nothing downstream matters.

## Phase 1 — Container format + CLI dumper tool

A real encoding format (version/build-id header, AES-256-GCM payload), and a
CLI tool that walks a source tree and emits encoded output.

## Phase 2 — Per-version internals matrix (the actual bulk of the work)

Repeat Phase 0's core dump/reload work against PHP 8.1, 8.2, 8.3, and 8.5
(8.4 already covered by Phase 0). Each minor version has touched
`zend_op_array` / `zend_class_entry` layout — expect real breakage per
version, not just a recompile:

- **8.1** — enums (backed enum internal representation), readonly
  properties, fibers, never return type, intersection types
- **8.2** — readonly classes, disjunctive normal form types, dynamic
  properties deprecation, `Random\Randomizer` / `Random\Engine\*`
- **8.3** — typed class constants, dynamic class constant fetch,
  `#[Override]`, `json_validate`
- **8.5** — whatever ships with it; re-derive the diff against 8.4 headers
  when the spike starts

Each target needs its own build of the extension against that version's Zend
headers (`phpize8.1`/`php-config8.1` etc., all confirmed present on this
machine), ideally with CI running dump+reload round trips against a real
test corpus per version.

## Phase 3 — Reflection/introspection parity testing

Not spot-checked — a systematic test matrix asserting `ReflectionClass` /
`ReflectionMethod` / `ReflectionParameter` / `ReflectionProperty` behave
identically for loader-reconstructed classes vs. normally-compiled ones.
This is the exact class of bug that made an existing commercial PHP encoder
unusable for a real PHP-DI-based application
(`ReflectionParameter::isDefaultValueAvailable()` silently always `false`),
so it gets a dedicated phase rather than being folded into general testing.

## Phase 4 — OPcache interop, packaging, key/licensing model, anti-tamper

Deferred until 0–3 prove out. Decide whether loaded op_arrays participate in
OPcache's shm cache normally (usually desirable for performance) or bypass
it entirely; build the encoder CLI + loader `.so` packaging per PHP version
× distro/glibc combo; design key handling and container integrity checks.

## Beyond PHP

The container format and CLI driver shape are meant to be reusable across
target languages/runtimes once the PHP backend is real — but no other
backend is scaffolded yet. Adding one before Phase 0–3 are solid for PHP
would be solving a problem nobody has confirmed exists yet.
