#pragma once

/// @file pinhole.h
/// @brief Native overlay ("pinhole") rendering API for LibAnyar.
///
/// A Pinhole reserves a DOM rectangle as a transparent placeholder and renders
/// a native GPU surface positioned exactly over that element, on top of the
/// webview compositor output, inside the same OS window.  C++ draws directly
/// to the native GL/Metal/D3D surface — no JS in the hot path, no SharedBuffer
/// copy, no texImage2D upload.
///
/// @par Supported CSS subset
/// The overlay is a flat native rectangle.  The following CSS properties on the
/// placeholder div (or any ancestor) do NOT affect the overlay:
///   - border-radius
///   - transform: rotate / scale / 3D (console warning + fallback engaged)
///   - opacity, mix-blend-mode, filter, backdrop-filter
///   - position: sticky (not officially supported)
///
/// @par Scroll behaviour
/// By default the overlay is hidden during scroll and reshown on idle.  This
/// avoids visible "swim" artifacts at the cost of a brief flicker.
///
/// @par Fallback
/// When GL initialisation fails (headless, sandbox, missing libepoxy) the
/// constructor does NOT throw.  Instead a fallback Pinhole is created that
/// routes to the existing SharedBuffer + FrameRenderer pipeline driving an
/// injected <canvas>.  Query Pinhole::is_native() to detect this.
///
/// @par Platform-specific implementation files
///   - Linux: core/src/pinhole_linux.cpp (GtkOverlay + GtkGLArea)
///   - Windows (Phase 7): core/src/pinhole_win32.cpp (DComp + D3D11)
///   - macOS  (Phase 7): core/src/pinhole_macos.mm  (CAMetalLayer)

#include <anyar/types.h>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <utility>

namespace anyar {

// ── Pixel formats (same enum as FrameRenderer for drop-in compatibility) ────

/// Pixel formats supported by PinholeRenderContext::draw_image().
/// Matches the pixel_format enum in @libanyar/api/canvas on the JS side.
enum class pixel_format {
    rgba,       ///< 4 bytes/px — RGBA 8-bit each
    rgb,        ///< 3 bytes/px — RGB 8-bit (no alpha)
    bgra,       ///< 4 bytes/px — BGRA byte order (Windows / Direct3D cameras)
    grayscale,  ///< 1 byte/px  — single luminance channel
    yuv420,     ///< 1.5 bytes/px — YUV 4:2:0 planar (Y + U + V planes)
    nv12,       ///< 1.5 bytes/px — YUV 4:2:0 semi-planar, UV interleaved
    nv21,       ///< 1.5 bytes/px — YUV 4:2:0 semi-planar, VU interleaved
};

// ── PinholeOptions ───────────────────────────────────────────────────────────

/// Configuration options for a Pinhole.
struct PinholeOptions {
    /// Default pixel format for draw_image(). Can be overridden per-call.
    pixel_format format = pixel_format::rgba;

    /// If true, render loop runs at vsync rate (GtkGLArea continuous mode).
    /// If false (default), only renders on request_redraw().
    bool continuous = false;

    /// If true, keep overlay visible during scroll (may cause swim artifacts).
    /// Default false: hide during scroll, reshow on idle.
    /// Phase 4g.2 JS protocol required for scroll detection; in Phase 1 with
    /// set_rect() this has no effect.
    bool show_during_scroll = false;

    /// If true, skip native GL initialisation entirely and route all rendering
    /// through the SharedBuffer + 2D-canvas fallback path.  Primarily used for
    /// testing the fallback path in CI environments where GL succeeds, and as
    /// an opt-in for applications that want predictable cross-environment
    /// behaviour at the cost of GPU acceleration.
    bool force_fallback = false;
};

// ── PinholeRenderContext ─────────────────────────────────────────────────────

/// Render context passed to the Pinhole::on_render callback.
///
/// All methods must be called only from within the on_render callback.
/// The underlying GL context is current when the callback is invoked.
class PinholeRenderContext {
public:
    /// Frame size in device pixels (CSS size × devicePixelRatio).
    std::pair<int, int> size_px() const;

    /// Device pixel ratio for the current display.
    double dpr() const;

    /// Clear the surface with an RGBA colour (each component 0.0 – 1.0).
    void clear(float r, float g, float b, float a = 1.0f);

    /// Upload and draw a CPU-side image to fill the entire surface.
    ///
    /// @param data   Pointer to raw pixel data.
    /// @param size   Total byte size of the pixel data buffer.
    /// @param width  Image width in pixels.
    /// @param height Image height in pixels.
    /// @param fmt    Pixel format of @p data.
    ///
    /// All pixel_format values are supported:
    /// - rgba, rgb, bgra, grayscale — single GL texture plane.
    /// - yuv420 (I420), nv12, nv21  — multi-plane BT.601 full-range YUV→RGB.
    void draw_image(const uint8_t* data, std::size_t size,
                    int width, int height,
                    pixel_format fmt = pixel_format::rgba);

