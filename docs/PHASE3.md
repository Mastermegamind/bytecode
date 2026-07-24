# Phase 3 — Reflection Parity

Phase 3 turns the Phase 0 Reflection spot check into a systematic parity suite.
It is blocked until class/method reconstruction works.

## Scope

Compare source-compiled code against loader-reconstructed code for:

- `ReflectionClass`
- `ReflectionMethod`
- `ReflectionParameter`
- `ReflectionProperty`
- constructor promotion and typed properties
- defaults, nullable types, named types, unions/intersections where supported

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
