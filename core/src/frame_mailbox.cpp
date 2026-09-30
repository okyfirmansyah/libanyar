#include <anyar/frame_mailbox.h>

namespace anyar {

// ── pixel_format helpers ────────────────────────────────────────────────────

std::size_t pixel_format_byte_size(pixel_format fmt, int width, int height) {
    if (width <= 0 || height <= 0) return 0;
    const auto w  = static_cast<std::size_t>(width);
    const auto h  = static_cast<std::size_t>(height);
    const auto cw = (w + 1) / 2;   // chroma planes round UP for odd sizes
    const auto ch = (h + 1) / 2;
    switch (fmt) {
        case pixel_format::rgba:
        case pixel_format::bgra:      return w * h * 4;
        case pixel_format::rgb:       return w * h * 3;
        case pixel_format::grayscale: return w * h;
        case pixel_format::yuv420:          // Y | U (cw×ch) | V (cw×ch)
        case pixel_format::nv12:            // Y | UV interleaved (2·cw × ch)
        case pixel_format::nv21:      return w * h + 2 * cw * ch;
    }
    return 0;
}

const char* pixel_format_name(pixel_format fmt) {
    switch (fmt) {
        case pixel_format::rgba:      return "rgba";
        case pixel_format::rgb:       return "rgb";
        case pixel_format::bgra:      return "bgra";
        case pixel_format::grayscale: return "grayscale";
        case pixel_format::yuv420:    return "yuv420";
        case pixel_format::nv12:      return "nv12";
        case pixel_format::nv21:      return "nv21";
    }
    return "rgba";
}

std::optional<pixel_format> pixel_format_from_name(const std::string& name) {
    for (auto f : {pixel_format::rgba, pixel_format::rgb, pixel_format::bgra,
                   pixel_format::grayscale, pixel_format::yuv420,
                   pixel_format::nv12, pixel_format::nv21}) {
        if (name == pixel_format_name(f)) return f;
    }
    return std::nullopt;
}

// ── FrameMailbox ────────────────────────────────────────────────────────────

FrameMailbox::FrameMailbox(std::size_t max_pooled) : max_pooled_(max_pooled) {}

std::shared_ptr<Frame> FrameMailbox::acquire(std::size_t bytes) {
    std::shared_ptr<Frame> frame;
    {
        std::lock_guard<std::mutex> lk(mu_);
        // A pooled frame is free when the pool holds the only reference:
        // not latest_ (that would be a second ref), not held by a consumer,
        // not handed to the producer.  New refs can only be created under
        // mu_ (latest()) or by us, so the check cannot race.
        for (auto& f : pool_) {
            if (f.use_count() == 1) { frame = f; break; }
        }
        if (!frame) {
            frame = std::make_shared<Frame>();
            if (pool_.size() < max_pooled_) pool_.push_back(frame);
        }
    }
    // Resize outside the lock: nobody else can see this frame yet.
    frame->data.resize(bytes);
    return frame;
}

void FrameMailbox::publish(std::shared_ptr<Frame> frame) {
    if (!frame) return;
    std::shared_ptr<Frame> previous;
    {
        std::lock_guard<std::mutex> lk(mu_);
        frame->seq = ++seq_;
        previous = std::move(latest_);
        latest_  = std::move(frame);
    }
    // `previous` is released here, outside the lock.
}

std::shared_ptr<const Frame> FrameMailbox::latest() const {
    std::lock_guard<std::mutex> lk(mu_);
    return latest_;
}

void FrameMailbox::clear() {
    std::shared_ptr<Frame> previous;
    {
        std::lock_guard<std::mutex> lk(mu_);
        previous = std::move(latest_);
    }
}

uint64_t FrameMailbox::published_count() const {
    std::lock_guard<std::mutex> lk(mu_);
    return seq_;
}

} // namespace anyar
