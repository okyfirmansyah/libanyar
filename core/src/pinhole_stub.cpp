/// @file pinhole_stub.cpp
/// @brief Stub Pinhole implementation for platforms without native overlay support.
///
/// On these platforms, Pinhole::is_native() returns false and renders are silently
/// dropped until the full platform implementation is added (Phase 7).

#ifndef __linux__

#include <anyar/pinhole.h>

namespace anyar {

// ── PinholeRenderContext (stub) ──────────────────────────────────────────────

struct PinholeRenderContext::Impl {};

PinholeRenderContext::PinholeRenderContext(Impl* impl) : impl_(impl) {}
PinholeRenderContext::~PinholeRenderContext() = default;

std::pair<int, int> PinholeRenderContext::size_px() const { return {0, 0}; }
double PinholeRenderContext::dpr() const { return 1.0; }
void PinholeRenderContext::clear(float, float, float, float) {}
void PinholeRenderContext::draw_image(const uint8_t*, std::size_t, int, int, pixel_format) {}

// ── Pinhole::Impl (stub) ─────────────────────────────────────────────────────

struct Pinhole::Impl {
    std::string    id_;
    PinholeOptions opts_;
    bool           is_native_ = false;
};

// ── Pinhole (stub) ───────────────────────────────────────────────────────────

Pinhole::Pinhole() : impl_(std::make_unique<Impl>()) {}
Pinhole::~Pinhole() = default;
Pinhole::Pinhole(Pinhole&&) noexcept = default;
Pinhole& Pinhole::operator=(Pinhole&&) noexcept = default;

const std::string& Pinhole::id() const        { return impl_->id_; }
bool Pinhole::is_native() const               { return false; }
void Pinhole::on_render(RenderFn)             {}
void Pinhole::on_resize(std::function<void(int,int,double)>) {}
void Pinhole::on_visibility(std::function<void(bool)>)       {}
void Pinhole::on_dom_detached(std::function<void()>)         {}
void Pinhole::notify_dom_detached()           {}
void Pinhole::notify_window_destroyed()       {}
void Pinhole::request_redraw()                {}
void Pinhole::set_continuous(bool)            {}
void Pinhole::set_rect(int, int, int, int)    {}
void Pinhole::set_visible(bool)               {}
void Pinhole::set_z_index(int)                {}
int  Pinhole::z_index() const                 { return 0; }
void Pinhole::set_window_active(bool)         {}
void Pinhole::set_reorder_callback(std::function<void()>) {}
void Pinhole::reorder_in_overlay()            {}
std::string Pinhole::tracking_js()            { return {}; }

void Pinhole::platform_init(const std::string& id,
                             const PinholeOptions& opts,
                             void* /*overlay*/,
                             std::function<void(const std::string&)> /*eval_fn*/)
{
    impl_->id_   = id;
    impl_->opts_ = opts;
}

void Pinhole::override_eval_fn_for_test(std::function<void(const std::string&)>) {}

} // namespace anyar

#endif // !__linux__
