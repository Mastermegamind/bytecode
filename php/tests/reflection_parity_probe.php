<?php
require __DIR__ . '/reflection_parity_subject.php';

function reflection_parity_type(?ReflectionType $type): mixed
{
    if ($type === null) {
        return null;
    }

    if ($type instanceof ReflectionNamedType) {
        return [
            'kind' => 'named',
            'name' => $type->getName(),
            'allowsNull' => $type->allowsNull(),
            'builtin' => $type->isBuiltin(),
        ];
    }

    $children = [];
    if ($type instanceof ReflectionUnionType || $type instanceof ReflectionIntersectionType) {
        foreach ($type->getTypes() as $child) {
            $children[] = reflection_parity_type($child);
        }
    }

    return [
        'kind' => $type instanceof ReflectionIntersectionType ? 'intersection' : 'union',
        'name' => (string) $type,
        'allowsNull' => $type->allowsNull(),
        'types' => $children,
    ];
}

function reflection_parity_parameter(ReflectionParameter $parameter): array
{
    return [
        'name' => $parameter->getName(),
        'type' => reflection_parity_type($parameter->getType()),
        'optional' => $parameter->isOptional(),
        'allowsNull' => $parameter->allowsNull(),
        'defaultAvailable' => $parameter->isDefaultValueAvailable(),
        'default' => $parameter->isDefaultValueAvailable()
            ? $parameter->getDefaultValue()
            : null,
        'promoted' => $parameter->isPromoted(),
    ];
}

function reflection_parity_method(ReflectionMethod $method): array
{
    return [
        'name' => $method->getName(),
        'visibility' => implode(' ', Reflection::getModifierNames($method->getModifiers())),
        'returnType' => reflection_parity_type($method->getReturnType()),
        'parameters' => array_map(
            reflection_parity_parameter(...),
            $method->getParameters(),
        ),
    ];
}

function reflection_parity_property(ReflectionProperty $property): array
{
    return [
        'name' => $property->getName(),
        'visibility' => implode(' ', Reflection::getModifierNames($property->getModifiers())),
        'type' => reflection_parity_type($property->getType()),
        'readonly' => $property->isReadOnly(),
        'promoted' => $property->isPromoted(),
        'hasDefault' => $property->hasDefaultValue(),
        'default' => $property->hasDefaultValue() ? $property->getDefaultValue() : null,
    ];
}

$class = new ReflectionClass(ReflectionParityService::class);
$methods = [];
foreach ($class->getMethods() as $method) {
    if ($method->getDeclaringClass()->getName() === $class->getName()) {
        $methods[$method->getName()] = reflection_parity_method($method);
    }
}
ksort($methods);

$properties = [];
foreach ($class->getProperties() as $property) {
    $properties[$property->getName()] = reflection_parity_property($property);
}
ksort($properties);

echo json_encode([
    'class' => $class->getName(),
    'constants' => $class->getConstants(),
    'constructor' => reflection_parity_method($class->getConstructor()),
    'methods' => $methods,
    'properties' => $properties,
    'call' => (new ReflectionParityService(new ReflectionParityDependency()))->handle(),
], JSON_UNESCAPED_SLASHES | JSON_UNESCAPED_UNICODE | JSON_THROW_ON_ERROR), "\n";
