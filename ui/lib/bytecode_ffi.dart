// Dart FFI bindings for libbytecode_tools (see rust/include/bytecode_tools.h).
//
// Lets the desktop app run keygen / pack / single-container verify in-process
// via the Rust shared library, with no subprocess and no PHP runtime. Loading
// is best-effort: if the library is not built, `BytecodeFfi.open` returns null
// and callers should fall back to the CLI/PHP path.

import 'dart:ffi';
import 'dart:io';

import 'package:ffi/ffi.dart';

typedef _KeygenNative = Pointer<Utf8> Function();
typedef _Keygen = Pointer<Utf8> Function();

typedef _FreeNative = Void Function(Pointer<Utf8>);
typedef _Free = void Function(Pointer<Utf8>);

typedef _PackNative = Int32 Function(Pointer<Utf8>, Pointer<Utf8>, Uint32);
typedef _Pack = int Function(Pointer<Utf8>, Pointer<Utf8>, int);

typedef _VerifyNative = Int32 Function(
    Pointer<Utf8>, Int32, Uint32, Pointer<Pointer<Utf8>>);
typedef _Verify = int Function(
    Pointer<Utf8>, int, int, Pointer<Pointer<Utf8>>);

typedef _DetectNative = Int64 Function();
typedef _Detect = int Function();

typedef _LastErrNative = Pointer<Utf8> Function();
typedef _LastErr = Pointer<Utf8> Function();

class BytecodeFfiException implements Exception {
  final String message;
  BytecodeFfiException(this.message);
  @override
  String toString() => 'BytecodeFfiException: $message';
}

class BytecodeFfi {
  final DynamicLibrary _lib;
  final String libraryPath;

  late final _Keygen _keygen;
  late final _Free _free;
  late final _Pack _pack;
  late final _Verify _verify;
  late final _Detect _detect;
  late final _LastErr _lastErr;

  BytecodeFfi._(this._lib, this.libraryPath) {
    _keygen = _lib.lookupFunction<_KeygenNative, _Keygen>('bt_keygen_hex');
    _free = _lib.lookupFunction<_FreeNative, _Free>('bt_string_free');
    _pack = _lib.lookupFunction<_PackNative, _Pack>('bt_pack');
    _verify = _lib.lookupFunction<_VerifyNative, _Verify>('bt_verify_one');
    _detect =
        _lib.lookupFunction<_DetectNative, _Detect>('bt_detect_php_version_id');
    _lastErr = _lib.lookupFunction<_LastErrNative, _LastErr>('bt_last_error');
  }

  static String _libName() {
    if (Platform.isWindows) return 'bytecode_tools.dll';
    if (Platform.isMacOS) return 'libbytecode_tools.dylib';
    return 'libbytecode_tools.so';
  }

  static BytecodeFfi? _tryOpen(String path) {
    try {
      return BytecodeFfi._(DynamicLibrary.open(path), path);
    } catch (_) {
      return null;
    }
  }

  /// Locate and load the library under `<root>/rust/target/{release,debug}`,
  /// then fall back to the OS loader by bare name. Returns null if unavailable.
  static BytecodeFfi? open(String root) {
    final name = _libName();
    final candidates = <String>[
      '$root/rust/target/release/$name',
      '$root/rust/target/debug/$name',
    ];
    for (final path in candidates) {
      if (File(path).existsSync()) {
        final lib = _tryOpen(path);
        if (lib != null) return lib;
      }
    }
    // Last resort: let the dynamic linker resolve it (LD_LIBRARY_PATH, etc.).
    return _tryOpen(name);
  }

  String _takeError() {
    final p = _lastErr();
    if (p == nullptr) return 'unknown error';
    final s = p.toDartString();
    return s.isEmpty ? 'unknown error' : s;
  }

  /// Generate a fresh 32-byte key as 64 hex characters.
  String keygenHex() {
    final p = _keygen();
    if (p == nullptr) throw BytecodeFfiException('keygen allocation failed');
    final s = p.toDartString();
    _free(p);
    return s;
  }

  /// Wrap a raw OPD2 blob into a BYTC v2 container (throws on failure).
  void pack(String rawPath, String outPath, int phpVersionId) {
    final raw = rawPath.toNativeUtf8();
    final out = outPath.toNativeUtf8();
    try {
      final rc = _pack(raw, out, phpVersionId);
      if (rc != 0) throw BytecodeFfiException(_takeError());
    } finally {
      malloc.free(raw);
      malloc.free(out);
    }
  }

  /// Verify one `.bytc` container, returning the JSON summary string. When
  /// [decryptTest] is true the container is also AES-GCM-decrypted.
  String verifyOne(String path, bool decryptTest, int wantPhpVersionId) {
    final p = path.toNativeUtf8();
    final outPtr = calloc<Pointer<Utf8>>();
    try {
      final rc = _verify(p, decryptTest ? 1 : 0, wantPhpVersionId, outPtr);
      if (rc != 0) throw BytecodeFfiException(_takeError());
      final jp = outPtr.value;
      final json = jp == nullptr ? '{}' : jp.toDartString();
      if (jp != nullptr) _free(jp);
      return json;
    } finally {
      malloc.free(p);
      calloc.free(outPtr);
    }
  }

  /// Best-effort local PHP_VERSION_ID (BYTECODE_PHP_VERSION_ID or a `php`
  /// probe); -1 if it cannot be determined.
  int detectPhpVersionId() => _detect();
}
