<?php
$shared = ($shared ?? 0) + 1;
\Fiber::suspend();
return $shared;
