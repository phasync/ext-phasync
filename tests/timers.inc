<?php
// SKIPIF helper for tests that count preemption calls in a time window: skip
// where the host's timers are too coarse (macOS CI runners: a 5 ms sleep can
// take 20-50 ms, and vary from run to run: their workflow sets
// PHASYNC_TEST_COARSE_TIMERS). $ms is the longest a 5 ms sleep may take.
function skip_if_coarse_timers(float $ms): void
{
    if (getenv('PHASYNC_TEST_COARSE_TIMERS')) die("skip the host's timers are too coarse (PHASYNC_TEST_COARSE_TIMERS)");
    $t = hrtime(true);
    for ($i = 0; $i < 10; $i++) usleep(5000);
    $took = (hrtime(true) - $t) / 1e7;
    if ($took > $ms) die(sprintf("skip the host's timers are too coarse (a 5 ms sleep takes %.0f ms)", $took));
}
