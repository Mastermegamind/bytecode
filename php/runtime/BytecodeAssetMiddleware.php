<?php

declare(strict_types=1);

require_once __DIR__ . '/bytecode-assets.php';

final class BytecodeAssetMiddleware
{
    public function __construct(
        private string $publicRoot,
        private string $urlPrefix = '/',
    ) {
        $this->publicRoot = rtrim($this->publicRoot, '/\\');
        $this->urlPrefix = '/' . trim($this->urlPrefix, '/');
        if ($this->urlPrefix === '/') {
            $this->urlPrefix = '';
        }
    }

    public function __invoke(object $request, object $handler): mixed
    {
        $path = (string) $request->getUri()->getPath();
        if ($this->urlPrefix !== '' && !str_starts_with($path, $this->urlPrefix . '/')) {
            return $handler->handle($request);
        }

        $rel = ltrim($this->urlPrefix === '' ? $path : substr($path, strlen($this->urlPrefix)), '/');
        if ($rel === '' || str_contains($rel, '..')) {
            return $handler->handle($request);
        }

        $container = $this->publicRoot . DIRECTORY_SEPARATOR . str_replace('/', DIRECTORY_SEPARATOR, $rel);
        if (!is_file($container) || substr((string) file_get_contents($container, false, null, 0, 4), 0, 4) !== 'BYTC') {
            return $handler->handle($request);
        }

        $body = bytecode_asset_decrypt($container);
        if (class_exists('Slim\Psr7\Response')) {
            $response = new Slim\Psr7\Response(200);
        } elseif (class_exists('Nyholm\Psr7\Response')) {
            $response = new Nyholm\Psr7\Response(200);
        } else {
            bytecode_asset_serve($container);
        }
        $response->getBody()->write($body);
        return $response
            ->withHeader('Content-Type', bytecode_asset_mime($container))
            ->withHeader('Cache-Control', 'public, max-age=31536000, immutable');
    }
}
