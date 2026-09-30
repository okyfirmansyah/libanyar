# Graceful Shutdown

LibAnyar owns the shutdown order inside `app.run()`. App and plugin code still need to stop their own background work when `shutdown()` is called.

## Rules

- Run long-lived loops in an `anyar::BackgroundTask` (`<anyar/task.h>`). Poll `token.stop_requested()` in the loop and call `task.stop()` in `shutdown()`. `stop()` requests the stop **and joins**, so the fiber is really gone before the service stops. Fibers still alive at `service_->stop()` can make process exit spin.
- Wrap blocking calls (codecs, file I/O, heavy CPU work) in `anyar::run_blocking(service_, fn)` so they never stall the service thread. That also keeps stop latency low.
- If the loop can wait on back-pressure or consumer release, prefer a non-blocking call (`SharedBufferPool::try_acquire_write()`) or add a close/cancel path (`SharedBufferPool::close()`).
- Do not call `service_->stop()` from plugin code or window-close handlers. Let `App::run()` own the sequence.
- Test by closing the native window while the app is busy, not only when idle.

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