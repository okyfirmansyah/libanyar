# Graceful Shutdown

LibAnyar owns the shutdown order inside `app.run()`. App and plugin code still need to stop their own background work when `shutdown()` is called.

## Rules

- If you start long-lived work with `service_->execute()`, give it a stop flag that the loop checks quickly.
- If the loop can wait on back-pressure or consumer release, add a close/cancel path that unblocks the wait during shutdown.
- Do not call `service_->stop()` from plugin code or window-close handlers. Let `App::run()` own the sequence.
- Test by closing the native window while the app is busy, not only when idle.

## Minimal Pattern

```cpp
class MyPlugin : public anyar::IAnyarPlugin {
public:
    void initialize(anyar::PluginContext& ctx) override {
        service_ = ctx.service;
        stop_ = false;
        service_->execute([this]() {
            while (!stop_) {
                do_one_tick();
            }
        });
    }

    void shutdown() override {
        stop_ = true;
        if (pool_) pool_->close();
    }

private:
    asyik::service_ptr service_;
    std::atomic<bool> stop_{false};
};
```

## Examples

- `VideoPlugin`: stops decode work and closes the frame pool so waiting producers can exit.
- `WifiPlugin`: stops the scan loop in `shutdown()` and lets the fiber unwind naturally.