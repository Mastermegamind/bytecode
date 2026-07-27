<?php

declare(strict_types=1);

require_once __DIR__ . '/bytecode-assets.php';

final class BytecodeTwigLoader implements Twig\Loader\LoaderInterface, Twig\Loader\SourceContextLoaderInterface
{
    public function __construct(private string $root)
    {
        $this->root = rtrim($this->root, '/\\');
    }

    public function getSourceContext(string $name): Twig\Source
    {
        $path = $this->path($name);
        if (!is_file($path)) {
            throw new Twig\Error\LoaderError("Template not found: {$name}");
        }
        return new Twig\Source(bytecode_asset_decrypt($path), $name, $path);
    }

    public function getCacheKey(string $name): string
    {
        return $this->path($name) . ':' . filemtime($this->path($name));
    }

    public function isFresh(string $name, int $time): bool
    {
        return filemtime($this->path($name)) <= $time;
    }

    public function exists(string $name): bool
    {
        return is_file($this->path($name));
    }

    private function path(string $name): string
    {
        $clean = ltrim(str_replace('\\', '/', $name), '/');
        if ($clean === '' || str_contains($clean, '..')) {
            throw new Twig\Error\LoaderError("Unsafe template path: {$name}");
        }
        return $this->root . DIRECTORY_SEPARATOR . str_replace('/', DIRECTORY_SEPARATOR, $clean);
    }
}
