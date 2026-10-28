# Debugging and testing

Full guide: `docs/debugging.md` in the LibAnyar repo. Everything below works on any LibAnyar binary through environment variables, so there's no rebuild.

## Switches

| Env | Effect |
|---|---|
| `ANYAR_CONSOLE=1` | Page `console.*`, uncaught errors and unhandled rejections go to the terminal as `[js:<window>] level: …` |
| `ANYAR_DEBUG_TOOLS=1` | Registers `debug:info`, `debug:screenshot`, `debug:input`, `debug:exit`; adds `window.anyarTest` |
| `ANYAR_TEST_SCRIPT=path.js` | Injects a test driver into the main window (implies the two above) |
| `ANYAR_TEST_TIMEOUT=sec` | Aborts the test with exit code 124 |
| `config.debug = true` | Webview DevTools (Linux: right-click → Inspect; Windows: F12) |

Platform support:
- Console forwarding and test scripts work everywhere.
- Screenshot and synthetic input are implemented on Linux; Windows and macOS report `capture: false` / `input: false` from `anyarTest.info()` until their backends land.
- Write tests that check `info()` and degrade gracefully.

## Triage order

1. **See the errors.** Run with `ANYAR_CONSOLE=1`. Most "the button does nothing" bugs are an exception you couldn't see.
2. **Split code from engine.** Fix the port (`config.port = 3080`) and open `http://127.0.0.1:3080/` in Chrome. If the bug reproduces there, it's your code: iterate in Chrome DevTools. If it only happens in the app, it's the engine (WebKitGTK/WebView2) or a native path (IPC binding, `anyar-shm://`, Pinhole): debug in the app.
3. **Freezes**: all IPC pauses together → something blocks the service thread. Find the command doing I/O or CPU work and wrap it in `run_blocking`.
4. **Crashes on exit** → see "shutdown race" below.

WebKit quirks to remember:
- `Error.stack` has no `Name: message` line (the forwarder adds one).
- Errors thrown from *injected* scripts show as "Script error.".
- CSS/WebGL support follows the system WebKitGTK version, not Chrome's.

## Scripted UI test (runs in the real webview)

```js
// tests/smoke.js — top-level await is fine; runs after `load`
const caps = await anyarTest.info();
await anyarTest.waitFor('#app[data-ready]', 20000);          // selector or () => condition
console.log('ready');                                        // → [js:main] log: ready

if (caps.input) {
  await anyarTest.click('#new-note');                        // real OS-level click (trusted event)
  await anyarTest.type('Buy milk');
  await anyarTest.key('Enter');
} else {                                                     // platforms without synthetic input yet
  document.querySelector('#new-note').click();
}
await anyarTest.waitFor(() => document.querySelectorAll('.note').length === 1, 5000);
anyarTest.assert(document.querySelector('.note').textContent.includes('Buy milk'), 'note text');
if (caps.capture) await anyarTest.screenshot('artifacts/notes.png');
await anyarTest.done(0);                                     // exit code
```

```bash
xvfb-run -a env ANYAR_TEST_SCRIPT=tests/smoke.js ANYAR_TEST_TIMEOUT=120 ./build/my_app; echo $?
```

`anyarTest` API:

| Member | Description |
|---|---|
| `waitFor(selectorOrFn, ms = 10000)` | Polls until the selector matches or the function returns a truthy value |
| `sleep(ms)` | Delay |
| `click(target, opts)` | Click; `target` = selector, element or `{x, y}` in CSS px; `opts` = `{ button, shift, ctrl, alt, meta }` |
| `move(target)` | Move the pointer |
| `down(target, opts)`, `up(target, opts)` | Press / release a mouse button |
| `scroll(target, dy, dx)` | Wheel at a target |
| `key(name, opts)`, `type(text)` | Keyboard input into the focused element |
| `screenshot(path)` | Capture the real webview |
| `info()` | Platform capabilities |
| `invoke(cmd, args)` | Call any command |
| `assert(cond, msg)` | Throws if false |
| `done(code)`, `fail(msg)` | Finish the test |

Exit codes: the code passed to `done()`; 1 for an uncaught error in the script; 124 for a timeout.

From app code (TS), the same capabilities are in `import { debug } from '@libanyar/api'`.

## Hunting a shutdown race

Symptoms: an occasional `malloc_consolidate(): unaligned fastbin chunk`, `double free`, `corrupted size`, a segfault, or a hang, all when the app closes. These are timing bugs, so build a loop and measure:

```bash
cat > rand_exit.js <<'EOF'
const d = Math.round(Math.random() * 2500); console.log('DELAY', d);
await anyarTest.sleep(d); await anyarTest.done(1);
EOF
for i in $(seq 30); do xvfb-run -a env ANYAR_TEST_SCRIPT=rand_exit.js ./my_app >/dev/null 2>&1; echo $?; done | sort | uniq -c
# 30 × "1" = clean; 134/139 = crash
```

- Run **sequentially**: parallel runs shift the timing and can hide the bug.
- Change one thing per batch: disable a command, a release path, an exit timing. Watch the crash rate move.
- The usual culprits, in order:
  1. State shared between `shutdown()` (main thread) and command handlers (service thread) without a lock.
  2. A BackgroundTask not stopped.
  3. A blocked producer not woken (`pool.close()`).
  4. Work still running on the worker pool when the service stops.
  5. Calling `service_->stop()` yourself.
- ASAN (`-DANYAR_ASAN=ON` in LibAnyar builds) can change timing enough to hide a race. The crash-rate loop is the reliable signal.

## Unit tests

Keep plugin logic (parsing, geometry, state machines) in plain C++ files with no `App` dependency and test them with Catch2. LibAnyar's own tests use the Catch2 bundled with LibAsyik. Test the IPC surface through the scripted UI test above, not by mocking the webview.
