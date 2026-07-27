<?php

declare(strict_types=1);

function bytecode_asset_fail(string $message, int $code = 500): never
{
    if (!headers_sent()) {
        http_response_code($code);
        header('Content-Type: text/plain; charset=utf-8');
    }
    echo $message, PHP_EOL;
    exit(1);
}

function bytecode_asset_u32le(string $bytes, int &$offset): int
{
    if (strlen($bytes) < $offset + 4) {
        bytecode_asset_fail('truncated asset container');
    }
    $value = unpack('V', substr($bytes, $offset, 4))[1];
    $offset += 4;
    return $value;
}

function bytecode_asset_u64le(string $bytes, int &$offset): int
{
    if (strlen($bytes) < $offset + 8) {
        bytecode_asset_fail('truncated asset container');
    }
    $parts = unpack('Vlo/Vhi', substr($bytes, $offset, 8));
    $offset += 8;
    return $parts['lo'] + ($parts['hi'] << 32);
}

function bytecode_asset_take(string $bytes, int &$offset, int $length): string
{
    if ($length < 0 || strlen($bytes) < $offset + $length) {
        bytecode_asset_fail('truncated asset container');
    }
    $value = substr($bytes, $offset, $length);
    $offset += $length;
    return $value;
}

function bytecode_asset_license_file(?string $containerPath): ?string
{
    $explicit = getenv('OPDUMP_LICENSE_FILE');
    if (is_string($explicit) && $explicit !== '') {
        return $explicit;
    }
    if ($containerPath !== null) {
        $candidate = dirname($containerPath) . DIRECTORY_SEPARATOR . 'bytecode.license.json';
        if (is_file($candidate)) {
            return $candidate;
        }
    }
    $map = getenv('OPDUMP_MAP') ?: getenv('BYTECODE_MAP') ?: '';
    if ($map !== '') {
        $candidate = dirname($map) . DIRECTORY_SEPARATOR . 'bytecode.license.json';
        if (is_file($candidate)) {
            return $candidate;
        }
    }
    return null;
}

function bytecode_asset_unwrap_license_dek(?string $containerPath): ?string
{
    $keyFile = getenv('OPDUMP_LICENSE_KEY_FILE') ?: '';
    if ($keyFile === '') {
        return null;
    }
    $licenseFile = bytecode_asset_license_file($containerPath);
    if ($licenseFile === null || !is_file($licenseFile)) {
        bytecode_asset_fail('asset license file not found');
    }
    $license = json_decode((string) file_get_contents($licenseFile), true);
    if (!is_array($license) || ($license['format'] ?? '') !== 'bytecode-license-v1') {
        bytecode_asset_fail('invalid asset license file');
    }
    $wrapped = base64_decode((string) ($license['wrapped_dek'] ?? ''), true);
    if ($wrapped === false || $wrapped === '') {
        bytecode_asset_fail('asset license is missing wrapped_dek');
    }

    $cmd = [
        'openssl', 'pkeyutl', '-decrypt',
        '-inkey', $keyFile,
        '-pkeyopt', 'rsa_padding_mode:oaep',
        '-pkeyopt', 'rsa_oaep_md:sha256',
        '-pkeyopt', 'rsa_mgf1_md:sha256',
    ];
    $descriptors = [0 => ['pipe', 'r'], 1 => ['pipe', 'w'], 2 => ['pipe', 'w']];
    $proc = proc_open($cmd, $descriptors, $pipes);
    if ($proc === false) {
        bytecode_asset_fail('failed to start openssl for asset license unwrap');
    }
    fwrite($pipes[0], $wrapped);
    fclose($pipes[0]);
    $dek = stream_get_contents($pipes[1]);
    $stderr = stream_get_contents($pipes[2]);
    fclose($pipes[1]);
    fclose($pipes[2]);
    $exit = proc_close($proc);
    if ($exit !== 0 || strlen($dek) !== 32) {
        bytecode_asset_fail('asset license unwrap failed' . ($stderr !== '' ? ': ' . trim($stderr) : ''));
    }
    return bin2hex($dek);
}

function bytecode_asset_ikm(?string $containerPath): string
{
    $vendorKeyFile = getenv('BYTECODE_VENDOR_KEY_FILE') ?: dirname(__DIR__, 2) . DIRECTORY_SEPARATOR . 'build' . DIRECTORY_SEPARATOR . 'vendor-secret.key';
    $fileKey = is_file($vendorKeyFile) ? trim((string) file_get_contents($vendorKeyFile)) : '';
    $hex = getenv('BYTECODE_DEK') ?: getenv('BYTECODE_KEY') ?: getenv('OPDUMP_KEY') ?: getenv('BYTECODE_VENDOR_KEY') ?: $fileKey;
    if ($hex === '') {
        $hex = bytecode_asset_unwrap_license_dek($containerPath) ?? '';
    }
    if (!preg_match('/\A[0-9a-fA-F]{64}\z/', $hex)) {
        bytecode_asset_fail('no valid asset key material available');
    }
    return hex2bin($hex);
}

