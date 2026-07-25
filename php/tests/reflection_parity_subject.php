<?php
class ReflectionParityDependency
{
    public function write(string $message): void
    {
    }

    public function now(): DateTimeImmutable
    {
        return new DateTimeImmutable('@0');
    }
}

class ReflectionParityService
{
    public function __construct(
        public readonly ReflectionParityDependency $dependency,
        public readonly ?string $label = 'phase3',
        int $limit = 25,
    ) {
    }

    public function handle(string $prefix = 'ok'): string
    {
        return $prefix . ':' . $this->label;
    }
}
