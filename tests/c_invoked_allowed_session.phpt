--TEST--
sapi(): I/O in a session save handler suspends
--EXTENSIONS--
phasync
session
--INI--
session.use_cookies=0
session.cache_limiter=
session.save_path=
--FILE--
<?php
require __DIR__ . '/c_invoked.inc';
class CiSess implements SessionHandlerInterface { public static $s;
  function open($p, $n): bool { return true; } function close(): bool { return true; }
  function read($id): string|false { (self::$s)(); return ''; } function write($id, $d): bool { return true; }
  function destroy($id): bool { return true; } function gc($m): int|false { return 0; }
  function create_sid(): string { return 'abc'; } function validateId($id): bool { return true; } }
c_invoked("session read handler", function ($sleep) { CiSess::$s = $sleep; session_set_save_handler(new CiSess, false); session_id('abc'); session_start(); session_write_close(); });
?>
--EXPECT--
session read handler: A before, B, A after
