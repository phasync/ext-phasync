# PHP called from C: when it may suspend

Userland PHP that C code (or the engine) calls may suspend its fiber, and may be preempted, unless the stack holds something on the never-list below. A never-list callback runs inline: its waits block the process, and nothing interleaves. Every cooperative wait, and every preemption checkpoint, asks `phasync_never_suspend()` (poller.c); `phasync_from_c()` is the same question for a wait.

Why a list at all: PHP already has to survive a callback doing anything to userland data, so a second fiber running meanwhile is no new risk. The new risk is C state that only works if calls strictly nest: a global saved and restored around the call, an "in progress" marker a recursion check reads, or library state mid-call (OpenSSL in `SSL_read`, libxml, readline). Switching fibers inside such a call corrupts it silently. Everything else (frame-local C state, or one object whose concurrent use fails hard, like `conn_busy`) may suspend.

## The check

1. Refuse if `zend_fiber_switch_blocked()`, if `EG(in_autoload)` is non-empty (also covers the frame-less VM autoload), or if `OG(running)`.
2. Walk from the current frame to the fiber's first frame:
   - a C function frame: refuse if the function is in the table (by name, name prefix, class, or module);
   - a user frame entered from C or the engine (`ZEND_CALL_TOP`, not a generator): refuse if it is in the table (by method name, argument shape, or the opcode of the frame that entered it). A TOP frame whose `prev` is a user frame is an engine callback whatever the opcode; under `INCLUDE_OR_EVAL` only a `ZEND_CALL_CODE` frame is the included file;
   - the function a preemption checkpoint calls is a root: a wait inside it is allowed, a checkpoint inside it is not (it is not re-entrant).
3. For a checkpoint also: no destructor, no `#[\phasync\Uninterruptible]`, nothing at the bottom of the script's stack (exception handler, shutdown function) outside fibers.

## Never-list

| Callback | Detected by | Why | To take it off |
|---|---|---|---|
| autoloaders, `spl_autoload*` | `EG(in_autoload)`, `spl_autoload_call` frame | `in_autoload` is the recursion guard; a second fiber sees the class as "being loaded" | per-class-name autoload lock in a `zend_autoload` wrapper (owner coroutine; others wait), plus swapping `EG(prev_exception)`, `filename_override`, `lineno_override` per fiber |
| output handlers, `ob_*` callbacks | `OG(running)` | one global handler-running flag | swap `OG` pieces per request/coroutine |
| error handlers (also `#[\Deprecated]`, `#[\NoDiscard]`, compile-time) | TOP frame with args `(int, string, string, int)` | `EG(user_error_handler)` is unset while it runs; compile-time errors run mid-compile | none planned: the argument shape is a heuristic |
| `header_register_callback` | function identity, remembered by the override | called from `sapi_send_headers` with the headers half sent | none planned |
| ticks, pcntl signal handlers | `zend_fiber_switch_blocked()`; handler shape `(signo, siginfo)` before PHP 8.6 | PHP forbids `Fiber::suspend()` there | n/a |
| compile and include time: user stream wrapper or error handler under `INCLUDE_OR_EVAL`, `DECLARE_CLASS*` | caller opcode | suspending inside the compiler | none |
| constant expressions: `new` in initializers, defaults, constants, statics, static props | caller opcodes (`RECV_INIT`, `NEW`, `BIND_STATIC`, `FETCH_CONSTANT`, `FETCH_CLASS_CONSTANT`, `*_STATIC_PROP_*`, `DECLARE_CONST`) | evaluated half-way through resolving a class or constant | none |
| `__get`, `__set`, `__isset`, `__unset` | method name | per-object, per-property guards; the other fiber bypasses `__get` | make a second fiber wait on the guard (hook in `std_object_handlers`) |
| lazy object initializers (8.4+) | first argument has an entry in `EG(lazy_objects_store)` | initialization flags on the object | second fiber waits for initialization |
| `Serializable`, `__serialize`, `__unserialize`, `__sleep`, `__wakeup`, `serialize()`, `unserialize()` | method and function names | `BG(serialize)`, `BG(unserialize)`, `serialize_lock` | swap those per coroutine |
| `__debugInfo`, `var_dump`, `print_r`, `debug_zval_dump`, `json_encode` | method and function names | `GC_PROTECT_RECURSION` guards on the arrays and objects dumped | none: the guards are on shared data |
| `usort` family: `usort`, `uasort`, `uksort`, `array_u*`, `array_diff_u*`, `array_intersect_u*`, `ArrayObject/ArrayIterator::uasort/uksort` | name and prefix | `BG(user_compare_fci)` is global | swap `BG(user_compare_fci)` and `_cache` at each fiber switch |
| user stream wrappers and filters, except `stream_open`/`stream_read`/`stream_write` under plain I/O (`fopen`, `fread`, `fwrite`, `fgets`, `file_get_contents`, `stream_get_contents` ...) | method names | `FG(user_stream_current_filename)`, `PG(in_user_include)`, per-stream state | a busy mark per stream, failing hard; swap the two globals |
| session save handlers | module `session` | `PS(in_save_handler)` and the session state | swap `PS` per request |
| SOAP, readline, LDAP | module | library state mid-call | a busy mark per object |
| OpenSSL renegotiation limit callback | stream label `tcp_socket/ssl` as first argument | runs inside `SSL_read` | none |
| FFI callbacks | internal function with `ZEND_ACC_CALL_VIA_TRAMPOLINE` | called from foreign code on its stack | none |
| `UConverter`, `XSLTProcessor`, `DOMXPath`, `Dom\XPath`, `ZipArchive`, `mb_ereg_replace_callback` | class or function name; `toUCallback`/`fromUCallback` by method name | library or global state mid-call | a busy mark per object (`mb_ereg`: none, its regex state is global) |
| curl callbacks: write, read, header, xferinfo, progress, debug, fnmatch, prereq, ssh hostkey, multi push | `curl_exec` or `curl_multi_exec` frame under a user frame | libcurl's behaviour with two callbacks of one multi in flight is unknown, and the shared-multi design runs them from the driver | none planned. `curl_exec()` and `curl_multi_select()` themselves wait cooperatively. Tests reach write, read, xferinfo, progress, debug, header, prereq; fnmatch (ftp), ssh hostkey and push (HTTP/2) need servers not available here |
| GC destructor fiber | bottom function `gc_destructor_fiber` | not a phasync coroutine | hold `GC_G(dtor_fiber)` and resume it from the loop |

## May suspend

`array_map`, `array_filter`, `array_walk`, `array_reduce`, `array_find*`, `array_any`, `array_all`, `iterator_*`, `preg_replace_callback*`, `xml_parse*`, SQLite3/PDO behind `conn_busy`, `SplHeap`/`SplPriorityQueue`, `filter_var`, `IntlChar::enum*`, `Random\Randomizer`, and engine calls at a call site: Iterator methods under `foreach`, ArrayAccess from opcodes, `__toString`, `__clone`, property hooks, destructors at free (PHP 8.4+; 8.3 forbids a fiber switch there).

## Adding or removing an entry

Add a case to `phasync_never_cfunc()` (C functions) or `phasync_never_callback()` (user frames) in poller.c, and a `tests/c_invoked_never_*.phpt` proving it runs inline. To take one off, implement its mitigation, move its test to `tests/c_invoked_allowed_*.phpt`, and delete the entry. A new PHP extension or engine callback defaults to "may suspend": when a PHP version adds one that keeps global state across the call, it needs an entry.

Not covered by tests here (entries coded from the php-src reading): SOAP, LDAP rebind, readline completion, the OpenSSL renegotiation callback.
