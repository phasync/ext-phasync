--TEST--
sapi(): I/O in methods the engine calls (Iterator, IteratorAggregate, ArrayAccess, __toString, __clone, __call, Countable) suspends
--EXTENSIONS--
phasync
--FILE--
<?php
require __DIR__ . '/c_invoked.inc';
class CiE implements Iterator, ArrayAccess, Countable {
    private int $n = 0;
    function __construct(public Closure $s) {}
    function current(): mixed { ($this->s)(); return 1; } function key(): mixed { return 0; } function next(): void {}
    function rewind(): void { $this->n = 0; } function valid(): bool { return $this->n++ < 1; }
    function offsetGet(mixed $o): mixed { ($this->s)(); return 1; } function offsetSet(mixed $o, mixed $v): void { ($this->s)(); }
    function offsetExists(mixed $o): bool { ($this->s)(); return true; } function offsetUnset(mixed $o): void { ($this->s)(); }
    function count(): int { ($this->s)(); return 1; }
    function __toString(): string { ($this->s)(); return 'x'; }
    function __clone() { ($this->s)(); }
    function __call($n, $a) { ($this->s)(); }
}
class CiA implements IteratorAggregate { function __construct(public Closure $s) {} function getIterator(): Iterator { ($this->s)(); return new ArrayIterator([]); } }
c_invoked("foreach Iterator", function ($sleep) { foreach (new CiE($sleep) as $v); });
c_invoked("getIterator", function ($sleep) { foreach (new CiA($sleep) as $v); });
c_invoked("offsetGet", function ($sleep) { $o = new CiE($sleep); $o['a']; });
c_invoked("offsetSet", function ($sleep) { $o = new CiE($sleep); $o['a'] = 1; });
c_invoked("offsetExists", function ($sleep) { $o = new CiE($sleep); isset($o['a']); });
c_invoked("offsetUnset", function ($sleep) { $o = new CiE($sleep); unset($o['a']); });
c_invoked("count", function ($sleep) { count(new CiE($sleep)); });
c_invoked("__toString echo", function ($sleep) { $o = new CiE($sleep); ob_start(); echo $o; ob_end_clean(); });
c_invoked("__toString concat", function ($sleep) { $o = new CiE($sleep); $s = 'a' . $o; });
c_invoked("__toString in C function", function ($sleep) { strlen((string) new CiE($sleep)); str_contains(new CiE($sleep), 'x'); });
c_invoked("__clone", function ($sleep) { $o = new CiE($sleep); $c = clone $o; });
c_invoked("__call", function ($sleep) { (new CiE($sleep))->undefined(); });
?>
--EXPECT--
foreach Iterator: A before, B, A after
getIterator: A before, B, A after
offsetGet: A before, B, A after
offsetSet: A before, B, A after
offsetExists: A before, B, A after
offsetUnset: A before, B, A after
count: A before, B, A after
__toString echo: A before, B, A after
__toString concat: A before, B, A after
__toString in C function: A before, B, A after
__clone: A before, B, A after
__call: A before, B, A after
