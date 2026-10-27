--TEST--
sapi(): the request starts as PHP's own do: max_input_vars, post_max_size and multipart errors behave and report as at request startup; cookies are parsed by PHP (#10)
--EXTENSIONS--
phasync
--INI--
max_input_vars=3
post_max_size=64
--FILE--
<?php
require __DIR__ . '/sink.inc';
use function phasync\ext\sapi;

function req(array $info, string $body = '', ?Closure $code = null): void
{
    $s = new Sink;
    $s->body = $body;
    $s->info = $info + ['content_length' => strlen($body)];
    sapi($code ?? function () {
        echo json_encode([$_GET, $_POST, $_COOKIE, array_keys($_FILES)]), "\n";
    }, $s);
    echo trim($s->out), "\n--\n";
}
$form = ['method' => 'POST', 'content_type' => 'application/x-www-form-urlencoded'];

req(['method' => 'GET', 'query_string' => 'a=1&b=2&c=3&d=4']);
req($form, str_repeat('x', 100));
req(['method' => 'POST', 'content_type' => 'multipart/form-data'], 'whatever');
// as PHP parses them: the first of two cookies of one name wins, a '+' stays
req(['method' => 'GET', 'cookie' => 'k=first; k=second; sp=a+b%20c']);
?>
--EXPECTF--
Warning: PHP Request Startup: Input variables exceeded 3. To increase the limit change max_input_vars in php.ini. in Unknown on line 0
[{"a":"1","b":"2","c":"3"},[],[],[]]
--
Warning: PHP Request Startup: POST Content-Length of 100 bytes exceeds the limit of 64 bytes in Unknown on line 0
[[],[],[],[]]
--
Warning: %SMissing boundary in multipart/form-data POST data in Unknown on line 0
[[],[],[],[]]
--
[[],[],{"k":"first","sp":"a+b c"},[]]
--
