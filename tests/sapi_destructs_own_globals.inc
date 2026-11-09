<?php
// $sapiDestructsOwnGlobalsSeq: a pre-existing global the test sets before each include_global()
// call, read here only to label output -- never swept, since it existed before this request.
if (!class_exists('SapiDestructsOwnGlobalsObj', false)) {
    final class SapiDestructsOwnGlobalsObj
    {
        public function __destruct()
        {
            header('X-Destructed: ' . $GLOBALS['sapiDestructsOwnGlobalsSeq']);
        }
    }
}
$owned = new SapiDestructsOwnGlobalsObj();
register_shutdown_function(static fn () => header('X-Shutdown: ' . $GLOBALS['sapiDestructsOwnGlobalsSeq']));
echo 'body ', $GLOBALS['sapiDestructsOwnGlobalsSeq'], "\n";
