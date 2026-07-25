<?php

function plain_locals(int $a, int $b): int
{
    $sum = $a + $b;
    $extra = $sum * 2;
    return $extra;
}

function uses_compact(int $seed): array
{
    $local = $seed + 1;
    return compact('local');
}

echo plain_locals(2, 3), "\n";
echo uses_compact(5)['local'], "\n";