function bytecode_asset_decrypt(string $containerPath): string
{
    static $cache = [];
    static $telemetry = ['loads' => 0, 'cache_hits' => 0, 'decrypt_failures' => 0];
    $cacheEnabled = getenv('BYTECODE_ASSET_CACHE') !== '0';
    if ($cacheEnabled && isset($cache[$containerPath])) {
        $telemetry['cache_hits']++;
        $GLOBALS['BYTECODE_ASSET_TELEMETRY'] = $telemetry;
        return $cache[$containerPath];
    }
    $telemetry['loads']++;
    $bytes = file_get_contents($containerPath);
    if ($bytes === false) {
        bytecode_asset_fail("cannot read asset container: {$containerPath}", 404);
    }
    if (substr($bytes, 0, 4) !== 'BYTC') {
        bytecode_asset_fail("not a BYTC asset container: {$containerPath}");
    }

    $offset = 4;
    $version = bytecode_asset_u32le($bytes, $offset);
    if ($version !== 2) {
        bytecode_asset_fail("unsupported asset container version {$version}");
    }
    bytecode_asset_u32le($bytes, $offset); // PHP_VERSION_ID is retained for BYTC header compatibility.
    $backendId = bytecode_asset_take($bytes, $offset, bytecode_asset_u32le($bytes, $offset));
    $backendFormat = bytecode_asset_take($bytes, $offset, bytecode_asset_u32le($bytes, $offset));
    if ($backendId !== 'bytecode-asset' || $backendFormat !== 'RAW1') {
        bytecode_asset_fail("unsupported asset backend {$backendId}/{$backendFormat}");
    }
    $cipherId = bytecode_asset_u32le($bytes, $offset);
    $keyId = bytecode_asset_take($bytes, $offset, bytecode_asset_u32le($bytes, $offset));
    if ($cipherId !== 1) {
        bytecode_asset_fail("unsupported asset cipher_id {$cipherId}");
    }
    $aad = substr($bytes, 0, $offset);
    $nonce = bytecode_asset_take($bytes, $offset, bytecode_asset_u32le($bytes, $offset));
    $tag = bytecode_asset_take($bytes, $offset, bytecode_asset_u32le($bytes, $offset));
    $ciphertext = bytecode_asset_take($bytes, $offset, bytecode_asset_u64le($bytes, $offset));
    if ($offset !== strlen($bytes)) {
        bytecode_asset_fail('asset container has trailing bytes');
    }

    $key = hash_hkdf('sha256', bytecode_asset_ikm($containerPath), 32, 'bytecode-v2', $keyId);
    $plain = openssl_decrypt($ciphertext, 'aes-256-gcm', $key, OPENSSL_RAW_DATA, $nonce, $tag, $aad);
    if ($plain === false) {
        $telemetry['decrypt_failures']++;
        $GLOBALS['BYTECODE_ASSET_TELEMETRY'] = $telemetry;
        bytecode_asset_fail('asset authentication/decryption failed');
    }
    if ($cacheEnabled) {
        $cache[$containerPath] = $plain;
    }
    $GLOBALS['BYTECODE_ASSET_TELEMETRY'] = $telemetry;
    return $plain;
}

function bytecode_asset_telemetry(): array
{
    return $GLOBALS['BYTECODE_ASSET_TELEMETRY'] ?? ['loads' => 0, 'cache_hits' => 0, 'decrypt_failures' => 0];
}

function bytecode_asset_mime(string $path): string
{
    if (str_ends_with(strtolower(str_replace('\\', '/', $path)), '.blade.php')) {
        return 'text/x-blade; charset=utf-8';
    }
    return match (strtolower(pathinfo($path, PATHINFO_EXTENSION))) {
        'css' => 'text/css; charset=utf-8',
        'js', 'mjs' => 'text/javascript; charset=utf-8',
        'html', 'htm', 'twig' => 'text/html; charset=utf-8',
        'json' => 'application/json; charset=utf-8',
        'svg' => 'image/svg+xml',
        default => 'application/octet-stream',
    };
}

function bytecode_asset_serve(string $containerPath, ?string $mime = null): never
{
    $plain = bytecode_asset_decrypt($containerPath);
    if (!headers_sent()) {
        header('Content-Type: ' . ($mime ?? bytecode_asset_mime($containerPath)));
        header('Content-Length: ' . strlen($plain));
        header('Cache-Control: public, max-age=31536000, immutable');
    }
    echo $plain;
    exit(0);
}
