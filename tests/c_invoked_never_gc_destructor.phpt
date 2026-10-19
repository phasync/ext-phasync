--TEST--
sapi(): I/O in a destructor run by the cycle collector (a GC fiber) runs inline
--EXTENSIONS--
phasync
--FILE--
<?php
require __DIR__ . '/c_invoked.inc';
class CiG { public $self; function __construct(public Closure $s) { $this->self = $this; } function __destruct() { ($this->s)(); } }
c_invoked("GC destructor", function ($sleep) { new CiG($sleep); gc_collect_cycles(); });
?>
--EXPECT--
GC destructor: A before, A after, B
