<?php
function guarded(bool $throw = true): string
{
    try {
        if ($throw) {
            throw new RuntimeException('phase0');
        }
        return 'no-error';
    } catch (RuntimeException $e) {
        return 'caught:' . $e->getMessage();
    } finally {
        $GLOBALS['rung4_finally'] = 'finally';
    }
}

echo guarded(), ':', $GLOBALS['rung4_finally'], "\n";
