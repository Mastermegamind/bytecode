<?php

class ScanSubject
{
    public function route(): void
    {
    }
}

$name = 'dynamic_function';
$$name = 1;
$name();

$class = ScanSubject::class;
new $class();

$object = new ScanSubject();
$method = 'route';
$object->$method();
$object->{$method}();

define($name, 1);
defined($name);
function_exists($name);
class_exists($class);
method_exists($object, $method);
property_exists($object, $method);
constant($name);

call_user_func([$object, $method]);
call_user_func_array([$object, $method], []);
is_callable([$object, $method]);

$callable = [ScanSubject::class, 'route'];
$mode = PDO::FETCH_OBJ;
