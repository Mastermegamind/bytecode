<?php

class Rung3Greeter
{
    public string $name;

    public function __construct(string $name = "world")
    {
        $this->name = $name;
    }

    public function message(string $prefix = "hello")
    {
        return $prefix . " " . $this->name;
    }
}

$greeter = new Rung3Greeter();
echo $greeter->message();
