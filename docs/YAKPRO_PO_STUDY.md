# YAK Pro PO Study Notes

Source studied: `https://github.com/Mastermegamind/yakpro-po`

Local study clone: `/tmp/yakpro-po`

License: MIT. These notes are architectural observations and an upgrade plan;
do not copy code into this project without preserving the upstream license and
reviewing whether the source-obfuscation approach still fits this bytecode
encoder.

## What Yakpro Does Well

Yakpro is an AST-based PHP source obfuscator built on `nikic/PHP-Parser`.
Its core strengths are not encryption; they are project-scale source rewriting
and operational controls around that rewriting.

Main features observed:

- Removes comments/formatting and can emit single-line PHP.
- Scrambles names for variables, functions, constants, class constants,
  classes, interfaces, traits, methods, properties, namespaces, and labels.
- Supports ignore lists and ignore-prefix lists per symbol category.
- Has reserved-name handling for PHP keywords, superglobals, magic methods,
  built-in functions/classes, and members of built-in classes.
- Supports configurable scramble modes: identifier-like, hex-like, numeric-like.
- Keeps persistent scramble context for directory builds, so unchanged symbols
  keep stable names between incremental runs.
- Provides reverse lookup with `--whatis` for debugging obfuscated builds.
- Rewrites some control flow: `if`/`elseif`/`else`, loops, `break`, and
  `continue` can become label/goto-based forms.
- Shuffles statement chunks with tunable chunk size/ratio.
- Obfuscates string literals by rendering bytes as mixed hex/octal escapes.
- Recursively processes project directories with keep/skip lists, symlink
  handling, max-depth protection, and timestamp-based incremental rebuilds.
- Has compatibility checks for difficult dynamic constructs such as
  `define()`, `defined()`, `function_exists()`, indirect calls, and callable
  arrays like `[SomeClass::class, 'methodName']`.

## Important Lessons For This Project

This project is stronger than Yakpro in one key way: encoded bytecode is not
valid readable PHP source. Source obfuscation should therefore be optional
defense-in-depth, not the core protection model.

Still, Yakpro highlights several practical needs that also apply here:

- **Preserve external contracts.** Frameworks, Composer autoloading, DI
  containers, magic methods, PDO object hydration, route callable arrays, and
  Reflection all depend on stable names.
- **Make obfuscation configurable by category.** All-or-nothing renaming is too
  risky for real apps.
- **Keep a stable mapping context.** Incremental builds must not randomly rename
  public/internal symbols on every dump.
- **Offer diagnostics.** A reverse lookup/debug map is valuable when a customer
  sends an error from an encoded build.
- **Treat dynamic PHP as a first-class risk.** Indirect function/method calls,
  variable variables, `class_exists()`, `function_exists()`, `defined()`, and
  literal strings that name symbols need analysis before renaming.
- **Use parser/engine-aware transforms, not regex.** Yakpro's AST visitor model
  is the right class of tool for source-level transforms.

## Upgrade Ideas For Bytecode

### 1. Add A Preflight Compatibility Scanner

Before encoding, scan PHP sources for constructs that make name rewriting or
bytecode loading risky:

- variable variables
- dynamic function calls
- dynamic class names
- dynamic property/method fetches
- `define()` / `defined()` with non-literal names
- `function_exists()` / `class_exists()` / `method_exists()` with non-literals
- callable arrays with string method names
- PDO `FETCH_OBJ` style property hydration

Output should be warnings first, not hard failures. This can run even before
any source-level obfuscation exists.

### 2. Add Symbol Policy Configuration

Create a project config file for the encoder, separate from package/licensing
configuration. Suggested first shape:

```json
{
  "exclude": ["vendor/*", "storage/*"],
  "keep": ["public/index.php"],
  "symbols": {
    "classes": "preserve",
    "methods": "preserve",
    "properties": "preserve",
    "functions": "preserve",
    "variables": "internal"
  },
  "ignore": {
    "classes": ["App\\Kernel"],
    "methods": ["__construct", "__invoke"],
    "prefixes": ["Symfony\\", "Psr\\"]
  }
}
```

Even if symbol rewriting is deferred, the config is useful immediately for
preflight warnings and reproducible dump behavior.

### 3. Store A Debug Map In The Manifest

Yakpro's `--whatis` is a good operational idea. For bytecode, keep any future
symbol/debug map outside the executable payload by default:

- development builds: manifest can include source path and optional symbol map
- production builds: symbol map should be encrypted separately or not emitted
- support builds: allow a separate encrypted diagnostics bundle

### 4. Prefer Metadata-Aware Bytecode Renaming Later

If we eventually rename functions/classes/properties, do it with Zend metadata
and Reflection parity tests, not only source rewriting. Required gates:

- Reflection snapshots still match expected public contracts.
- Composer autoload and framework route/container tests still pass.
- Literal symbol references are either rewritten or reported.
- Public API symbols can be preserved by policy.

### 5. Keep Source Obfuscation Optional

Yakpro-style source obfuscation may be useful before bytecode dumping for:

- stripping comments/formatting from any files intentionally left as PHP source
- low-risk variable/local-label renaming
- string literal escape rendering

But source obfuscation should not replace the encrypted container path.

## Suggested Implementation Order

1. Add `bytecode-scan` preflight command.
2. Add `bytecode.json` config loading for excludes, keeps, and symbol policy.
3. Extend manifest output with scanner warnings and build policy metadata.
4. Add tests for dynamic PHP hazards and framework-like callable arrays.
5. Only then prototype optional source-level transforms for local variables and
   string literals.

Initial implementation note: `php/bin/bytecode-scan` now exists and uses
PHP's tokenizer so it has no new Composer dependency. A later version can move
to `nikic/php-parser` when the scanner needs full AST awareness.

## Feature Parity Checklist

- [ ] Recursive keep/skip policy
- [ ] Timestamp/incremental build cache
- [ ] Stable build context
- [ ] Ignore names by category
- [ ] Ignore prefixes by category
- [ ] Built-in PHP symbol reservation
- [x] Dynamic-call scanner
- [x] Callable-array scanner
- [ ] Manifest diagnostics
- [ ] Optional source-minify stage
- [ ] Optional string literal source-obfuscation stage
- [ ] Optional local-variable rename stage
