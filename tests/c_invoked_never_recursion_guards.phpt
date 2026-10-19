--TEST--
sapi(): I/O in jsonSerialize() and __debugInfo() runs inline (recursion guards are process-wide state)
--EXTENSIONS--
phasync
--FILE--
<?php
require __DIR__ . '/c_invoked.inc';
class CiJ implements JsonSerializable { function __construct(public Closure $s) {} function jsonSerialize(): mixed { ($this->s)(); return 1; } }
class CiDbg { function __construct(public Closure $s) {} function __debugInfo() { ($this->s)(); return []; } }
c_invoked("jsonSerialize", function ($sleep) { json_encode(new CiJ($sleep)); });
c_invoked("__debugInfo var_dump", function ($sleep) { ob_start(); var_dump(new CiDbg($sleep)); ob_end_clean(); });
c_invoked("__debugInfo print_r", function ($sleep) { print_r(new CiDbg($sleep), true); });
?>
--EXPECT--
jsonSerialize: A before, A after, B
__debugInfo var_dump: A before, A after, B
__debugInfo print_r: A before, A after, B
