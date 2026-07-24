<?php
require __DIR__ . '/rung5_subject.php';

$class = new ReflectionClass(Rung5Service::class);
$ctor = $class->getConstructor();
$method = $class->getMethod('handle');
$props = [];

foreach ($class->getProperties() as $property) {
    $type = $property->getType();
    $props[$property->getName()] = [
        'type' => $type ? (string) $type : null,
        'readonly' => $property->isReadOnly(),
        'promoted' => $property->isPromoted(),
    ];
}
ksort($props);

$params = [];
foreach ($ctor->getParameters() as $param) {
    $type = $param->getType();
    $params[$param->getName()] = [
        'type' => $type ? (string) $type : null,
        'builtin' => $type instanceof ReflectionNamedType ? $type->isBuiltin() : null,
        'optional' => $param->isOptional(),
        'defaultAvailable' => $param->isDefaultValueAvailable(),
        'default' => $param->isDefaultValueAvailable() ? $param->getDefaultValue() : null,
        'promoted' => $param->isPromoted(),
    ];
}

$handleParam = $method->getParameters()[0];

echo json_encode([
    'class' => $class->getName(),
    'ctorParams' => $params,
    'props' => $props,
    'methodReturn' => (string) $method->getReturnType(),
    'methodDefault' => $handleParam->getDefaultValue(),
    'call' => (new Rung5Service(new Rung5Dependency()))->handle(),
], JSON_UNESCAPED_SLASHES | JSON_UNESCAPED_UNICODE), "\n";
