<?php
class Rung5Dependency
{
}

class Rung5Service
{
    public function __construct(
        public readonly Rung5Dependency $dependency,
        public readonly ?string $label = 'exam',
        int $limit = 25,
    ) {
    }

    public function handle(string $prefix = 'ok'): string
    {
        return $prefix . ':' . $this->label;
    }
}
