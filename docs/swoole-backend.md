# Goal for a later release: phasync on Swoole

Status: direction agreed, proof of concept on branch `poc/pseudo-fiber`; not scheduled. Swerve ships first.

## The goal

When the Swoole extension is loaded, phasync runs on Swoole: its tasks are Swoole coroutines, its event loop
is Swoole's, and ext-phasync's own concurrency (cooperative blocking functions, `Suspension::poll()`,
preemption) backs off. phasync's semantics stay: `phasync::go()` still returns a `\Fiber`, and Frame,
cancellation and deadlines, exceptions, `Scope`, `Channel`, `WaitGroup` and `StringBuffer` behave as on
ext-phasync, with the deviations listed below.

## Findings

- **Real Fibers can't be the tasks.** A Zend Fiber inside a Swoole coroutine works alone, but two that
  interleave across coroutines crash PHP (Swoole 6.3.0RC1, PHP 8.5.12). Swoole saves and restores
  `EG(active_fiber)` and `EG(current_fiber_context)` per coroutine, but every coroutine's Fibers return to
  the one shared main fiber context. This is a Swoole bug, and it remains for any library that makes its own
  Fibers (Revolt, Amp) inside Swoole coroutines.
- **ext-phasync's own loop blocks Swoole.** Its `Suspension::poll()` epoll stalls Swoole's reactor, and
  Swoole's runtime hooks wrap ext-phasync's function overrides. The two can't both run the concurrency.
- **phasync without the extension already coexists with Swoole**, except that a blocking call in one phasync
  fiber stalls its sibling fibers.
- **Swoole can interrupt most waits.** `Coroutine::cancel($cid, true)` interrupts sleeps, sockets,
  `Co\Channel`, `Co::yield()` and `flock()` within about 50 ms. Two cases differ: a FIFO `open()` (Swoole's
  thread pool) only takes the exception when it completes, and `shell_exec()` only when the child process
  exits.

## Design

**Pseudo-fibers (ext-phasync).** `phasync\pseudo_fiber()` returns a real `\Fiber` object that never runs as
a Zend fiber; its own object handlers mark it. `Fiber`'s methods (start, resume, throw, getReturn, the
`is*()` methods) on such an object go to a PHP driver registered with `phasync\pseudo_fiber_driver()`.
`Fiber::getCurrent()` and `Fiber::suspend()` ask that driver when no real fiber is running. Other Fibers are
untouched.

**Swoole driver (PHP).** Each task is a plain coroutine started with `Co::create()`; its handle is the
pseudo-fiber.

| Operation | Becomes |
|---|---|
| `Fiber::suspend($v)` in a task | `Co::yield()` |
| `resume()` / `throw()` on a parked task | `Co::resume($cid)` |
| `throw($e)` into a task blocked in a hooked call | `phasync\swoole_cancel($cid, $e)` |

`start()` and `resume()` return the next suspend value, or `null` when the task blocks in a hooked call
instead and carries on by itself.

**Throwing any exception (ext-phasync).** `phasync\swoole_cancel($cid, $e)` calls Swoole's
`cancel($cid, true)` and replaces the `CanceledException` that Swoole creates with `$e`, keeping Swoole's
as `$e`'s previous. Swoole restores that exception in its `on_resume` callback, which bypasses Zend's throw
hook, so the swap chains `on_resume`. It is armed for a coroutine resumed inside `cancel()`, and keyed by the
exception's object handle for one resumed later.

**phasync core (phasync-core's code).** Extract a driver interface from EventLoop's mechanics:

- start/resume/throw a task;
- park and wake;
- readable/writable;
- timers and the clock (`now`);
- defer/run.

The semantic layer stays as it is: frames, scopes, cancellation, flags and exceptions. The current mechanics
become the default driver and the Swoole driver a second one. `Channel`, `WaitGroup` and `StringBuffer`
need no change, since they only use `phasync::awaitFlag`/`raiseFlag`/`sleep`/`yield`/`readable`/
`writable`/`getFiber`.

## Proof of concept

The branch `poc/pseudo-fiber` holds the ext-phasync part (`pseudo_fiber.c`), the Swoole driver
(`poc/swoole/SwooleFibers.php`) and its test (`poc/swoole/test.php`). The test passes 23/23, and
ext-phasync's suite still passes. It covers:

- Fiber API semantics;
- 50 tasks that interleave hooked sleeps with parks, without crashing and running concurrently;
- `throw()` into parked tasks, into hooked calls, and into a wait Swoole can't interrupt;
- a structured-concurrency case: one child fails and its sibling's hooked socket read is cancelled with
  the parent's exception;
- FiberErrors that match a real Fiber's, native Fibers unaffected, handles freed.

```
php -n -d extension=/path/to/swoole.so -d extension=modules/phasync.so poc/swoole/test.php
```

The PoC has these limits:

- It reads Swoole's `on_resume` through a C++ symbol (`dlsym`), so it is tied to Swoole's internals.
- It needs NTS Swoole: in ZTS builds that pointer is `thread_local`.
- It allows one driver per process.
- It finds Swoole's exception by scanning the object store.

## Expected deviations from ext-phasync-backed phasync

- No preemption, `Suspension`, `sapi()` or `run_next()`: those are ext-phasync's own loop.
- `start()`/`resume()` return `null` when the task blocks in a hooked call instead of parking.
- Waits Swoole can't interrupt take a cancellation only when they end.
- Real Fibers created inside tasks still hit the Swoole bug above.

## Remaining work

1. **Driver seam in phasync core**, by phasync-core. Run the characterization tests (`docs/SEMANTICS.md`,
   68 of 76 rules pinned) against the Swoole driver, and report the results per rule.
2. **`Scope` following Swoole coroutines.** ext-scope hooks Swoole's `on_yield`/`on_resume`, or the
   `fiber-interop/scope` polyfill.
3. **Clock and timers.** Swoole timers, and an injectable clock.
4. **ext-phasync backs off when Swoole is loaded.** No cooperative overrides, no poller.
5. **Production quality for the PoC parts.** Stub/arginfo; a pseudo-fiber's `__construct()` blocked;
   ZTS; a cleaner way to find Swoole's exception; phpt tests in CI (CI already builds Swoole).
6. **Report the Fiber-in-coroutine crash to Swoole**, with `host3.php`-style repro: two coroutines, one
   Fiber parked and one returning.
