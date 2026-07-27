<?php

declare(strict_types=1);

require_once __DIR__ . '/bytecode-assets.php';

final class BytecodeBladeCompiler extends Illuminate\View\Compilers\BladeCompiler
{
    public function compile($path = null): void
    {
        if ($path === null) {
            return;
        }

        $source = $this->isBytecodeContainer($path)
            ? bytecode_asset_decrypt($path)
            : $this->files->get($path);

        $compiled = $this->compileString($source);
        if (!is_null($this->cachePath)) {
            $this->files->put($this->getCompiledPath($path), $compiled);
        }
    }

    public function isExpired($path): bool
    {
        if (!$this->isBytecodeContainer($path)) {
            return parent::isExpired($path);
        }

        $compiled = $this->getCompiledPath($path);
        if (!$this->files->exists($compiled)) {
            return true;
        }
        return $this->files->lastModified($path) >= $this->files->lastModified($compiled);
    }

    private function isBytecodeContainer(string $path): bool
    {
        if (!$this->files->exists($path)) {
            return false;
        }
        $handle = @fopen($path, 'rb');
        if (!$handle) {
            return false;
        }
        $magic = fread($handle, 4);
        fclose($handle);
        return $magic === 'BYTC';
    }
}
