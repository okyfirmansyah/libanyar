// LibAnyar — run_blocking() + BackgroundTask tests

#define CATCH_CONFIG_NO_POSIX_SIGNALS
#include <catch2/catch.hpp>

#include <anyar/task.h>

#include <boost/fiber/operations.hpp>

#include <atomic>
#include <chrono>
#include <stdexcept>
#include <thread>

using namespace anyar;
using namespace std::chrono_literals;

// Run `body` as a fiber on a fresh service; returns when body returns.
template <typename F>
static void with_service(F&& body) {
    auto svc = asyik::make_service();
    svc->execute([&] {
        body(svc);
        svc->stop();
    });
    svc->run();
}

TEST_CASE("run_blocking: runs off the service thread and returns the value", "[task]") {
    with_service([](asyik::service_ptr svc) {
        const auto service_tid = std::this_thread::get_id();
        std::thread::id worker_tid;
        int v = run_blocking(svc, [&] {
            worker_tid = std::this_thread::get_id();
            return 42;
        });
        CHECK(v == 42);
        CHECK(worker_tid != service_tid);
    });
}

TEST_CASE("run_blocking: rethrows exceptions from the job", "[task]") {
    with_service([](asyik::service_ptr svc) {
        CHECK_THROWS_WITH(run_blocking(svc, []() -> int { throw std::runtime_error("boom"); }),
                          "boom");
    });
}

TEST_CASE("run_blocking: other fibers keep running while a job blocks", "[task]") {
    with_service([](asyik::service_ptr svc) {
        std::atomic<int> ticks{0};
        std::atomic<bool> done{false}, ticker_exited{false};
        svc->execute([&] {
            while (!done) { ++ticks; boost::this_fiber::sleep_for(5ms); }
            ticker_exited = true;
        });
        run_blocking(svc, [] { std::this_thread::sleep_for(150ms); return 0; });
        done = true;
        // Fibers must be gone before svc->stop() (libasyik rule).
        while (!ticker_exited) boost::this_fiber::sleep_for(1ms);
        // The ticker fiber ran ~30 times while the service thread was "busy".
        CHECK(ticks.load() >= 10);
    });
}

TEST_CASE("BackgroundTask: stop() requests stop and joins", "[task]") {
    with_service([](asyik::service_ptr svc) {
        BackgroundTask task;
        std::atomic<int> iterations{0};
        task.start(svc, [&](StopToken st) {
            while (!st.stop_requested()) { ++iterations; boost::this_fiber::sleep_for(1ms); }
        });
        boost::this_fiber::sleep_for(20ms);
        CHECK(task.running());
        CHECK(task.stop(1s));
        CHECK_FALSE(task.running());
        const int after = iterations.load();
        boost::this_fiber::sleep_for(10ms);
        CHECK(iterations.load() == after);   // really gone
    });
}

TEST_CASE("BackgroundTask: start while running throws; restart after stop works", "[task]") {
    with_service([](asyik::service_ptr svc) {
        BackgroundTask task;
        std::atomic<int> runs{0};
        auto body = [&](StopToken st) {
            ++runs;
            while (!st.stop_requested()) boost::this_fiber::sleep_for(1ms);
        };
        task.start(svc, body);
        CHECK_THROWS_AS(task.start(svc, body), std::logic_error);
        CHECK(task.stop(1s));
        task.start(svc, body);           // fresh stop token
        boost::this_fiber::sleep_for(5ms);
        CHECK(task.running());
        CHECK(task.stop(1s));
        CHECK(runs.load() == 2);
    });
}

TEST_CASE("BackgroundTask: a throwing body still finishes", "[task]") {
    with_service([](asyik::service_ptr svc) {
        BackgroundTask task;
        task.start(svc, [](StopToken) { throw std::runtime_error("body failed"); });
        CHECK(task.join_for(1s));
        CHECK_FALSE(task.running());
    });
}

TEST_CASE("BackgroundTask: join_for times out on a body that ignores stop", "[task]") {
    with_service([](asyik::service_ptr svc) {
        BackgroundTask task;
        std::atomic<bool> release{false};
        task.start(svc, [&](StopToken) {
            while (!release) boost::this_fiber::sleep_for(1ms);
        });
        CHECK_FALSE(task.stop(30ms));
        release = true;
        CHECK(task.join_for(1s));
    });
}

TEST_CASE("BackgroundTask: can be joined from a plain (non-service) thread", "[task]") {
    auto svc = asyik::make_service();
    std::thread service_thread([&] { svc->run(); });

    BackgroundTask task;
    std::atomic<bool> in_body{false};
    task.start(svc, [&](StopToken st) {
        in_body = true;
        while (!st.stop_requested()) boost::this_fiber::sleep_for(1ms);
    });
    for (int i = 0; i < 200 && !in_body; ++i) std::this_thread::sleep_for(1ms);
    CHECK(in_body.load());
    CHECK(task.stop(2s));          // blocks this thread, like plugin shutdown()
    CHECK_FALSE(task.running());

    svc->stop();
    service_thread.join();
}

TEST_CASE("BackgroundTask: join on a never-started task returns immediately", "[task]") {
    BackgroundTask task;
    CHECK_FALSE(task.running());
    CHECK(task.join_for(0ms));
    task.join();
    CHECK(task.stop());
}
