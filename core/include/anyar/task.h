#pragma once

/// @file task.h
/// @brief Background-work primitives for plugins and command handlers.
///
/// LibAnyar runs every command handler, HTTP route and event on ONE service
/// thread (cooperative fibers).  Anything that blocks that thread — FFmpeg,
/// file I/O, compression, a long SQL query — freezes all IPC, events and
/// HTTP for its duration.  Two tools fix that:
///
///  - run_blocking(): run a blocking callable on LibAsyik's worker pool and
///    suspend only the calling *fiber* until it finishes.
///  - BackgroundTask: a long-lived fiber loop with a cooperative stop token
///    and a real join, so a plugin can stop a loop, wait for it to be gone,
///    and start a fresh one without the two ever overlapping.

#include <libasyik/service.hpp>

#include <boost/fiber/future.hpp>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <functional>
#include <memory>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>

namespace anyar {

/// Run @p fn on LibAsyik's worker thread pool and wait for its result.
///
/// Called from a fiber, only that fiber is suspended — other fibers on the
/// service thread (IPC, HTTP, events) keep running.  Called from a plain
/// thread, that thread blocks.  Exceptions thrown by @p fn are rethrown here.
///
/// @p fn runs on another thread: it must not touch fiber-only state or call
/// webview APIs, and anything it captures by reference must outlive the call
/// (it does — the caller waits).
///
/// @param service  Service whose worker pool to use (e.g. PluginContext::service).
/// @param fn       Callable with no arguments.
/// @returns        Whatever @p fn returns.
/// @code
///   auto peaks = anyar::run_blocking(service_, [path] { return decode_peaks(path); });
/// @endcode
template <typename F>
auto run_blocking(const asyik::service_ptr& service, F&& fn) -> std::invoke_result_t<F&> {
    if (!service) throw std::invalid_argument("run_blocking: null service");
    return service->async(std::forward<F>(fn)).get();
}

/// Cooperative cancellation flag handed to a BackgroundTask body.
class StopToken {
public:
    StopToken() = default;
    /// True once BackgroundTask::request_stop() (or stop()) was called.
    bool stop_requested() const noexcept { return flag_ && flag_->load(std::memory_order_acquire); }

private:
    friend class BackgroundTask;
    explicit StopToken(std::shared_ptr<std::atomic<bool>> f) : flag_(std::move(f)) {}
    std::shared_ptr<std::atomic<bool>> flag_;
};

/// A restartable fiber loop with cooperative stop and join.
///
/// The body runs as a fiber on the service thread and must poll
/// `token.stop_requested()` regularly (and between run_blocking() calls).
/// Exceptions escaping the body are caught and logged; the task still counts
/// as finished.
///
/// join()/join_for() may be called from a fiber (suspends the fiber) or from
/// a plain thread such as the GTK main thread in IAnyarPlugin::shutdown()
/// (blocks the thread) — but NOT from inside the task's own body, and not
/// after the service has stopped (the body can no longer run to completion).
///
/// @code
///   anyar::BackgroundTask decode_;
///   decode_.start(service_, [this](anyar::StopToken st) {
///       while (!st.stop_requested()) { ... }
///   });
///   decode_.stop();   // request_stop() + join_for(5s)
/// @endcode
class BackgroundTask {
public:
    BackgroundTask() = default;
    /// Requests stop but does not wait (a destructor must never block on
    /// the service thread).  Call stop() explicitly in plugin shutdown().
    ~BackgroundTask() { request_stop(); }

    BackgroundTask(const BackgroundTask&) = delete;
    BackgroundTask& operator=(const BackgroundTask&) = delete;

    /// Launch @p body as a fiber on @p service.
    /// @throws std::logic_error if a previous run has not finished yet —
    ///         call stop() (or request_stop() + join()) first.
    void start(const asyik::service_ptr& service, std::function<void(StopToken)> body) {
        if (!service) throw std::invalid_argument("BackgroundTask::start: null service");
        if (running()) throw std::logic_error("BackgroundTask::start: previous run still active");

        auto run = std::make_shared<Run>();
        run->done_future = run->done.get_future().share();
        run_ = run;

        service->execute([run, body = std::move(body)]() mutable {
            try {
                body(StopToken(run->stop));
            } catch (const std::exception& e) {
                std::fprintf(stderr, "[LibAnyar] BackgroundTask body threw: %s\n", e.what());
            } catch (...) {
                std::fprintf(stderr, "[LibAnyar] BackgroundTask body threw a non-std exception\n");
            }
            body = nullptr;   // release captures before signalling completion
            run->finished.store(true, std::memory_order_release);
            run->done.set_value();
        });
    }

    /// Ask the running body to exit.  Non-blocking; safe from any thread.
    void request_stop() noexcept {
        if (auto r = run_) r->stop->store(true, std::memory_order_release);
    }

    /// Wait until the current run (if any) has finished.
    void join() {
        if (auto r = run_) r->done_future.wait();
    }

    /// Wait at most @p timeout.  @returns true if no run is active afterwards.
    bool join_for(std::chrono::milliseconds timeout) {
        auto r = run_;
        if (!r) return true;
        return r->done_future.wait_for(timeout) == boost::fibers::future_status::ready;
    }

    /// request_stop() + join_for(@p timeout).  @returns true if it stopped.
    bool stop(std::chrono::milliseconds timeout = std::chrono::seconds(5)) {
        request_stop();
        return join_for(timeout);
    }

    /// True between start() and the body returning.
    bool running() const noexcept {
        auto r = run_;
        return r && !r->finished.load(std::memory_order_acquire);
    }

private:
    struct Run {
        std::shared_ptr<std::atomic<bool>> stop = std::make_shared<std::atomic<bool>>(false);
        std::atomic<bool>                  finished{false};
        boost::fibers::promise<void>       done;
        boost::fibers::shared_future<void> done_future;
    };
    // Only mutated by start(); plugins call start/stop from service fibers
    // or from shutdown() after all commands have finished.
    std::shared_ptr<Run> run_;
};

} // namespace anyar
