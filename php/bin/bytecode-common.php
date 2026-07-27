<?php

declare(strict_types=1);

function bytecode_project_version(): string
{
    $path = dirname(__DIR__, 2) . DIRECTORY_SEPARATOR . 'VERSION';
    if (!is_file($path)) {
        return 'v0.0.0-dev';
    }
    $version = trim((string) file_get_contents($path));
    return $version !== '' ? $version : 'v0.0.0-dev';
}
