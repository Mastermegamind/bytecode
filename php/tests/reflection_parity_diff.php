<?php
if ($argc !== 3) {
    fwrite(STDERR, "usage: php reflection_parity_diff.php expected.json actual.json\n");
    exit(2);
}

$expected = json_decode(file_get_contents($argv[1]), true, 512, JSON_THROW_ON_ERROR);
$actual = json_decode(file_get_contents($argv[2]), true, 512, JSON_THROW_ON_ERROR);
$diffs = [];

$walk = function (mixed $left, mixed $right, string $path) use (&$walk, &$diffs): void {
    if (is_array($left) && is_array($right)) {
        $keys = array_unique(array_merge(array_keys($left), array_keys($right)));
        sort($keys);
        foreach ($keys as $key) {
            $childPath = $path . '/' . $key;
            if (!array_key_exists($key, $left)) {
                $diffs[] = "$childPath missing from expected";
                continue;
            }
            if (!array_key_exists($key, $right)) {
                $diffs[] = "$childPath missing from actual";
                continue;
            }
            $walk($left[$key], $right[$key], $childPath);
        }
        return;
    }

    if ($left !== $right) {
        $diffs[] = sprintf(
            '%s expected %s, got %s',
            $path,
            json_encode($left, JSON_UNESCAPED_SLASHES | JSON_UNESCAPED_UNICODE),
            json_encode($right, JSON_UNESCAPED_SLASHES | JSON_UNESCAPED_UNICODE),
        );
    }
};

$walk($expected, $actual, '');

if ($diffs === []) {
    exit(0);
}

foreach ($diffs as $diff) {
    fwrite(STDERR, $diff . "\n");
}
exit(1);
