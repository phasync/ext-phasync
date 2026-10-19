--TEST--
sapi(): I/O in serialization callbacks (__serialize, __sleep, __wakeup, __unserialize, Serializable) runs inline
--EXTENSIONS--
phasync
--FILE--
<?php
error_reporting(E_ALL & ~E_DEPRECATED);
require __DIR__ . '/c_invoked.inc';
class CiSer { public static $s; public $a = 1; function __serialize(): array { (self::$s)(); return []; } function __unserialize(array $d): void { (self::$s)(); } }
class CiSleep { public static $s; public $a = 1; function __sleep() { (self::$s)(); return ['a']; } function __wakeup() { (self::$s)(); } }
class CiOld implements Serializable { public static $s; function serialize() { (self::$s)(); return 'x'; } function unserialize($d) { (self::$s)(); } }
c_invoked("__serialize", function ($sleep) { CiSer::$s = $sleep; serialize(new CiSer); });
c_invoked("__unserialize", function ($sleep) { CiSer::$s = $sleep; unserialize('O:5:"CiSer":0:{}'); });
c_invoked("__sleep", function ($sleep) { CiSleep::$s = $sleep; serialize(new CiSleep); });
c_invoked("__wakeup", function ($sleep) { CiSleep::$s = $sleep; unserialize('O:7:"CiSleep":1:{s:1:"a";i:1;}'); });
c_invoked("Serializable::serialize", function ($sleep) { CiOld::$s = $sleep; serialize(new CiOld); });
c_invoked("Serializable::unserialize", function ($sleep) { CiOld::$s = $sleep; unserialize('C:5:"CiOld":1:{x}'); });
?>
--EXPECT--
__serialize: A before, A after, B
__unserialize: A before, A after, B
__sleep: A before, A after, B
__wakeup: A before, A after, B
Serializable::serialize: A before, A after, B
Serializable::unserialize: A before, A after, B
