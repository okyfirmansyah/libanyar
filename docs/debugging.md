# Debugging & UI Automation

A LibAnyar UI runs inside the operating system's webview (WebKitGTK on Linux, WebView2 on Windows, WKWebView on macOS). That gives you the real engine your users get, but no browser address bar, no console in the terminal, and no headless mode. LibAnyar adds the missing pieces:

| Tool | Turn on with | What it gives you |
|---|---|---|
| [DevTools](#1-devtools) | `AppConfig::debug = true` | The engine's inspector inside the app window |
| [Console forwarding](#2-console-forwarding) | `ANYAR_CONSOLE=1` | The page's `console.*`, uncaught errors and unhandled rejections in your terminal |
| [Automation commands](#3-automation-commands) | `ANYAR_DEBUG_TOOLS=1` | Screenshots of the real webview, real (OS-level) clicks and keys, exit with a code |
| [Test scripts](#4-test-scripts) | `ANYAR_TEST_SCRIPT=test.js` | A JS driver injected into your app, so you can run scripted UI tests in the real engine, also in CI |
| [Browser mode](#5-opening-the-ui-in-a-browser) | a fixed port | The same running app in Chrome/Firefox, including headless Chrome |

The environment variables work on any LibAnyar binary, debug or release, without rebuilding. Each one can also be set in code (`AppConfig::debug_tools`, see `<anyar/debug.h>`).

> Leave `debug_tools.automation` off in builds you ship. While it is on, page code can take screenshots and synthesize input.

## Platform support

| | Linux (WebKitGTK) | Windows (WebView2) | macOS (WKWebView) |
|---|---|---|---|
| DevTools | ✅ right-click → Inspect | ✅ F12 | planned with the macOS port |
| Console forwarding | ✅ | ✅ | ✅ (JS-only) |
| Test scripts, `debug:exit`, `debug:info` | ✅ | ✅ | ✅ (JS-only) |
| `debug:screenshot` | ✅ | not yet (planned: `CapturePreview`) | not yet (planned: `takeSnapshot`) |
| `debug:input` | ✅ | not yet (planned: `SendInput` / `SendMouseInput`) | not yet (planned: synthesized `NSEvent`s) |

Ask the running app with `debug.info()` / `anyarTest.info()`. It reports `capture` and `input` as `true` or `false`, so one test script can run everywhere and skip what a platform cannot do yet.

## 1. DevTools

```cpp
anyar::AppConfig config;
config.debug = true;            // or per window: WindowConfig::debug
```

On Linux, right-click the page and choose **Inspect Element**. On Windows, press **F12**. With `debug` off, LibAnyar also disables the browser context menu and Ctrl+wheel zoom, so the app feels native. A handy pattern is to tie it to an environment variable: the examples use `ATLAS_DEBUG=1`, `VIDEO_PLAYER_DEBUG=1` and so on.

## 2. Console forwarding

```bash
ANYAR_CONSOLE=1 ./my_app
```

```
[js:main] log: loaded: Full pack · 3 LODs
[js:main] warn: this is a warning with an object {"a":1}
[js:main] error: Unhandled rejection: TypeError: bad thing
@http://127.0.0.1:41297/assets/index-3f1c.js:8:25
[js:settings] error: Uncaught ReferenceError: foo is not defined (http://…/settings.js:12:3)
```

- `log`, `info` and `debug` go to stdout; `warn` and `error` go to stderr. The prefix is the window label.
- Objects are printed as JSON and errors as `Name: message` plus the stack. Each message is cut at 4,000 characters.
- Forwarding starts before your page scripts run, so errors during startup are caught too.
- WebKit hides the details of errors thrown from *injected* scripts (test scripts) as `Uncaught Script error.`. Errors from your own page code are reported in full. In test scripts, catch and `console.error()` them yourself, or let the test wrapper do it (see below).

## 3. Automation commands

```bash
ANYAR_DEBUG_TOOLS=1 ./my_app
```

This registers four IPC commands. From the frontend, use the typed `debug` module:

```ts
import { debug } from '@libanyar/api';

const caps = await debug.info();                 // { platform, capture, input, … }
await debug.screenshot({ path: 'shots/home.png' });
await debug.click('#open-file');                 // selector, element or {x, y}
await debug.typeText('left femur');
await debug.key('Enter');
await debug.key('s', { ctrl: true });
await debug.scroll('#list', 3);
await debug.mouse('down', { x: 100, y: 100 }); await debug.moveTo({ x: 300, y: 120 }); await debug.mouse('up', { x: 300, y: 120 });
await debug.exit(0);
```

| Command | Args | Result |
|---|---|---|
| `debug:info` | — | `{ platform, console, automation, test_script, capture, input }` |
| `debug:screenshot` | `{ path?, label? }` | `{ width, height, bytes, path }`, or `png_base64` when no path is given |
| `debug:input` | `{ type, x, y, button?, dx?, dy?, key?, text?, shift?, ctrl?, alt?, meta?, label? }`. `type` is `move` · `down` · `up` · `click` · `scroll` · `key` · `text` | `{ ok }` |
| `debug:exit` | `{ code? }` | Closes every window; `App::run()` returns `code` |

Things to know:

- **Screenshots** show what the engine rendered: DOM, CSS, WebGL. They are full resolution: device pixels, so 2× on HiDPI. Native [Pinhole](pinhole-rendering.md) surfaces are composited *below* the webview and are not in the image. A relative `path` is resolved against the app's working directory.
- **Input** goes through the toolkit's event queue (GDK on Linux), not `element.dispatchEvent()`. The page receives trusted events: hover states, focus, `isTrusted`, and user-gesture rules (starting audio, opening file pickers) all behave as for a real user. Coordinates are CSS pixels from the top-left of the webview; `getBoundingClientRect()` values work directly.
- `label` targets another window; by default the command acts on the window that sent it.
- From C++, the same operations are `Window::capture(callback)` and `Window::inject_input(InputEvent)`, called on the main thread.

## 4. Test scripts

A test script is a plain JavaScript file. LibAnyar injects it into the **main window** before your page loads and runs it after the `load` event, inside an `async` function, so top-level `await` works. No bundler or test runner is needed.

```bash
ANYAR_TEST_SCRIPT=tests/smoke.js ANYAR_TEST_TIMEOUT=60 ./my_app
echo $?     # the code passed to anyarTest.done()
```

```js
// tests/smoke.js
await anyarTest.waitFor('#app.ready', 15000);          // selector or predicate
console.log('ready:', document.title);                   // shows up as [js:main] log: …

await anyarTest.click('#search');
await anyarTest.type('left femur');
await anyarTest.key('Enter');
await anyarTest.waitFor(() => !document.querySelector('#info').hidden);
anyarTest.assert(document.querySelector('#info h3').textContent === 'Left femur', 'selection');

if ((await anyarTest.info()).capture) await anyarTest.screenshot('artifacts/selected.png');
await anyarTest.done(0);
```

Setting `ANYAR_TEST_SCRIPT` also turns on console forwarding and automation.

**`anyarTest` API**

| Member | Description |
|---|---|
| `waitFor(selectorOrFn, timeoutMs = 10000)` | Polls every 50 ms; resolves with the element or truthy value, rejects on timeout |
| `sleep(ms)` | Delay |
| `click(target, opts?)`, `move(target)`, `down(target, opts?)`, `up(target, opts?)` | Pointer input; `target` = selector, element or `{x, y}`; `opts` = `{ button, shift, ctrl, alt, meta }` |
| `scroll(target, dy, dx?)` | Wheel at a target |
| `key(name, opts?)`, `type(text)` | Keyboard input into the focused element |
| `screenshot(path?)` | `debug:screenshot` |
| `info()` | `debug:info` |
| `invoke(cmd, args)` | Call any IPC command directly |
| `assert(cond, msg)` | Throws if false |
| `done(code = 0)`, `fail(msg)` | Finish the test (`fail` logs the message and exits 1) |
| `label` | This window's label |

`anyarTest` is also available in **every** window whenever automation is on, which is handy for poking at the app from DevTools.

**Exit codes**

- the code passed to `done()`;
- **1** for an uncaught error in the script (logged as `[anyarTest] uncaught: …`);
- **124** when `ANYAR_TEST_TIMEOUT` (`debug_tools.test_timeout_s`) expires first.

If the script never calls `done()`, the app keeps running, which is useful when you want the script to set up a state and then explore by hand. A page reload runs the script again.

**In CI (Linux).** Run the app under Xvfb. WebGL then uses software rendering, which is slower but works:

```bash
xvfb-run -a env GDK_BACKEND=x11 ANYAR_TEST_SCRIPT=tests/smoke.js ANYAR_TEST_TIMEOUT=120 ./build/my_app
```

Keep `GDK_BACKEND=x11`. On a Wayland desktop, GTK otherwise ignores the Xvfb display and opens the window on your real screen (it falls back to the default `wayland-0` socket even if `WAYLAND_DISPLAY` is unset). Screenshots still work then, because they come from the webview, but a screen recorder grabbing Xvfb sees only black. On a CI machine without Wayland it makes no difference.

LibAnyar's own `tests/debug_tools` test (label `display`) does exactly this. It checks the exit code, the forwarded console output, real clicks and typing, and that the screenshot is a valid PNG.

## 5. Opening the UI in a browser

The backend always serves the frontend over HTTP. When the page is not inside the native webview, `@libanyar/api` falls back to HTTP for commands and to a WebSocket for events. Pin the port and open it in any browser:

```cpp
config.port = 3080;        // the JS bridge's default when no port is injected
```

Then open `http://127.0.0.1:3080/`. You get Chrome's or Firefox's DevTools, and you can drive the page with headless Chrome over the Chrome DevTools Protocol, Puppeteer or Playwright, against the **real C++ backend**. Pass `--use-angle=gl-egl` (or `vulkan`) to give headless Chrome the real GPU for WebGL.

What this does *not* test:

- the shipped engine: you are in Blink, not WebKitGTK or WebView2;
- the native IPC binding;
- zero-copy `anyar-shm://` buffer reads (the HTTP fallback is used instead).

It is the fastest way to iterate on layout and logic. Confirm engine-specific behaviour with [test scripts](#4-test-scripts) in the real app.

**Rule of thumb:** if a bug also shows up in Chrome, it is your code: iterate there. If it only shows up in the app, it is engine- or native-path-specific: stay in the app (DevTools + console forwarding + a test script).

## Recipe: hunting a shutdown race

Crashes when the app exits at "the wrong moment" are hard to reproduce by hand. Make the moment random and run it many times:

```bash
cat > rand_exit.js <<'EOF'
const d = Math.round(Math.random() * 2500); console.log('DELAY', d);
await anyarTest.sleep(d); await anyarTest.done(1);
EOF
for i in $(seq 30); do
  xvfb-run -a env GDK_BACKEND=x11 ANYAR_TEST_SCRIPT=rand_exit.js ./my_app >run_$i.log 2>&1
  echo $?                      # 1 = clean, 134/139 = crash
done | sort | uniq -c
```

Count crashes, change one thing, and count again. Run sequentially: parallel runs change the timing, and in our case they lowered the crash rate from ~15% to ~2%. This is how the anatomy atlas's shutdown race was found: a `std::map` touched by `shutdown()` on the main thread and by commands on the service thread. See [Graceful Shutdown](graceful-shutdown.md).

## Remote inspection (not yet integrated)

- **WebView2** honours `WEBVIEW2_ADDITIONAL_BROWSER_ARGUMENTS=--remote-debugging-port=9222`. That exposes the Chrome DevTools Protocol of the *real* app, so Playwright's `connectOverCDP` can drive it.
- **WebKitGTK** has a remote inspector (`WEBKIT_INSPECTOR_SERVER=127.0.0.1:2999`).

Neither has been verified with LibAnyar yet.
