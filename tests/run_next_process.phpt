--TEST--
run_next(): cwd, umask, alarm and the signal mask are restored; handlers, environment, locale and ini are reset
--EXTENSIONS--
phasync
pcntl
--FILE--
<?php
require __DIR__ . '/run_next.inc';
$d = rn_dir();
mkdir("$d/sub");
file_put_contents("$d/main.php", <<<'PHP'
<?php
$state = __DIR__ . '/state';
if (!file_exists($state)) {
    file_put_contents($state, json_encode([umask(), setlocale(LC_ALL, '0')]));
    phasync\ext\run_next(__FILE__);
    chdir(__DIR__ . '/sub');
    umask(0077);
    pcntl_alarm(30);
    pcntl_sigprocmask(SIG_BLOCK, [SIGUSR1]);
    pcntl_signal(SIGUSR2, fn() => print("leaked handler\n"));
    putenv('RN_LEAK=1');
    $_ENV['RN_LEAK'] = 1;
    setlocale(LC_ALL, 'C.UTF-8');
    ini_set('precision', '5');
    ini_set('error_log', __DIR__ . '/leak.log');
    stream_context_set_default(['http' => ['method' => 'POST']]);
    echo "first\n";
    return;
}
echo "second\n";
echo 'cwd: ', getcwd() === __DIR__ ? 'start' : getcwd(), "\n";
[$umask, $locale] = json_decode(file_get_contents($state));
echo 'umask: ', umask() === $umask ? 'restored' : 'leaked', "\n";
echo 'alarm: ', pcntl_alarm(0), "\n";
pcntl_sigprocmask(SIG_BLOCK, [SIGUSR2], $mask);
echo 'blocked: ', json_encode($mask), "\n";
pcntl_sigprocmask(SIG_UNBLOCK, [SIGUSR2]);
echo 'putenv: ', var_export(getenv('RN_LEAK'), true), ', $_ENV: ', var_export(isset($_ENV['RN_LEAK']), true), "\n";
echo 'locale: ', setlocale(LC_ALL, '0') === $locale ? 'restored' : 'leaked', "\n";
echo 'ini: ', ini_get('precision'), ' ', var_export(ini_get('error_log'), true), "\n";
echo 'context: ', json_encode(stream_context_get_options(stream_context_get_default())), "\n";
// the leaked mask would keep SIGUSR1 from arriving; a leaked handler would print
pcntl_async_signals(true);
pcntl_signal(SIGUSR1, fn() => print("SIGUSR1 delivered\n"));
posix_kill(getmypid(), SIGUSR1);
echo "sending SIGUSR2 with no handler\n";
posix_kill(getmypid(), SIGUSR2);
usleep(100000);
echo "survived\n";
PHP);
[$out, $status] = rn_exec($d, "$d/main.php", [], null, ['load' => ['posix'], 'ini' => ['precision' => '14']]);
echo $out, "survived: ", str_contains($out, 'survived') ? 'yes' : 'no', "\n";
rn_clean($d);
?>
--EXPECT--
first
second
cwd: start
umask: restored
alarm: 0
blocked: []
putenv: false, $_ENV: false
locale: restored
ini: 14 ''
context: []
SIGUSR1 delivered
sending SIGUSR2 with no handler
survived: no
