--TEST--
memo: phasync.memo_memory bounds the entries, counted in the bytes they take; least recently used ones go, and within one request (whose values may still be in use) the evicted wait in a graveyard of at most phasync.memo_memory, after which nothing new is stored until the request ends
--EXTENSIONS--
phasync
json
--INI--
phasync.memo_memory=1M
--FILE--
<?php
$stats = static fn () => phasync\ext\memo_stats();
$hot = json_encode(['hot' => range(1, 50)]);
$kept = [];
for ($i = 0; $i < 3000; $i++) {
    $text = json_encode(['n' => $i, 'items' => range(1, 40)]);   // ~170 bytes of text, ~1.5 KB as an entry
    json_decode($text, true);
    $kept[] = json_decode($text, true);   // the second time: stored; and still in use
    json_decode($hot, true);              // used all along: stays
    $s = $stats();
    if ($s['bytes'] > 1024 * 1024 || $s['graveyard_bytes'] > 1024 * 1024) {
        echo "over the cap at $i: ", $s['bytes'], ' ', $s['graveyard_bytes'], "\n";
        break;
    }
}
$s = $stats();
echo 'evicted: ', $s['evicted'] > 500 ? 'many' : $s['evicted'], ', refused once the graveyard was full: ', $s['refused'] > 500 ? 'many' : $s['refused'], "\n";
$h = $s['json_hits'];
json_decode($hot, true);
echo 'the hot entry stayed: ', $stats()['json_hits'] - $h === 1 ? 'yes' : 'no', "\n";
$ok = true;
foreach ($kept as $i => $v) { $ok = $ok && $v['n'] === $i && count($v['items']) === 40 && $v['items'][39] === 40; }
echo 'evicted values still in use intact: ', var_export($ok, true), "\n";
?>
--EXPECT--
evicted: many, refused once the graveyard was full: many
the hot entry stayed: yes
evicted values still in use intact: true
