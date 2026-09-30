#pragma once

/// @file frame_mailbox.h
/// @brief Lifetime-safe "latest frame" handoff between a producer (fiber,
///        worker thread) and a consumer on another thread (e.g. a Pinhole
///        on_render callback on the GTK main thread).
///
/// Frames are reference-counted: a consumer that grabbed a frame via
/// latest() keeps it alive and unmodified for as long as it holds the
/// shared_ptr, no matter what the producer does meanwhile.  The producer
/// never blocks — acquire() recycles a frame only when nobody else holds it,
/// otherwise it allocates.
///
/// @code
///   auto mailbox = std::make_shared<anyar::FrameMailbox>();
///   pin->on_render([mailbox](anyar::PinholeRenderContext& ctx) {
///       ctx.clear(0, 0, 0, 1);
///       if (auto f = mailbox->latest()) ctx.draw_frame(*f);
///   });
///   // producer (any thread):
///   auto f = mailbox->acquire(anyar::pixel_format_byte_size(fmt, w, h));
///   fill(f->data.data());
///   f->width = w; f->height = h; f->format = fmt;
///   mailbox->publish(std::move(f));
///   pin->request_redraw();
/// @endcode

#include <anyar/pixel_format.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <vector>

namespace anyar {

/// A CPU-side image plus the metadata needed to draw it.
struct Frame {
    std::vector<uint8_t> data;   ///< Tightly-packed pixel bytes.
    int          width  = 0;     ///< Width in pixels.
    int          height = 0;     ///< Height in pixels.
    pixel_format format = pixel_format::rgba;  ///< Layout of @ref data.
    double       pts    = 0.0;   ///< Presentation timestamp (seconds), app-defined.
    uint64_t     seq    = 0;     ///< Set by FrameMailbox::publish(); increases per publish.
};

/// Single-slot, thread-safe, non-blocking frame handoff with frame recycling.
/// All methods are safe to call concurrently from any thread (they only use
/// a short std::mutex critical section — never a fiber primitive).
class FrameMailbox {
public:
    /// @param max_pooled  Upper bound on frames kept for recycling.  When all
    ///                    pooled frames are in use, acquire() allocates an
    ///                    unpooled frame instead of blocking.
    explicit FrameMailbox(std::size_t max_pooled = 8);

    /// Producer: get a writable frame whose `data` has exactly @p bytes bytes.
    /// The returned frame is never visible to consumers until publish().
    /// Contents of a recycled frame are unspecified.
    std::shared_ptr<Frame> acquire(std::size_t bytes);

    /// Producer: make @p frame the latest frame.  The previously published
    /// frame is released (and becomes recyclable once no consumer holds it).
    /// Null is ignored — use clear() to drop the current frame.
    void publish(std::shared_ptr<Frame> frame);

    /// Consumer: the most recently published frame, or null if none.
    std::shared_ptr<const Frame> latest() const;

    /// Drop the current frame (latest() returns null until the next publish).
    void clear();

    /// Number of publish() calls so far.
    uint64_t published_count() const;

private:
    mutable std::mutex                   mu_;
    std::size_t                          max_pooled_;
    std::vector<std::shared_ptr<Frame>>  pool_;
    std::shared_ptr<Frame>               latest_;
    uint64_t                             seq_ = 0;
};

} // namespace anyar
