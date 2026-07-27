<?php

declare(strict_types=1);

require_once __DIR__ . '/bytecode-assets.php';

final class BytecodeConfig
{
    public static function decryptEnv(string $containerPath): array
    {
        $plain = bytecode_asset_decrypt($containerPath);
        $values = [];
        foreach (preg_split('/\r?\n/', $plain) ?: [] as $line) {
            $line = trim($line);
            if ($line === '' || str_starts_with($line, '#') || !str_contains($line, '=')) {
                continue;
            }
            [$key, $value] = explode('=', $line, 2);
            $values[trim($key)] = trim($value, " \t\n\r\0\x0B\"'");
        }
        return $values;
    }

    public static function loadEnv(string $containerPath, bool $overwrite = false): array
    {
        $values = self::decryptEnv($containerPath);
        foreach ($values as $key => $value) {
            if ($overwrite || getenv($key) === false) {
                putenv("{$key}={$value}");
                $_ENV[$key] = $value;
                $_SERVER[$key] = $value;
            }
        }
        return $values;
    }
}
