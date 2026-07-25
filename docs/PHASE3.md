# Phase 3 — Reflection Parity

Phase 3 turns the Phase 0 Reflection spot check into a systematic parity suite.
Class/method reconstruction now works for the PHP 8.4 spike, so the initial
parity harness exists and runs in the Phase 2 matrix.

## Scope

Compare source-compiled code against loader-reconstructed code for:

- `ReflectionClass`
- `ReflectionMethod`
- `ReflectionParameter`
- `ReflectionProperty`
- constructor promotion and typed properties
- defaults, nullable types, named types, unions/intersections where supported

## Runner

The current Phase 3 groundwork is:

```bash
php/tests/run-reflection-parity.sh
```

It snapshots source-compiled metadata, dumps the subject file to `BYTC1`,
replaces that subject source with invalid PHP, reflects the reconstructed class
through `OPDUMP_MODE=load-tree`, and compares the loader snapshot against the
source snapshot. The probe stays normal PHP so failures stay focused on subject
metadata rather than probe-helper serialization. Failures are reported as JSON
paths via
`php/tests/reflection_parity_diff.php`.

The corpus currently covers:

- promoted readonly constructor properties
- parameter defaults and `isDefaultValueAvailable()`
- nullable named types
- promoted property visibility/type metadata
- a real method call after reflection has been captured

Class constants, array/default property slots, interfaces,
union/intersection types, and `never` return types are intentionally listed as
next corpus additions. The first harness stays aligned with reconstruction
behavior that already passes the Phase 0 loader ladder.

## Critical Gate

The non-negotiable check is:

```php
(new ReflectionMethod($class, 'method'))
    ->getParameters()[0]
    ->isDefaultValueAvailable()
```

It must match source compilation exactly. This is the known failure mode that
motivated the project.

## Done Criteria

1. Reflection source-vs-loader snapshots match for the supported corpus.
2. Failures print field-level diffs, not just pass/fail.
3. The suite runs against every Phase 2 PHP version.
