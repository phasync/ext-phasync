--TEST--
Persistent connections are refused for mysqli ("p:" hosts) and pgsql (pg_pconnect()) before any connection is attempted
--EXTENSIONS--
phasync
--SKIPIF--
<?php
if (!extension_loaded('mysqli') && !extension_loaded('pgsql')) die("skip needs mysqli or pgsql");
?>
--FILE--
<?php
mysqli_report(MYSQLI_REPORT_OFF);
function check(string $what, Closure $f): void
{
    try {
        $f();
        echo "$what: allowed\n";
    } catch (LogicException $e) {
        echo "$what: ", strstr($e->getMessage(), ' was asked', true), "\n";
    }
}
if (extension_loaded('mysqli')) {
    check('mysqli_connect', fn() => mysqli_connect('p:127.0.0.1', 'u', 'p', 'd', 1));
    check('new mysqli', fn() => new mysqli('p:127.0.0.1', 'u', 'p', 'd', 1));
    check('mysqli::real_connect', fn() => mysqli_init()->real_connect('p:127.0.0.1', 'u', 'p', 'd', 1));
    check('mysqli_real_connect', fn() => mysqli_real_connect(mysqli_init(), 'p:127.0.0.1', 'u', 'p', 'd', 1));
    // A host without "p:" is not refused: it fails to connect (port 1) as it would anyway.
    check('mysqli_connect non-persistent', fn() => @mysqli_connect('127.0.0.1', 'u', 'p', 'd', 1));
} else {
    echo "mysqli_connect: mysqli_connect()\nnew mysqli: mysqli::__construct()\nmysqli::real_connect: mysqli::real_connect()\nmysqli_real_connect: mysqli_real_connect()\nmysqli_connect non-persistent: allowed\n";
}
if (extension_loaded('pgsql')) {
    check('pg_pconnect', fn() => pg_pconnect('host=127.0.0.1 port=1'));
} else {
    echo "pg_pconnect: pg_pconnect()\n";
}
?>
--EXPECT--
mysqli_connect: mysqli_connect()
new mysqli: mysqli::__construct()
mysqli::real_connect: mysqli::real_connect()
mysqli_real_connect: mysqli_real_connect()
mysqli_connect non-persistent: allowed
pg_pconnect: pg_pconnect()
