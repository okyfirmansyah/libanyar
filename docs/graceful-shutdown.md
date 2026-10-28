# Graceful Shutdown

LibAnyar owns the shutdown order inside `app.run()`. App and plugin code still need to stop their own background work when `shutdown()` is called.

## Rules

- Run long-lived loops in an `anyar::BackgroundTask` (`<anyar/task.h>`). Poll `token.stop_requested()` in the loop and call `task.stop()` in `shutdown()`. `stop()` requests the stop **and joins**, so the fiber is really gone before the service stops. Fibers still alive at `service_->stop()` can make process exit spin.
- Wrap blocking calls (codecs, file I/O, heavy CPU work) in `anyar::run_blocking(service_, fn)` so they never stall the service thread. That also keeps stop latency low.
- If the loop can wait on back-pressure or consumer release, prefer a non-blocking call (`SharedBufferPool::try_acquire_write()`) or add a close/cancel path (`SharedBufferPool::close()`).
- Do not call `service_->stop()` from plugin code or window-close handlers. Let `App::run()` own the sequence.
- **`shutdown()` runs on the main (UI) thread while the service thread is still dispatching commands.** The page can still send fire-and-forget commands during shutdown, so any state that both `shutdown()` and your command handlers touch (maps of buffers, sessions, handles) needs a `std::mutex`. Without one, an exit at the wrong moment corrupts the heap. The anatomy atlas hit exactly this with its buffer map (2026-10-28).
- Commands that wait on `run_blocking()` must finish before the service stops. Count them in flight and wait for zero in `shutdown()`, or make them bail out early on a stop flag (see `AtlasPlugin::InFlight`).
- Test by closing the native window while the app is busy, not only when idle. To do that repeatably, run the app many times with a test script that exits at a random moment (`ANYAR_TEST_SCRIPT`, see [Debugging](debugging.md#recipe-hunting-a-shutdown-race)).

## Minimal Pattern

```cpp
#include <anyar/task.h>

class MyPlugin : public anyar::IAnyarPlugin {
public:
    void initialize(anyar::PluginContext& ctx) override {
        service_ = ctx.service;
        loop_.start(service_, [this](anyar::StopToken st) {
            while (!st.stop_requested()) {
                auto result = anyar::run_blocking(service_, [] { return do_blocking_tick(); });
                publish(result);
            }
        });
    }

    void shutdown() override {
        loop_.stop();   // request stop + join (default 5 s timeout)
    }

private:
    asyik::service_ptr    service_;
    anyar::BackgroundTask loop_;
};
```

## Examples

- `VideoPlugin`: decode loop is a `BackgroundTask`; `stop_playback()` joins it on re-open, close and `shutdown()`, so sessions never overlap.
- `WifiPlugin`: stops the scan loop in `shutdown()` and lets the fiber unwind naturally.