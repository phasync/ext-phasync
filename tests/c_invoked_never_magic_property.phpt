--TEST--
sapi(): I/O in __get/__set/__isset/__unset runs inline (the per-property guard is not a lock)
--EXTENSIONS--
phasync
--FILE--
<?php
require __DIR__ . '/c_invoked.inc';
class CiM { function __construct(public Closure $s) {}
  function __get($n) { ($this->s)(); return 1; } function __set($n, $v) { ($this->s)(); }
  function __isset($n) { ($this->s)(); return true; } function __unset($n) { ($this->s)(); } }
c_invoked("__get", function ($sleep) { $o = new CiM($sleep); $o->x; });
c_invoked("__set", function ($sleep) { $o = new CiM($sleep); $o->x = 1; });
c_invoked("__isset", function ($sleep) { $o = new CiM($sleep); isset($o->x); });
c_invoked("__unset", function ($sleep) { $o = new CiM($sleep); unset($o->x); });
?>
--EXPECT--
__get: A before, A after, B
__set: A before, A after, B
__isset: A before, A after, B
__unset: A before, A after, B