    class Impl;
    ~PinholeRenderContext();
    PinholeRenderContext(const PinholeRenderContext&) = delete;
    PinholeRenderContext& operator=(const PinholeRenderContext&) = delete;

private:
    friend class Pinhole;
    explicit PinholeRenderContext(Impl* impl);
    Impl* impl_;  // non-owning; Impl is owned by Pinhole::Impl
};

// ── Pinhole ──────────────────────────────────────────────────────────────────

/// A native GPU surface positioned over a DOM placeholder element.
///
/// Created via Window::create_pinhole().  The pinhole tracks the
/// `data-anyar-pinhole="<id>"` attribute in the webview DOM (Phase 4g.2).
/// In Phase 1, position is set manually via set_rect().
class Pinhole {
public:
    using RenderFn = std::function<void(PinholeRenderContext&)>;

    /// Unique identifier matching the data-anyar-pinhole DOM attribute.
    const std::string& id() const;

    /// Register the render callback.  Called once per requested/continuous frame.
    /// The callback runs on the GTK main thread (Linux) with the GL context current.
    /// Thread-safe: safe to call from any thread before the first redraw.
    ///
    /// @par Fiber safety
    /// The callback executes directly on the GTK main thread, NOT inside a
    /// LibAsyik fiber.  If you spawn LibAsyik fibers inside the callback, you
    /// MUST cancel or join them before calling `App::stop()` / before the
    /// service thread exits.  Fibers still running after `service_->stop()`
    /// will cause undefined behaviour.
    ///
    /// @par Exception safety
    /// Exceptions thrown by the render callback are caught and logged; the
    /// frame is dropped.  Throwing from the callback never propagates into
    /// GTK signal handlers (which would terminate the program).
    void on_render(RenderFn fn);

    /// Register a callback fired when the overlay is resized or DPR changes.
    void on_resize(std::function<void(int width_px, int height_px, double dpr)> fn);

    /// Register a callback fired when overlay visibility changes (scroll-hide, etc.)
    void on_visibility(std::function<void(bool visible)> fn);

    /// Schedule one redraw (push model — default).
    /// Thread-safe: queues a render on the main thread.
    void request_redraw();

    /// Enable or disable continuous render mode (vsync rate).
    void set_continuous(bool enabled);

    /// Phase 1 helper: manually position the overlay (CSS pixels).
    /// In Phase 4g.2+ this is also called by the JS tracking protocol.
    /// Thread-safe: dispatches the GTK resize to the main thread.
    void set_rect(int x_css, int y_css, int width_css, int height_css);

    /// Show or hide the native overlay surface.
    /// Called by the JS scroll-hide protocol (Phase 4g.2).
    /// Thread-safe: dispatches GTK show/hide to the main thread.
    void set_visible(bool visible);

    /// Returns true if this is a real native overlay; false if falling back
    /// to the SharedBuffer + FrameRenderer pipeline.
    bool is_native() const;

    /// Returns the self-contained JS bootstrap snippet that auto-discovers
    /// [data-anyar-pinhole] elements and tracks their rects via IPC.
    /// Injected once per window by Window::create_pinhole().
    /// Returns an empty string on non-Linux platforms.
    static std::string tracking_js();

    /// Register a callback fired when the placeholder element is removed from
    /// the DOM (JS tracking sends `pinhole:dom_detached`).
    /// C++ typically stops the render loop in response.
    void on_dom_detached(std::function<void()> fn);

    /// @internal  Fire the dom_detached callback.
    /// Called by the IPC handler for `pinhole:dom_detached`.
    void notify_dom_detached();

    /// Z-order of this pinhole relative to siblings in the same window.
    /// Higher value = drawn on top.  Default: 0 (creation order).
    /// Thread-safe; dispatches the GTK reorder to the main thread.
    void set_z_index(int z);
    int  z_index() const;

    /// Called by Window on OS window minimize/restore.
    /// Pauses or resumes the GL render loop without changing user visibility.
    /// Thread-safe.
    void set_window_active(bool active);

    /// @internal  Set by Window::create_pinhole() to trigger z-order reflow
    /// when set_z_index() is called.
    void set_reorder_callback(std::function<void()> fn);

    /// @internal  Remove and re-insert this pinhole's GL area at the end of
    /// the GtkOverlay child list (= topmost among current siblings).
    /// Must be called on the GTK main thread.
    void reorder_in_overlay();

    ~Pinhole();

    // Non-copyable, movable
    Pinhole(const Pinhole&) = delete;
    Pinhole& operator=(const Pinhole&) = delete;
    Pinhole(Pinhole&&) noexcept;
    Pinhole& operator=(Pinhole&&) noexcept;

    class Impl;

    /// @internal
    /// Platform-specific initialisation — called by Window::create_pinhole()
    /// once after the Pinhole object has been constructed.
    /// @param overlay  GtkOverlay* (Linux), nullptr on other platforms.
    /// @param eval_fn  Webview JS eval function used by the canvas fallback
    ///                 path (4g.5) to inject and drive a 2D-canvas renderer
    ///                 when GL is unavailable.  May be empty on platforms
    ///                 that do not support the fallback.
    void platform_init(const std::string& id, const PinholeOptions& opts,
                       void* overlay,
                       std::function<void(const std::string&)> eval_fn);

    /// @internal  Test affordance: replace the eval function set by
    /// platform_init().  Production code should not need this.  Allows tests
    /// to capture JS injected by the fallback path without a real webview.
    /// No-op on non-Linux platforms.
    void override_eval_fn_for_test(std::function<void(const std::string&)> fn);

private:
    Pinhole();  // constructed only by Window::create_pinhole
    std::unique_ptr<Impl> impl_;
    friend class Window;
};

} // namespace anyar
