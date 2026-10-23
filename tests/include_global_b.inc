<?php
// Fixture for include_global tests: run after include_global_a.inc, in the
// same request. Its own top level IS the global scope, so it sees $x set
// by the earlier include_global() call without any `global` keyword.
$y = $x . '-seen-by-b';
