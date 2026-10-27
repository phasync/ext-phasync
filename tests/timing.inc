<?php
// A wait lasted its timeout: at least that long, and less than 0.25 s more. Timers on
// macOS CI runners overrun by up to ~0.15 s, native usleep() and stream_select() as well.
function in_range(float $elapsed, float $timeout): bool { return $elapsed >= $timeout - 0.01 && $elapsed < $timeout + 0.25; }
