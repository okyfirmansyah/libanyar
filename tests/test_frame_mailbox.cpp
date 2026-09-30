// LibAnyar — FrameMailbox + pixel_format helper tests

#define CATCH_CONFIG_NO_POSIX_SIGNALS
#include <catch2/catch.hpp>

#include <anyar/frame_mailbox.h>

#include <atomic>
#include <cstring>
#include <thread>

using namespace anyar;

TEST_CASE("pixel_format_byte_size: packed sizes incl. odd 4:2:0 dims", "[frame]") {
    CHECK(pixel_format_byte_size(pixel_format::rgba, 4, 2) == 32);
    CHECK(pixel_format_byte_size(pixel_format::bgra, 4, 2) == 32);
    CHECK(pixel_format_byte_size(pixel_format::rgb, 4, 2) == 24);
    CHECK(pixel_format_byte_size(pixel_format::grayscale, 4, 2) == 8);
    CHECK(pixel_format_byte_size(pixel_format::yuv420, 4, 2) == 8 + 2 * 2 * 1);
    CHECK(pixel_format_byte_size(pixel_format::nv12, 4, 2) == 8 + 2 * 2 * 1);
    // Odd sizes: chroma is ceil(w/2) x ceil(h/2) — what the renderers read.
    CHECK(pixel_format_byte_size(pixel_format::yuv420, 3, 3) == 9 + 2 * 2 * 2);
    CHECK(pixel_format_byte_size(pixel_format::nv21, 5, 1) == 5 + 2 * 3 * 1);
    CHECK(pixel_format_byte_size(pixel_format::rgba, 0, 10) == 0);
}

TEST_CASE("pixel_format names round-trip", "[frame]") {
    for (auto f : {pixel_format::rgba, pixel_format::rgb, pixel_format::bgra,
                   pixel_format::grayscale, pixel_format::yuv420,
                   pixel_format::nv12, pixel_format::nv21}) {
        auto parsed = pixel_format_from_name(pixel_format_name(f));
        REQUIRE(parsed.has_value());
        CHECK(*parsed == f);
    }
    CHECK_FALSE(pixel_format_from_name("argb").has_value());
}

TEST_CASE("FrameMailbox: latest() returns the most recent publish", "[frame]") {
    FrameMailbox mb;
    CHECK(mb.latest() == nullptr);

    auto a = mb.acquire(16);
    CHECK(a->data.size() == 16);
    a->pts = 1.0;
    mb.publish(a);
    auto b = mb.acquire(16);
    b->pts = 2.0;
    mb.publish(b);

    auto l = mb.latest();
    REQUIRE(l);
    CHECK(l->pts == 2.0);
    CHECK(l->seq == 2);
    CHECK(mb.published_count() == 2);

    mb.clear();
    CHECK(mb.latest() == nullptr);
    mb.publish(nullptr);                 // ignored
    CHECK(mb.latest() == nullptr);
}

TEST_CASE("FrameMailbox: frames held by a consumer are never recycled", "[frame]") {
    FrameMailbox mb(2);
    auto f1 = mb.acquire(8);
    mb.publish(f1);
    auto held = mb.latest();             // consumer keeps frame 1
    f1.reset();

    auto f2 = mb.acquire(8);
    CHECK(f2.get() != held.get());
    mb.publish(f2);                      // frame 1 no longer latest…
    f2.reset();

    auto f3 = mb.acquire(8);             // …but still held → not reused
    CHECK(f3.get() != held.get());
    CHECK(f3.get() != mb.latest().get());
}

TEST_CASE("FrameMailbox: a released frame is recycled", "[frame]") {
    FrameMailbox mb(4);
    auto f1 = mb.acquire(8);
    Frame* raw1 = f1.get();
    mb.publish(f1);
    f1.reset();
    auto f2 = mb.acquire(8);
    mb.publish(f2);                      // frame 1 dropped by the mailbox
    f2.reset();
    auto f3 = mb.acquire(32);
    CHECK(f3.get() == raw1);             // reused allocation
    CHECK(f3->data.size() == 32);
}

TEST_CASE("FrameMailbox: acquire never blocks when the pool is exhausted", "[frame]") {
    FrameMailbox mb(1);
    auto a = mb.acquire(4);
    auto b = mb.acquire(4);              // pool full + in use → fresh frame
    auto c = mb.acquire(4);
    CHECK(a.get() != b.get());
    CHECK(b.get() != c.get());
}

TEST_CASE("FrameMailbox: concurrent producer/consumer never sees a torn frame", "[frame][stress]") {
    FrameMailbox mb(3);
    constexpr size_t kBytes = 64 * 1024;
    std::atomic<bool> stop{false};
    std::atomic<int> torn{0}, reads{0};

    std::thread consumer([&] {
        while (!stop) {
            if (auto f = mb.latest()) {
                // Every byte of a published frame carries the same value;
                // while we hold it the producer must not rewrite it.
                const uint8_t v = f->data[0];
                for (size_t i = 0; i < f->data.size(); i += 97) {
                    if (f->data[i] != v) { ++torn; break; }
                }
                if (f->data.back() != v) ++torn;
                ++reads;
            }
        }
    });

    for (int i = 0; i < 3000; ++i) {
        auto f = mb.acquire(kBytes);
        std::memset(f->data.data(), i & 0xFF, kBytes);
        mb.publish(std::move(f));
    }
    stop = true;
    consumer.join();
    CHECK(torn.load() == 0);
    CHECK(reads.load() > 0);
}
