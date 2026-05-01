/// @file pinhole_linux.cpp
/// @brief Linux (GtkOverlay + GtkGLArea) implementation of Pinhole.
///
/// Widget hierarchy established by this module (see ADR-008):
///
///   GtkWindow
///     └── GtkOverlay                    ← inserted by Window::show_window()
///           ├── WebKitWebView           ← main child (fills overlay)
///           └── GtkGLArea  (per-pin)    ← overlay child, positioned by get-child-position signal
///
/// GL context: OpenGL 3.3 core, libepoxy for function loading.
/// All GTK calls must run on the GTK main thread.

#ifdef __linux__

#include <anyar/pinhole.h>
#include <anyar/shared_buffer.h>
#include <anyar/window.h>

#include <epoxy/gl.h>
#include <gtk/gtk.h>

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <utility>

namespace anyar {

// ── GLSL sources (GL 3.3 core) ───────────────────────────────────────────────

static constexpr const char* kVertexSrc = R"glsl(
#version 330 core
// Fullscreen-quad, UV origin at top-left.
// Vertices provided as (ndc_x, ndc_y, u, v).
layout(location = 0) in vec2 aPos;
layout(location = 1) in vec2 aUV;
out vec2 vUV;
void main() {
    gl_Position = vec4(aPos, 0.0, 1.0);
    vUV = aUV;
}
)glsl";

static constexpr const char* kFragmentSrc = R"glsl(
#version 330 core
uniform sampler2D uTex;
uniform vec4 uClearColour;
uniform bool uHasTexture;
in vec2 vUV;
out vec4 fragColour;
void main() {
    if (uHasTexture) {
        fragColour = texture(uTex, vUV);
    } else {
        fragColour = uClearColour;
    }
}
)glsl";

// YUV multi-plane → RGB fragment shader (BT.601 full-range, GL 3.3 core).
// Supports YUV420/I420 (3 separate GL_RED planes) and NV12/NV21
// (Y GL_RED + UV/VU GL_RG interleaved plane).
// uYUVMode: 0 = YUV420, 1 = NV12 (UV interleaved), 2 = NV21 (VU interleaved).
static constexpr const char* kYUVFragmentSrc = R"glsl(
#version 330 core
uniform sampler2D uTexY;   // Luma plane (full-size, GL_RED)
uniform sampler2D uTexU;   // Cb plane (half-size GL_RED) or UV/VU plane (GL_RG)
uniform sampler2D uTexV;   // Cr plane (half-size GL_RED, YUV420 only)
uniform int       uYUVMode;
in  vec2 vUV;
out vec4 fragColour;
void main() {
    float y = texture(uTexY, vUV).r;
    float u, v;
    if (uYUVMode == 0) {         // YUV420 / I420
        u = texture(uTexU, vUV).r;
        v = texture(uTexV, vUV).r;
    } else if (uYUVMode == 1) {  // NV12: UV interleaved (.r=U, .g=V)
        vec2 uv = texture(uTexU, vUV).rg;
        u = uv.r; v = uv.g;
    } else {                     // NV21: VU interleaved (.r=V, .g=U)
        vec2 vu = texture(uTexU, vUV).rg;
        v = vu.r; u = vu.g;
    }
    // BT.601 full-range: Y in [0,1], U/V in [0,1] centred at 0.5
    u -= 0.5; v -= 0.5;
    float r = y                + 1.40200 * v;
    float g = y - 0.34414 * u - 0.71414 * v;
    float b = y + 1.77200 * u;
    fragColour = vec4(clamp(vec3(r, g, b), 0.0, 1.0), 1.0);
}
)glsl";

// Fullscreen-quad vertices: (NDC x, NDC y, U, V)
// UV origin at top-left so draw_image() needn't flip.
static constexpr float kQuadVerts[] = {
    // tri 1
    -1.f, -1.f, 0.f, 1.f,
     1.f, -1.f, 1.f, 1.f,
    -1.f,  1.f, 0.f, 0.f,
    // tri 2
     1.f, -1.f, 1.f, 1.f,
     1.f,  1.f, 1.f, 0.f,
    -1.f,  1.f, 0.f, 0.f,
};

// ── Compile / link helpers ────────────────────────────────────────────────────

static GLuint compile_shader(GLenum type, const char* src) {
    GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &src, nullptr);
    glCompileShader(s);
    GLint ok = GL_FALSE;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[512] = {};
        glGetShaderInfoLog(s, sizeof(log), nullptr, log);
        g_warning("anyar::Pinhole shader compile error: %s", log);
        glDeleteShader(s);
        return 0;
    }
    return s;
}

static GLuint link_program(GLuint vs, GLuint fs) {
    if (!vs || !fs) return 0;
    GLuint prog = glCreateProgram();
    glAttachShader(prog, vs);
    glAttachShader(prog, fs);
    glLinkProgram(prog);
    GLint ok = GL_FALSE;
    glGetProgramiv(prog, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[512] = {};
        glGetProgramInfoLog(prog, sizeof(log), nullptr, log);
        g_warning("anyar::Pinhole shader link error: %s", log);
        glDeleteProgram(prog);
        return 0;
    }
    return prog;
}

// ── Per-plane GL upload helpers ───────────────────────────────────────────────
// All helpers require a GL context to be current.

/// Upload or update a single-channel (GL_R8) texture plane (Y / Cb / Cr).
static void upload_r8_plane(GLuint& tex_id, const uint8_t* data, int w, int h) {
    if (!tex_id) {
        glGenTextures(1, &tex_id);
        glBindTexture(GL_TEXTURE_2D, tex_id);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S,     GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T,     GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, w, h, 0,
                     GL_RED, GL_UNSIGNED_BYTE, data);
    } else {
        glBindTexture(GL_TEXTURE_2D, tex_id);
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, w, h,
                        GL_RED, GL_UNSIGNED_BYTE, data);
    }
}

/// Upload or update a two-channel (GL_RG8) texture plane (NV12/NV21 chroma).
static void upload_rg8_plane(GLuint& tex_id, const uint8_t* data, int w, int h) {
    if (!tex_id) {
        glGenTextures(1, &tex_id);
        glBindTexture(GL_TEXTURE_2D, tex_id);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S,     GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T,     GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RG8, w, h, 0,
                     GL_RG, GL_UNSIGNED_BYTE, data);
    } else {
        glBindTexture(GL_TEXTURE_2D, tex_id);
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, w, h,
                        GL_RG, GL_UNSIGNED_BYTE, data);
    }
}

// ── FallbackState ────────────────────────────────────────────────────────────
//
// Heap-allocated state shared between Pinhole::Impl and any g_idle_add tasks
// queued from non-main threads.  Using a shared_ptr means the idle callback
// can check `dead` even after Pinhole::Impl has been freed.

struct FallbackState {
    std::atomic<bool>                        dead{false};
    std::atomic<bool>                        redraw_pending{false}; ///< coalesce request_redraw
    std::string                              id;
    std::mutex                               mu;
    std::shared_ptr<SharedBuffer>            buf;   ///< RGBA frame buffer (guarded by mu)
    std::function<void(const std::string&)>  eval;  ///< webview_eval wrapper (guarded by mu)
    int                                      w{0};  ///< Current canvas width  (px)
    int                                      h{0};  ///< Current canvas height (px)
};

// ── PinholeRenderContext::Impl ────────────────────────────────────────────────

struct PinholeRenderContext::Impl {
    // GL state (current when render callback is invoked)
    int width_px  = 0;
    int height_px = 0;
    double dpr_   = 1.0;

    // Single-plane program (non-owning ref — owned by Pinhole::Impl)
    GLuint program     = 0;
    GLint  loc_clear   = -1;  // uClearColour
    GLint  loc_has_tex = -1;  // uHasTexture
    GLint  loc_tex     = -1;  // uTex (texture unit 0)

    // YUV multi-plane program (non-owning ref — owned by Pinhole::Impl)
    GLuint yuv_program    = 0;
    GLint  loc_yuv_mode   = -1;  // uYUVMode
    GLint  loc_yuv_tex_y  = -1;  // uTexY (unit 0)
    GLint  loc_yuv_tex_u  = -1;  // uTexU (unit 1)
    GLint  loc_yuv_tex_v  = -1;  // uTexV (unit 2, YUV420 only)

    // Single-plane texture state (lazy upload)
    GLuint       tex          = 0;
    int          tex_w        = 0;
    int          tex_h        = 0;
    bool         has_tex      = false;
    pixel_format current_fmt  = pixel_format::rgba;

    // YUV multi-plane texture state
    GLuint       tex_y         = 0;  // Luma (all YUV modes)
    GLuint       tex_uv        = 0;  // Chroma: Cb (yuv420) or UV/VU (nv12/nv21)
    GLuint       tex_v         = 0;  // Cr (yuv420 only)
    int          tex_yuv_w     = 0;
    int          tex_yuv_h     = 0;
    pixel_format current_yuv_fmt = pixel_format::yuv420;

    // Geometry (non-owning refs)
    GLuint vao = 0;
    GLuint vbo = 0;

    // CPU fallback mode (active when Pinhole::is_native_ == false, 4g.5)
    bool     cpu_mode = false;
    uint8_t* cpu_rgba = nullptr;  ///< Non-owning; points into FallbackState::buf
};

// ── Pinhole::Impl ─────────────────────────────────────────────────────────────

struct Pinhole::Impl {
    // Identity
    std::string       id_;
    PinholeOptions    opts_;
    bool              is_native_ = true;

    // GTK widgets (non-owning: GTK tree owns lifetime)
    GtkGLArea*  gl_area_ = nullptr;
    GtkOverlay* overlay_ = nullptr;  // parent overlay (Window::Impl owns)

    // Rect in CSS pixels (set_rect() / Phase 4g.2 auto-tracking)
    int rect_x_ = 0, rect_y_ = 0, rect_w_ = 0, rect_h_ = 0;

    // User callbacks (protected by mutex for cross-thread on_render())
    std::mutex              cb_mutex_;
    Pinhole::RenderFn       render_fn_;
    std::function<void(int,int,double)> resize_fn_;
    std::function<void(bool)>           visibility_fn_;

    // GL objects (valid only while GL context current)
    GLuint program_     = 0;
    GLuint yuv_program_ = 0;  ///< Multi-plane YUV → RGB program
    GLuint vao_         = 0;
    GLuint vbo_         = 0;

    // Render context helper (re-used per frame)
    PinholeRenderContext::Impl ctx_impl_;

    // Signal handler IDs for cleanup
    gulong sig_realize_     = 0;
    gulong sig_unrealize_   = 0;
    gulong sig_render_      = 0;
    gulong sig_child_pos_   = 0;

    /// Frame-clock tick callback id for continuous rendering. 0 = none.
    /// GtkGLArea::auto_render only re-renders when the widget is invalidated;
    /// to drive a real per-frame animation we install a tick callback that
    /// calls gtk_gl_area_queue_render() once per compositor frame.
    guint  tick_id_         = 0;

    // DOM lifecycle / z-order
    std::function<void()>  dom_detached_fn_;  ///< Fired by notify_dom_detached()
    std::function<void()>  reorder_fn_;       ///< Set by Window to trigger z reflow
    bool                   user_visible_  = true;  ///< Last value from set_visible()
    bool                   window_active_ = true;  ///< False when OS window minimized
    int                    z_index_       = 0;     ///< Z-order (higher = on top)

    /// Set to true in ~Impl() before any GTK cleanup.
    /// create_gl_area() checks this flag so a pending g_idle_add cannot
    /// use a dangling `this` pointer after the Impl has been destroyed.
    std::atomic<bool> destroyed_{false};

    // Fallback (non-GL) canvas renderer — active when is_native_ == false.
    // Created in set_eval_fn(); shared with g_idle_add tasks via shared_ptr
    // so that non-main-thread ~Impl() does not leave dangling pointers.
    std::shared_ptr<FallbackState> fb_state_;
    bool                           fb_canvas_injected_ = false;

    /// Invoke the render callback with a CPU-backed render context.
    /// Lives here (inside Pinhole::Impl) so it can access
    /// PinholeRenderContext's private constructor via Pinhole's friend status.
    static void invoke_cpu_render(const Pinhole::RenderFn& fn,
                                   PinholeRenderContext::Impl* ctx_impl) {
        PinholeRenderContext rctx(ctx_impl);
        fn(rctx);
    }

    // ── init_from_window() ────────────────────────────────────────────────
    // Called indirectly from Window::create_pinhole() via Pinhole::platform_init().
    // overlay_ must be set before calling (or may be nullptr if not yet shown).
    // GTK operations dispatched via g_idle_add so they run on the main thread.

    void init_from_window(const std::string& id, const PinholeOptions& opts,
                          GtkOverlay* overlay,
                          std::function<void(const std::string&)> eval_fn) {
        id_      = id;
        opts_    = opts;
        overlay_ = overlay;

        // Always create the FallbackState so eval_fn / set_rect can work even
        // before / without GL.  Eval may legitimately be empty on platforms
        // that wired no webview (test fixtures).
        fb_state_     = std::make_shared<FallbackState>();
        fb_state_->id = id_;
        if (eval_fn) {
            std::lock_guard<std::mutex> lk(fb_state_->mu);
            fb_state_->eval = std::move(eval_fn);
        }

        if (opts_.force_fallback) {
            // Skip GL widget creation entirely; flag native off immediately.
            // activate_fallback_canvas() will be called from set_rect().
            is_native_ = false;
            return;
        }

        // Defer GTK widget creation to main thread
        g_idle_add(+[](gpointer data) -> gboolean {
            static_cast<Impl*>(data)->create_gl_area();
            return G_SOURCE_REMOVE;
        }, this);
    }

    // ── activate_fallback_canvas() ───────────────────────────────────────
    // Allocate / resize the RGBA SharedBuffer and inject / update the JS
    // canvas in the placeholder div.  Safe to call from any thread.
    // No-op if rect dimensions are not yet known or eval_fn not set.

    void activate_fallback_canvas(int w, int h) {
        if (!fb_state_) return;
        if (w <= 0 || h <= 0) return;

        const std::string  buf_name = "__anyar_fb_" + id_;
        const std::size_t  sz       = static_cast<std::size_t>(w) * h * 4;
        bool               need_js  = false;

        {
            std::lock_guard<std::mutex> lk(fb_state_->mu);
            if (!fb_state_->eval) return;  // no webview attached yet

            if (fb_state_->w != w || fb_state_->h != h || !fb_state_->buf) {
                // (Re)allocate the shared memory buffer.
                if (fb_state_->buf) {
                    SharedBufferRegistry::instance().remove(fb_state_->buf->name());
                    fb_state_->buf.reset();
                }
                fb_state_->buf = SharedBuffer::create(buf_name, sz);
                std::memset(fb_state_->buf->data(), 0, sz);
                fb_state_->w = w;
                fb_state_->h = h;
                need_js = true;
            } else if (!fb_canvas_injected_) {
                need_js = true;
            }
        }

        if (!need_js && fb_canvas_injected_) return;

        // Build the idempotent canvas init / resize JS (runs in webview).
        std::string js;
        js.reserve(1024);
        js += "(function(){";
        if (!fb_canvas_injected_) {
            // Global one-time setup
            js += "if(!window.__anyar_pb_renderers){";
            js += "window.__anyar_pb_renderers={};";
            js += "window.__anyar_pb_frame=function(id){";
            js += "var info=window.__anyar_pb_renderers[id];";
            js += "if(!info||!info.canvas)return;";
            js += "fetch('anyar-shm://__anyar_fb_'+id)";
            js += ".then(function(r){return r.arrayBuffer();})";
            js += ".then(function(buf){";
            js += "var d=new ImageData(new Uint8ClampedArray(buf),info.w,info.h);";
            js += "info.ctx.putImageData(d,0,0);";
            js += "}).catch(function(){});};";
            js += "}";
            fb_canvas_injected_ = true;
        }
        // Per-pinhole canvas setup / resize (idempotent)
        js += "var id='";
        js += id_;
        js += "',w=";
        js += std::to_string(w);
        js += ",h=";
        js += std::to_string(h);
        js += ";var div=document.querySelector('[data-anyar-pinhole=\"'+id+'\"]');";
        js += "if(!div)return;";
        js += "var cv=document.getElementById('__anyar_fb_'+id);";
        js += "if(!cv){cv=document.createElement('canvas');";
        js += "cv.id='__anyar_fb_'+id;";
        js += "cv.style.cssText='position:absolute;top:0;left:0;width:100%;height:100%;pointer-events:none;';";
        js += "if(getComputedStyle(div).position==='static')div.style.position='relative';";
        js += "div.appendChild(cv);";
        js += "console.warn('[anyar] Pinhole \"'+id+'\": GL unavailable, using 2D canvas fallback.');";
        js += "}";
        js += "cv.width=w;cv.height=h;";
        js += "window.__anyar_pb_renderers[id]={canvas:cv,ctx:cv.getContext('2d'),w:w,h:h};";
        js += "})();";

        std::function<void(const std::string&)> eval_copy;
        {
            std::lock_guard<std::mutex> lk(fb_state_->mu);
            eval_copy = fb_state_->eval;
        }
        if (eval_copy) eval_copy(js);
    }

    // ── create_gl_area() ─────────────────────────────────────────────────
    // Must run on the GTK main thread.

    void create_gl_area() {
        // Guard: if Impl was destroyed before this idle callback fired, bail out.
        if (destroyed_.load(std::memory_order_acquire)) return;

        // The overlay pointer may be set from Window::Impl after show_window()
        // has been called.  If it isn't set yet (race: pinhole created before
        // the window is shown), defer again.
        if (!overlay_) {
            g_idle_add(+[](gpointer data) -> gboolean {
                static_cast<Impl*>(data)->create_gl_area();
                return G_SOURCE_REMOVE;
            }, this);
            return;
        }

        gl_area_ = GTK_GL_AREA(gtk_gl_area_new());
        // Request GL 3.3 core
        gtk_gl_area_set_required_version(gl_area_, 3, 3);
        gtk_gl_area_set_use_es(gl_area_, FALSE);
        gtk_gl_area_set_has_alpha(gl_area_, TRUE);
        gtk_gl_area_set_has_depth_buffer(gl_area_, FALSE);

        // auto_render: when something invalidates the widget, automatically
        // render. We always enable it; the per-frame drive for continuous mode
        // comes from the tick callback installed in on_realize().
        gtk_gl_area_set_auto_render(gl_area_, TRUE);

        // Signals
        sig_realize_ = g_signal_connect(
            gl_area_, "realize",
            G_CALLBACK(+[](GtkGLArea* area, gpointer d) {
                static_cast<Impl*>(d)->on_realize(area);
            }), this);

        sig_unrealize_ = g_signal_connect(
            gl_area_, "unrealize",
            G_CALLBACK(+[](GtkGLArea* area, gpointer d) {
                static_cast<Impl*>(d)->on_unrealize(area);
            }), this);

        sig_render_ = g_signal_connect(
            gl_area_, "render",
            G_CALLBACK(+[](GtkGLArea* area, GdkGLContext* ctx, gpointer d) -> gboolean {
                return static_cast<Impl*>(d)->on_render(area, ctx);
            }), this);

        // get-child-position: GtkOverlay calls this to position overlay children
        sig_child_pos_ = g_signal_connect(
            overlay_, "get-child-position",
            G_CALLBACK(+[](GtkOverlay* /*ov*/, GtkWidget* child,
                           GdkRectangle* alloc, gpointer d) -> gboolean {
                auto* self = static_cast<Impl*>(d);
                if (child != GTK_WIDGET(self->gl_area_)) return FALSE;
                // Convert CSS px rect to device px (scale factor)
                int sf = gtk_widget_get_scale_factor(child);
                alloc->x      = self->rect_x_;
                alloc->y      = self->rect_y_;
                alloc->width  = self->rect_w_;
                alloc->height = self->rect_h_;
                (void)sf;  // GtkAllocation is in logical (CSS) pixels
                return TRUE;
            }), this);

        // Add as overlay child (drawn above the main WebKitWebView)
        gtk_overlay_add_overlay(overlay_, GTK_WIDGET(gl_area_));
        // Allow the GtkGLArea to receive input (can be toggled later)
        gtk_overlay_set_overlay_pass_through(overlay_, GTK_WIDGET(gl_area_), TRUE);
        gtk_widget_show(GTK_WIDGET(gl_area_));
    }

    // ── on_realize() ─────────────────────────────────────────────────────
    // GL context is current; compile shaders, upload geometry.

    void on_realize(GtkGLArea* area) {
        gtk_gl_area_make_current(area);
        if (GError* err = gtk_gl_area_get_error(area)) {
            g_warning("anyar::Pinhole '%s': GL realize error: %s "
                      "\xe2\x80\x94 activating 2D canvas fallback.",
                      id_.c_str(), err->message);
            is_native_ = false;
            activate_fallback_canvas(rect_w_, rect_h_);
            return;
        }

        GLuint vs = compile_shader(GL_VERTEX_SHADER,   kVertexSrc);
        GLuint fs = compile_shader(GL_FRAGMENT_SHADER, kFragmentSrc);
        program_  = link_program(vs, fs);
        glDeleteShader(vs);
        glDeleteShader(fs);

        if (!program_) {
            g_warning("anyar::Pinhole '%s': GL program link failed "
                      "\xe2\x80\x94 activating 2D canvas fallback.", id_.c_str());
            is_native_ = false;
            activate_fallback_canvas(rect_w_, rect_h_);
            return;
        }

        // Uniforms
        ctx_impl_.program     = program_;
        ctx_impl_.loc_clear   = glGetUniformLocation(program_, "uClearColour");
        ctx_impl_.loc_has_tex = glGetUniformLocation(program_, "uHasTexture");
        ctx_impl_.loc_tex     = glGetUniformLocation(program_, "uTex");

        // YUV program — reuses the same vertex shader
        {
            GLuint yuv_vs = compile_shader(GL_VERTEX_SHADER,   kVertexSrc);
            GLuint yuv_fs = compile_shader(GL_FRAGMENT_SHADER, kYUVFragmentSrc);
            yuv_program_  = link_program(yuv_vs, yuv_fs);
            glDeleteShader(yuv_vs);
            glDeleteShader(yuv_fs);
        }
        ctx_impl_.yuv_program   = yuv_program_;
        ctx_impl_.loc_yuv_mode  = glGetUniformLocation(yuv_program_, "uYUVMode");
        ctx_impl_.loc_yuv_tex_y = glGetUniformLocation(yuv_program_, "uTexY");
        ctx_impl_.loc_yuv_tex_u = glGetUniformLocation(yuv_program_, "uTexU");
        ctx_impl_.loc_yuv_tex_v = glGetUniformLocation(yuv_program_, "uTexV");

        // VAO / VBO for fullscreen quad
        glGenVertexArrays(1, &vao_);
        glGenBuffers(1, &vbo_);
        glBindVertexArray(vao_);
        glBindBuffer(GL_ARRAY_BUFFER, vbo_);
        glBufferData(GL_ARRAY_BUFFER, sizeof(kQuadVerts), kQuadVerts, GL_STATIC_DRAW);
        glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float),
                              reinterpret_cast<void*>(0));
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float),
                              reinterpret_cast<void*>(2 * sizeof(float)));
        glEnableVertexAttribArray(1);
        glBindVertexArray(0);
        glBindBuffer(GL_ARRAY_BUFFER, 0);

        ctx_impl_.vao = vao_;
        ctx_impl_.vbo = vbo_;

        // Drive per-frame rendering for continuous mode now that GL is ready.
        if (opts_.continuous && window_active_) {
            install_tick_cb_if_needed();
        }
    }

    // ── on_unrealize() ───────────────────────────────────────────────────

    void on_unrealize(GtkGLArea* area) {
        // Stop the per-frame tick before tearing down GL objects.
        if (tick_id_) {
            gtk_widget_remove_tick_callback(GTK_WIDGET(area), tick_id_);
            tick_id_ = 0;
        }
        gtk_gl_area_make_current(area);
        destroy_gl_objects();
    }

    // Install a frame-clock tick callback that queues a redraw every frame.
    // Idempotent. Caller must be on the main thread.
    void install_tick_cb_if_needed() {
        if (tick_id_ || !gl_area_) return;
        tick_id_ = gtk_widget_add_tick_callback(
            GTK_WIDGET(gl_area_),
            +[](GtkWidget* w, GdkFrameClock*, gpointer) -> gboolean {
                gtk_gl_area_queue_render(GTK_GL_AREA(w));
                return G_SOURCE_CONTINUE;
            },
            nullptr, nullptr);
    }

    void remove_tick_cb() {
        if (!tick_id_ || !gl_area_) { tick_id_ = 0; return; }
        gtk_widget_remove_tick_callback(GTK_WIDGET(gl_area_), tick_id_);
        tick_id_ = 0;
    }

    // Destroy all GL objects (must be called with context current).
    void destroy_gl_objects() {
        // Single-plane texture
        if (ctx_impl_.tex) {
            glDeleteTextures(1, &ctx_impl_.tex);
            ctx_impl_.tex     = 0;
            ctx_impl_.has_tex = false;
        }
        // YUV multi-plane textures
        if (ctx_impl_.tex_y)  { glDeleteTextures(1, &ctx_impl_.tex_y);  ctx_impl_.tex_y  = 0; }
        if (ctx_impl_.tex_uv) { glDeleteTextures(1, &ctx_impl_.tex_uv); ctx_impl_.tex_uv = 0; }
        if (ctx_impl_.tex_v)  { glDeleteTextures(1, &ctx_impl_.tex_v);  ctx_impl_.tex_v  = 0; }
        ctx_impl_.tex_yuv_w = 0;
        ctx_impl_.tex_yuv_h = 0;
        // Geometry and programs
        if (vao_) { glDeleteVertexArrays(1, &vao_); vao_ = 0; ctx_impl_.vao = 0; }
        if (vbo_) { glDeleteBuffers(1, &vbo_);      vbo_ = 0; ctx_impl_.vbo = 0; }
        if (program_)     { glDeleteProgram(program_);     program_     = 0; ctx_impl_.program     = 0; }
        if (yuv_program_) { glDeleteProgram(yuv_program_); yuv_program_ = 0; ctx_impl_.yuv_program = 0; }
    }

    // ── on_render() ──────────────────────────────────────────────────────
    // Called by GtkGLArea with GL context current.

    gboolean on_render(GtkGLArea* area, GdkGLContext* /*ctx*/) {
        if (!program_) return FALSE;

        // Determine frame size
        int scale = gtk_widget_get_scale_factor(GTK_WIDGET(area));
        GtkAllocation alloc;
        gtk_widget_get_allocation(GTK_WIDGET(area), &alloc);
        int w_px = alloc.width  * scale;
        int h_px = alloc.height * scale;

        ctx_impl_.width_px  = w_px;
        ctx_impl_.height_px = h_px;
        ctx_impl_.dpr_      = static_cast<double>(scale);

        glViewport(0, 0, w_px, h_px);
        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

        // Default: transparent clear
        glClearColor(0.f, 0.f, 0.f, 0.f);
        glClear(GL_COLOR_BUFFER_BIT);

        // Invoke user render callback (exceptions caught — must NOT propagate
        // into GTK signal handlers, which would call std::terminate).
        {
            std::lock_guard<std::mutex> lock(cb_mutex_);
            if (render_fn_) {
                PinholeRenderContext rctx(&ctx_impl_);
                try {
                    render_fn_(rctx);
                } catch (const std::exception& e) {
                    g_warning("anyar::Pinhole '%s': render callback threw: %s "
                              "\xe2\x80\x94 frame dropped.",
                              id_.c_str(), e.what());
                } catch (...) {
                    g_warning("anyar::Pinhole '%s': render callback threw "
                              "non-std exception \xe2\x80\x94 frame dropped.",
                              id_.c_str());
                }
            }
        }

        glFlush();
        return TRUE;
    }

    // ── destroy() ────────────────────────────────────────────────────────
    // Safe to call from any thread; marshals cleanup to main thread.

    void destroy() {
        if (!gl_area_) return;
        GtkGLArea* area = gl_area_;
        GtkOverlay* ov  = overlay_;
        gl_area_  = nullptr;
        overlay_  = nullptr;

        // Disconnect the child-position signal from the overlay
        if (sig_child_pos_ && ov) {
            g_signal_handler_disconnect(G_OBJECT(ov), sig_child_pos_);
            sig_child_pos_ = 0;
        }

        // Remove widget from overlay and let GTK finalize it
        if (ov && area) {
            gtk_widget_destroy(GTK_WIDGET(area));
        }
    }

    ~Impl() {
        // Signal any pending create_gl_area() idle callbacks not to proceed.
        destroyed_.store(true, std::memory_order_release);

        // Clear fallback state so that any queued do_fallback_render() idles
        // become no-ops.  Must happen before the GTK widget is destroyed so
        // the SharedBuffer is removed from the registry first.
        if (fb_state_) {
            fb_state_->dead.store(true, std::memory_order_release);
            {
                std::lock_guard<std::mutex> lk(fb_state_->mu);
                fb_state_->eval = nullptr;
                if (fb_state_->buf) {
                    SharedBufferRegistry::instance().remove(fb_state_->buf->name());
                    fb_state_->buf.reset();
                }
            }
        }

        if (!gl_area_) {
            // GTK widget was never created (e.g. window never shown, headless
            // test, or destroy() was already called).  Nothing to do.
            return;
        }

        if (g_main_context_is_owner(g_main_context_default())) {
            // Normal path: Window::~Impl calls pinholes.clear() on main thread
            // before webview_destroy(), so we get here with the loop running.
            destroy();
        } else {
            // Non-main-thread path (e.g. a fiber drops its shared_ptr<Pinhole>).
            // We cannot call GTK from this thread.  Extract the GTK handles and
            // schedule their cleanup on the main thread via g_idle_add().
            // g_idle_add() is thread-safe (just appends to the main context queue).
            //
            // Invariant: because Window::~Impl always calls pinholes.clear()
            // BEFORE webview_destroy(), the GTK main loop is still running when
            // this fires.
            struct Cleanup {
                GtkWidget*  gl_area;
                GtkOverlay* overlay;
                gulong      sig_child_pos;
            };
            auto* c = new Cleanup{
                GTK_WIDGET(gl_area_), overlay_, sig_child_pos_
            };
            // Prevent double-destroy in case destroy() or the overlay signal
            // handler fires before the idle runs.
            gl_area_       = nullptr;
            overlay_       = nullptr;
            sig_child_pos_ = 0;
            // Hold a GTK reference so the widget survives until the idle fires.
            // Guard with GTK_IS_WIDGET: if the widget was already destroyed by
            // GTK's hierarchy teardown (GtkWindow cascade) before we got here,
            // g_object_ref would fire a GLib critical and the idle is unnecessary.
            if (!GTK_IS_WIDGET(c->gl_area)) {
                delete c;
                return;
            }
            g_object_ref(c->gl_area);
            g_idle_add(+[](gpointer data) -> gboolean {
                auto* cl = static_cast<Cleanup*>(data);
                if (cl->sig_child_pos && cl->overlay) {
                    g_signal_handler_disconnect(
                        G_OBJECT(cl->overlay), cl->sig_child_pos);
                }
                // gtk_widget_destroy triggers unrealize → destroy_gl_objects
                // with the GL context current.
                gtk_widget_destroy(cl->gl_area);
                g_object_unref(cl->gl_area);
                delete cl;
                return G_SOURCE_REMOVE;
            }, c);
        }
    }
};

// ── CPU pixel-format → RGBA conversion (fallback render path, 4g.5) ──────────
//
// Converts all supported pixel_format values to packed RGBA bytes.
// `dst` must be at least dst_w * dst_h * 4 bytes.
// When src and dst dimensions differ, performs a cheap nearest-neighbor
// resample so behaviour is consistent with the GL path (which stretches
// via glViewport).

static inline void yuv601_to_rgba(float y, float u, float v, uint8_t out[4]) {
    out[0] = static_cast<uint8_t>(std::clamp(y + 1.40200f * v,                0.f, 1.f) * 255.f);
    out[1] = static_cast<uint8_t>(std::clamp(y - 0.34414f * u - 0.71414f * v, 0.f, 1.f) * 255.f);
    out[2] = static_cast<uint8_t>(std::clamp(y + 1.77200f * u,                0.f, 1.f) * 255.f);
    out[3] = 255u;
}

// Sample one source pixel at integer (sx, sy) and convert to RGBA bytes in `out`.
static inline void sample_pixel_rgba(uint8_t out[4],
                                       const uint8_t* src, int src_w, int src_h,
                                       int sx, int sy, pixel_format fmt)
{
    switch (fmt) {
        case pixel_format::rgba: {
            const uint8_t* p = src + (static_cast<std::size_t>(sy) * src_w + sx) * 4;
            out[0] = p[0]; out[1] = p[1]; out[2] = p[2]; out[3] = p[3];
            break;
        }
        case pixel_format::bgra: {
            const uint8_t* p = src + (static_cast<std::size_t>(sy) * src_w + sx) * 4;
            out[0] = p[2]; out[1] = p[1]; out[2] = p[0]; out[3] = p[3];
            break;
        }
        case pixel_format::rgb: {
            const uint8_t* p = src + (static_cast<std::size_t>(sy) * src_w + sx) * 3;
            out[0] = p[0]; out[1] = p[1]; out[2] = p[2]; out[3] = 255u;
            break;
        }
        case pixel_format::grayscale: {
            const uint8_t g = src[static_cast<std::size_t>(sy) * src_w + sx];
            out[0] = out[1] = out[2] = g; out[3] = 255u;
            break;
        }
        case pixel_format::yuv420: {
            const int      cw = (src_w + 1) / 2;
            const int      ch = (src_h + 1) / 2;
            const uint8_t* Y  = src;
            const uint8_t* U  = Y + static_cast<std::size_t>(src_w) * src_h;
            const uint8_t* V  = U + static_cast<std::size_t>(cw)    * ch;
            const float    y  = Y[static_cast<std::size_t>(sy) * src_w + sx] * (1.f / 255.f);
            const float    u  = U[static_cast<std::size_t>(sy / 2) * cw + (sx / 2)] * (1.f / 255.f) - 0.5f;
            const float    v  = V[static_cast<std::size_t>(sy / 2) * cw + (sx / 2)] * (1.f / 255.f) - 0.5f;
            yuv601_to_rgba(y, u, v, out);
            break;
        }
        case pixel_format::nv12:
        case pixel_format::nv21: {
            const int      cw    = (src_w + 1) / 2;
            const bool     nv21  = (fmt == pixel_format::nv21);
            const uint8_t* Y     = src;
            const uint8_t* UV    = Y + static_cast<std::size_t>(src_w) * src_h;
            const float    y     = Y[static_cast<std::size_t>(sy) * src_w + sx] * (1.f / 255.f);
            const std::size_t ix = (static_cast<std::size_t>(sy / 2) * cw + (sx / 2)) * 2;
            const float    c0    = UV[ix]     * (1.f / 255.f) - 0.5f;
            const float    c1    = UV[ix + 1] * (1.f / 255.f) - 0.5f;
            const float    u     = nv21 ? c1 : c0;
            const float    v     = nv21 ? c0 : c1;
            yuv601_to_rgba(y, u, v, out);
            break;
        }
    }
}

static void cpu_draw_image(uint8_t* dst, int dst_w, int dst_h,
                            const uint8_t* src, int src_w, int src_h,
                            pixel_format fmt)
{
    if (src_w <= 0 || src_h <= 0 || dst_w <= 0 || dst_h <= 0) return;

    // Identity-size fast path for rgba (memcpy)
    if (src_w == dst_w && src_h == dst_h && fmt == pixel_format::rgba) {
        std::memcpy(dst, src, static_cast<std::size_t>(src_w) * src_h * 4);
        return;
    }

    // General case (with optional nearest-neighbor resample).  Uses 64-bit
    // intermediates so it stays correct for very large dst dims.
    for (int y = 0; y < dst_h; ++y) {
        const int sy = static_cast<int>(
            (static_cast<std::int64_t>(y) * src_h) / dst_h);
        const int sy_c = sy < src_h ? sy : src_h - 1;
        for (int x = 0; x < dst_w; ++x) {
            const int sx = static_cast<int>(
                (static_cast<std::int64_t>(x) * src_w) / dst_w);
            const int sx_c = sx < src_w ? sx : src_w - 1;
            sample_pixel_rgba(&dst[(static_cast<std::size_t>(y) * dst_w + x) * 4],
                               src, src_w, src_h, sx_c, sy_c, fmt);
        }
    }
}

// ── do_fallback_render() ───────────────────────────────────────────────────────
//
// Called from g_idle_add tasks queued by request_redraw() in fallback mode.
// Takes shared_ptr copies captured at dispatch time so it is safe even if
// Pinhole::Impl is destroyed before the idle fires.

static void do_fallback_render(const std::shared_ptr<FallbackState>& state,
                                const Pinhole::RenderFn&              fn)
{
    // Clear the coalesce flag at the start of the render so any request_redraw()
    // calls fired DURING the user callback enqueue a fresh idle.
    state->redraw_pending.store(false, std::memory_order_release);

    if (state->dead.load(std::memory_order_acquire)) return;

    // Snapshot buf + eval under the mutex to hold a refcount on buf.
    std::shared_ptr<SharedBuffer>            buf;
    std::function<void(const std::string&)>  eval;
    int w, h;
    {
        std::lock_guard<std::mutex> lk(state->mu);
        if (!state->buf || !state->eval) return;
        buf  = state->buf;
        eval = state->eval;
        w    = state->w;
        h    = state->h;
    }
    if (w <= 0 || h <= 0) return;

    // Clear to transparent black before invoking user callback.
    std::memset(buf->data(), 0, static_cast<std::size_t>(w) * h * 4);

    // Build a CPU render context backed by the shared buffer.
    PinholeRenderContext::Impl ctx{};
    ctx.width_px  = w;
    ctx.height_px = h;
    ctx.dpr_      = 1.0;
    ctx.cpu_mode  = true;
    ctx.cpu_rgba  = buf->data();

    // Exceptions thrown from the user callback must NOT propagate into the
    // GLib idle dispatch (which would terminate the program).
    try {
        Pinhole::Impl::invoke_cpu_render(fn, &ctx);
    } catch (const std::exception& e) {
        g_warning("anyar::Pinhole '%s': fallback render callback threw: %s "
                  "\xe2\x80\x94 frame dropped.", state->id.c_str(), e.what());
        return;
    } catch (...) {
        g_warning("anyar::Pinhole '%s': fallback render callback threw "
                  "non-std exception \xe2\x80\x94 frame dropped.",
                  state->id.c_str());
        return;
    }

    // Notify the JS canvas to fetch and display the new frame.
    if (!state->dead.load(std::memory_order_acquire)) {
        eval("if(window.__anyar_pb_frame)window.__anyar_pb_frame('" + state->id + "')");
    }
}

// ── PinholeRenderContext implementation ───────────────────────────────────────

PinholeRenderContext::PinholeRenderContext(Impl* impl) : impl_(impl) {}
PinholeRenderContext::~PinholeRenderContext() = default;

std::pair<int, int> PinholeRenderContext::size_px() const {
    return {impl_->width_px, impl_->height_px};
}

double PinholeRenderContext::dpr() const {
    return impl_->dpr_;
}

void PinholeRenderContext::clear(float r, float g, float b, float a) {
    if (impl_->cpu_mode) {
        const auto to_byte = [](float f) -> uint8_t {
            return static_cast<uint8_t>(std::clamp(f * 255.f, 0.f, 255.f));
        };
        const uint8_t cr = to_byte(r), cg = to_byte(g),
                      cb = to_byte(b), ca = to_byte(a);
        const int n = impl_->width_px * impl_->height_px;
        uint8_t*  p = impl_->cpu_rgba;
        for (int i = 0; i < n; ++i) {
            p[i*4+0]=cr; p[i*4+1]=cg; p[i*4+2]=cb; p[i*4+3]=ca;
        }
        return;
    }
    glClearColor(r, g, b, a);
    glClear(GL_COLOR_BUFFER_BIT);
}

void PinholeRenderContext::draw_image(const uint8_t* data, std::size_t size,
                                      int width, int height, pixel_format fmt)
{
    // ── Size validation ─────────────────────────────────────────────────────
    std::size_t expected = 0;
    switch (fmt) {
        case pixel_format::rgba:
        case pixel_format::bgra:
            expected = static_cast<std::size_t>(width) * height * 4; break;
        case pixel_format::rgb:
            expected = static_cast<std::size_t>(width) * height * 3; break;
        case pixel_format::grayscale:
            expected = static_cast<std::size_t>(width) * height;     break;
        case pixel_format::yuv420:
        case pixel_format::nv12:
        case pixel_format::nv21:
            expected = static_cast<std::size_t>(width) * height * 3 / 2; break;
    }
    if (size < expected) {
        g_warning("anyar::Pinhole::draw_image: buffer too small "
                  "(%zu bytes, need %zu). Skipping frame.", size, expected);
        return;
    }

    // ── CPU fallback path (4g.5) ─────────────────────────────────────────────
    if (impl_->cpu_mode) {
        cpu_draw_image(impl_->cpu_rgba, impl_->width_px, impl_->height_px,
                       data, width, height, fmt);
        return;
    }

    // ── YUV multi-plane path ─────────────────────────────────────────────────
    const bool is_yuv = (fmt == pixel_format::yuv420 ||
                         fmt == pixel_format::nv12   ||
                         fmt == pixel_format::nv21);
    if (is_yuv) {
        if (!impl_->yuv_program) {
            g_warning("anyar::Pinhole::draw_image: YUV program not ready.");
            return;
        }
        const int chroma_w = (width  + 1) / 2;
        const int chroma_h = (height + 1) / 2;

        // Detect size or sub-format change:
        // YUV420 uses 3 × GL_R8 planes; NV12/NV21 use GL_R8 + GL_RG8.
        const bool was_yuv420    = (impl_->tex_y != 0 &&
                                    impl_->current_yuv_fmt == pixel_format::yuv420);
        const bool is_yuv420     = (fmt == pixel_format::yuv420);
        const bool need_recreate = (impl_->tex_yuv_w != width  ||
                                    impl_->tex_yuv_h != height ||
                                    (was_yuv420 != is_yuv420));
        if (need_recreate) {
            if (impl_->tex_y)  { glDeleteTextures(1, &impl_->tex_y);  impl_->tex_y  = 0; }
            if (impl_->tex_uv) { glDeleteTextures(1, &impl_->tex_uv); impl_->tex_uv = 0; }
            if (impl_->tex_v)  { glDeleteTextures(1, &impl_->tex_v);  impl_->tex_v  = 0; }
        }

        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);

        const uint8_t* y_plane = data;
        if (fmt == pixel_format::yuv420) {
            // I420: contiguous Y | Cb | Cr planes, chroma half-size in both dims
            const uint8_t* u_plane = data + static_cast<std::size_t>(width) * height;
            const uint8_t* v_plane = u_plane + static_cast<std::size_t>(chroma_w) * chroma_h;
            upload_r8_plane(impl_->tex_y,  y_plane, width,    height);
            upload_r8_plane(impl_->tex_uv, u_plane, chroma_w, chroma_h);
            upload_r8_plane(impl_->tex_v,  v_plane, chroma_w, chroma_h);
        } else {
            // NV12 / NV21: contiguous Y | UV (or VU) interleaved chroma
            const uint8_t* uv_plane = data + static_cast<std::size_t>(width) * height;
            upload_r8_plane(impl_->tex_y,   y_plane,  width,    height);
            upload_rg8_plane(impl_->tex_uv, uv_plane, chroma_w, chroma_h);
        }

        if (need_recreate) {
            impl_->tex_yuv_w = width;
            impl_->tex_yuv_h = height;
        }
        impl_->current_yuv_fmt = fmt;

        // Draw with YUV conversion program
        const int yuv_mode = (fmt == pixel_format::yuv420) ? 0
                           : (fmt == pixel_format::nv12)   ? 1 : 2;
        glUseProgram(impl_->yuv_program);
        glUniform1i(impl_->loc_yuv_mode, yuv_mode);

        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, impl_->tex_y);
        glUniform1i(impl_->loc_yuv_tex_y, 0);

        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D, impl_->tex_uv);
        glUniform1i(impl_->loc_yuv_tex_u, 1);

        if (fmt == pixel_format::yuv420 && impl_->tex_v) {
            glActiveTexture(GL_TEXTURE2);
            glBindTexture(GL_TEXTURE_2D, impl_->tex_v);
            glUniform1i(impl_->loc_yuv_tex_v, 2);
        }

        glBindVertexArray(impl_->vao);
        glDrawArrays(GL_TRIANGLES, 0, 6);
        glBindVertexArray(0);
        glActiveTexture(GL_TEXTURE2); glBindTexture(GL_TEXTURE_2D, 0);
        glActiveTexture(GL_TEXTURE1); glBindTexture(GL_TEXTURE_2D, 0);
        glActiveTexture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_2D, 0);
        glUseProgram(0);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
        return;
    }

    // ── Single-plane path (rgba, rgb, bgra, grayscale) ───────────────────────
    GLenum gl_fmt;
    GLint  gl_internal;
    switch (fmt) {
        case pixel_format::rgba:
            gl_fmt = GL_RGBA; gl_internal = GL_RGBA8; break;
        case pixel_format::bgra:
            gl_fmt = GL_BGRA; gl_internal = GL_RGBA8; break;
        case pixel_format::rgb:
            gl_fmt = GL_RGB;  gl_internal = GL_RGB8;  break;
        case pixel_format::grayscale:
            gl_fmt = GL_RED;  gl_internal = GL_R8;    break;
        default:
            return;  // unreachable (YUV handled above)
    }

    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);

    const bool need_recreate = !impl_->tex
                             || impl_->tex_w      != width
                             || impl_->tex_h      != height
                             || impl_->current_fmt != fmt;
    if (need_recreate) {
        if (impl_->tex) glDeleteTextures(1, &impl_->tex);
        glGenTextures(1, &impl_->tex);
        glBindTexture(GL_TEXTURE_2D, impl_->tex);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S,     GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T,     GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        // Grayscale: swizzle R → RGB, alpha to 1.0
        if (fmt == pixel_format::grayscale) {
            const GLint swizzle[] = {GL_RED, GL_RED, GL_RED, GL_ONE};
            glTexParameteriv(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_RGBA, swizzle);
        }
        glTexImage2D(GL_TEXTURE_2D, 0, gl_internal, width, height, 0,
                     gl_fmt, GL_UNSIGNED_BYTE, data);
        impl_->tex_w       = width;
        impl_->tex_h       = height;
        impl_->has_tex     = true;
        impl_->current_fmt = fmt;
    } else {
        glBindTexture(GL_TEXTURE_2D, impl_->tex);
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, width, height,
                        gl_fmt, GL_UNSIGNED_BYTE, data);
    }

    // Draw fullscreen quad with the texture
    glUseProgram(impl_->program);
    glUniform1i(impl_->loc_has_tex, GL_TRUE);
    glActiveTexture(GL_TEXTURE0);
    glUniform1i(impl_->loc_tex, 0);
    glBindVertexArray(impl_->vao);
    glDrawArrays(GL_TRIANGLES, 0, 6);
    glBindVertexArray(0);
    glBindTexture(GL_TEXTURE_2D, 0);
    glUseProgram(0);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
}

// ── Pinhole implementation ────────────────────────────────────────────────────

Pinhole::Pinhole() : impl_(std::make_unique<Impl>()) {}
Pinhole::~Pinhole() = default;

Pinhole::Pinhole(Pinhole&&) noexcept = default;
Pinhole& Pinhole::operator=(Pinhole&&) noexcept = default;

const std::string& Pinhole::id() const { return impl_->id_; }
bool Pinhole::is_native() const        { return impl_->is_native_; }

void Pinhole::on_render(RenderFn fn) {
    std::lock_guard<std::mutex> lock(impl_->cb_mutex_);
    impl_->render_fn_ = std::move(fn);
}

void Pinhole::on_resize(std::function<void(int, int, double)> fn) {
    std::lock_guard<std::mutex> lock(impl_->cb_mutex_);
    impl_->resize_fn_ = std::move(fn);
}

void Pinhole::on_visibility(std::function<void(bool)> fn) {
    std::lock_guard<std::mutex> lock(impl_->cb_mutex_);
    impl_->visibility_fn_ = std::move(fn);
}

void Pinhole::request_redraw() {
    if (!impl_->is_native_) {
        // Fallback: copy the current render function and dispatch a CPU render
        // via g_idle_add.  Using shared_ptr captures means this is safe even
        // if the Pinhole is destroyed before the idle fires.
        if (!impl_->fb_state_) return;

        // Coalesce: if a redraw is already queued, drop this one.  The GL path
        // gets this for free via gtk_gl_area_queue_render(); the fallback path
        // would otherwise grow the GLib idle queue under fast producers.
        if (impl_->fb_state_->redraw_pending.exchange(true, std::memory_order_acq_rel)) {
            return;
        }

        Pinhole::RenderFn fn;
        {
            std::lock_guard<std::mutex> lock(impl_->cb_mutex_);
            fn = impl_->render_fn_;
        }
        if (!fn) {
            // No callback registered — release the coalesce flag.
            impl_->fb_state_->redraw_pending.store(false, std::memory_order_release);
            return;
        }
        struct Task {
            std::shared_ptr<FallbackState> state;
            Pinhole::RenderFn              fn;
        };
        auto* task = new Task{impl_->fb_state_, std::move(fn)};
        g_idle_add(+[](gpointer data) -> gboolean {
            auto* t = static_cast<Task*>(data);
            do_fallback_render(t->state, t->fn);
            delete t;
            return G_SOURCE_REMOVE;
        }, task);
        return;
    }
    if (!impl_->gl_area_) return;
    GtkGLArea* area = impl_->gl_area_;
    // gtk_gl_area_queue_render is not thread-safe — use g_idle_add
    g_idle_add(+[](gpointer data) -> gboolean {
        gtk_gl_area_queue_render(static_cast<GtkGLArea*>(data));
        return G_SOURCE_REMOVE;
    }, area);
}

void Pinhole::set_continuous(bool enabled) {
    impl_->opts_.continuous = enabled;
    if (!impl_->is_native_) {
        if (enabled) {
            g_warning("anyar::Pinhole '%s': set_continuous(true) is not "
                      "supported in canvas fallback mode.", impl_->id_.c_str());
        }
        return;
    }
    // Marshal tick install/remove to the main thread.
    struct Task { Impl* self; bool on; };
    auto* t = new Task{impl_.get(), enabled && impl_->window_active_};
    g_idle_add(+[](gpointer data) -> gboolean {
        auto* tk = static_cast<Task*>(data);
        if (tk->on) tk->self->install_tick_cb_if_needed();
        else        tk->self->remove_tick_cb();
        delete tk;
        return G_SOURCE_REMOVE;
    }, t);
}

void Pinhole::set_rect(int x_css, int y_css, int width_css, int height_css) {
    impl_->rect_x_ = x_css;
    impl_->rect_y_ = y_css;
    impl_->rect_w_ = width_css;
    impl_->rect_h_ = height_css;

    if (!impl_->is_native_) {
        // Fallback mode: (re)init canvas if dimensions changed.
        impl_->activate_fallback_canvas(width_css, height_css);
        return;
    }

    // GL path: notify GtkOverlay to reposition the GL area.
    if (impl_->overlay_) {
        GtkWidget* ov = GTK_WIDGET(impl_->overlay_);
        g_idle_add(+[](gpointer data) -> gboolean {
            gtk_widget_queue_resize(static_cast<GtkWidget*>(data));
            return G_SOURCE_REMOVE;
        }, ov);
    }
}

void Pinhole::set_visible(bool visible) {
    impl_->user_visible_ = visible;
    if (!impl_->gl_area_) return;
    // Fire user callback immediately (may be on any thread)
    {
        std::lock_guard<std::mutex> lock(impl_->cb_mutex_);
        if (impl_->visibility_fn_) {
            impl_->visibility_fn_(visible);
        }
    }
    // Fallback: toggle the injected canvas display via webview_eval.
    if (!impl_->is_native_) {
        if (impl_->fb_state_) {
            std::function<void(const std::string&)> eval;
            {
                std::lock_guard<std::mutex> lk(impl_->fb_state_->mu);
                eval = impl_->fb_state_->eval;
            }
            if (eval) {
                std::string js = "var _c=document.getElementById('__anyar_fb_";
                js += impl_->id_;
                js += "');if(_c)_c.style.display='";
                js += (visible ? "" : "none");
                js += "';";
                eval(js);
            }
        }
        return;  // don't show/hide the (invisible) failed GL area
    }
    // GL path: effective visibility honours OS window active state
    bool effective = visible && impl_->window_active_;
    struct Payload { GtkWidget* w; bool vis; };
    auto* p = new Payload{GTK_WIDGET(impl_->gl_area_), effective};
    g_idle_add(+[](gpointer data) -> gboolean {
        auto* pl = static_cast<Payload*>(data);
        if (pl->vis) gtk_widget_show(pl->w);
        else         gtk_widget_hide(pl->w);
        delete pl;
        return G_SOURCE_REMOVE;
    }, p);
}

void Pinhole::on_dom_detached(std::function<void()> fn) {
    std::lock_guard<std::mutex> lock(impl_->cb_mutex_);
    impl_->dom_detached_fn_ = std::move(fn);
}

void Pinhole::notify_dom_detached() {
    // Hide the overlay (user_visible_ ← false; no user visibility callback intentional).
    set_visible(false);
    // Fire the C++ dom-detached callback synchronously.
    std::function<void()> fn;
    {
        std::lock_guard<std::mutex> lock(impl_->cb_mutex_);
        fn = impl_->dom_detached_fn_;
    }
    if (fn) fn();
}

void Pinhole::notify_window_destroyed() {
    impl_->destroyed_.store(true, std::memory_order_release);
    impl_->gl_area_ = nullptr;
    impl_->overlay_ = nullptr;
    impl_->sig_child_pos_ = 0;
    impl_->tick_id_ = 0;
    impl_->window_active_ = false;
    impl_->reorder_fn_ = nullptr;

    if (impl_->fb_state_) {
        std::lock_guard<std::mutex> lk(impl_->fb_state_->mu);
        impl_->fb_state_->eval = nullptr;
    }
}

int Pinhole::z_index() const { return impl_->z_index_; }

void Pinhole::set_z_index(int z) {
    impl_->z_index_ = z;
    if (impl_->reorder_fn_) impl_->reorder_fn_();
}

void Pinhole::set_reorder_callback(std::function<void()> fn) {
    impl_->reorder_fn_ = std::move(fn);
}

void Pinhole::reorder_in_overlay() {
    // Must be called on the GTK main thread (dispatched via g_idle_add).
    if (!impl_->gl_area_ || !impl_->overlay_) return;
    GtkWidget* w = GTK_WIDGET(impl_->gl_area_);
    g_object_ref(w);
    gtk_container_remove(GTK_CONTAINER(impl_->overlay_), w);
    gtk_overlay_add_overlay(impl_->overlay_, w);
    gtk_overlay_set_overlay_pass_through(impl_->overlay_, w, TRUE);
    // Restore effective visibility after re-insertion.
    if (impl_->user_visible_ && impl_->window_active_)
        gtk_widget_show(w);
    else
        gtk_widget_hide(w);
    g_object_unref(w);
}

void Pinhole::set_window_active(bool active) {
    impl_->window_active_ = active;
    if (!impl_->gl_area_) return;
    GtkGLArea* area    = impl_->gl_area_;
    bool effective_vis = active && impl_->user_visible_;
    bool tick_on       = active && impl_->opts_.continuous;
    struct Payload { GtkWidget* w; Impl* self; bool vis; bool tick_on; };
    auto* p = new Payload{GTK_WIDGET(area), impl_.get(), effective_vis, tick_on};
    g_idle_add(+[](gpointer data) -> gboolean {
        auto* pl = static_cast<Payload*>(data);
        if (pl->vis) gtk_widget_show(pl->w);
        else         gtk_widget_hide(pl->w);
        // Pause the per-frame tick when window is inactive/minimised
        // to avoid wasted GPU cycles.
        if (pl->tick_on) pl->self->install_tick_cb_if_needed();
        else             pl->self->remove_tick_cb();
        delete pl;
        return G_SOURCE_REMOVE;
    }, p);
}

// ── tracking_js() ─────────────────────────────────────────────────────────────
// Self-contained JS bootstrap injected via webview_init once per window.
// Scans for [data-anyar-pinhole] elements, tracks their CSS-pixel rects via
// ResizeObserver, hides during scroll, shows on idle after scrollend.
// Sends IPC commands: pinhole:update_rect, pinhole:set_visible.
// Deliberately avoids ES module syntax — must run in plain webview JS context.

/* static */ std::string Pinhole::tracking_js() {
    return R"js(
(function () {
  if (typeof window.__anyar_pinhole_init__ !== 'undefined') return;
  window.__anyar_pinhole_init__ = true;

  var _tracked = {};   // id → {ro, io, scrollTimer}
  var _wlabel  = (window.__LIBANYAR_WINDOW_LABEL__ || 'main');

  function _ipc(cmd, args) {
    if (typeof window.__anyar_ipc__ !== 'function') return;
    args.window_label = _wlabel;
    window.__anyar_ipc__(JSON.stringify({
      id: 'ph_' + cmd + '_' + Date.now(),
      cmd: cmd,
      args: args
    })).catch(function(){});
  }

  function sendRect(id, el) {
    var r = el.getBoundingClientRect();
    _ipc('pinhole:update_rect', {
      id: id,
      x: Math.round(r.left),
      y: Math.round(r.top),
      width: Math.round(r.width),
      height: Math.round(r.height),
      dpr: window.devicePixelRatio || 1
    });
  }

  function sendVisible(id, visible) {
    _ipc('pinhole:set_visible', { id: id, visible: visible });
  }

  // Detach a tracked pinhole: clean up observers and signal C++.
  function _detachTracked(id) {
    if (!_tracked[id]) return;
    _ipc('pinhole:dom_detached', { id: id });
    _tracked[id].ro.disconnect();
    _tracked[id].io.disconnect();
    window.removeEventListener('scroll', _tracked[id].onScroll, { capture: true });
    delete _tracked[id];
  }

  // Best-effort: if a pinhole is covered by a higher-z sibling, hide it.
  // Only signals hide; the IntersectionObserver / scroll protocol re-shows.
  function checkZSiblings() {
    var ids = Object.keys(_tracked);
    if (ids.length < 2) return;
    var rects = {}, zidx = {};
    ids.forEach(function(id) {
      var el = document.querySelector('[data-anyar-pinhole="' + id + '"]');
      if (!el) return;
      rects[id] = el.getBoundingClientRect();
      zidx[id]  = parseInt(window.getComputedStyle(el).zIndex) || 0;
    });
    ids.forEach(function(id_a) {
      if (!rects[id_a]) return;
      var covered = ids.some(function(id_b) {
        if (id_a === id_b || !rects[id_b] || zidx[id_b] <= zidx[id_a]) return false;
        var a = rects[id_a], b = rects[id_b];
        return !(a.right <= b.left || b.right <= a.left ||
                 a.bottom <= b.top || b.bottom <= a.top);
      });
      if (covered) sendVisible(id_a, false);
    });
  }

  function setupElement(id, el) {
    if (_tracked[id]) return;

    var scrollTimer = null;
    var isVisible   = true;

    // ResizeObserver: position + size changes (including scroll reflow)
    var ro = new ResizeObserver(function () {
      if (isVisible) sendRect(id, el);
    });
    ro.observe(el);

    // IntersectionObserver: handles display:none, scroll out-of-view, off-screen
    var io = new IntersectionObserver(function (entries) {
      entries.forEach(function (e) {
        isVisible = e.isIntersecting;
        sendVisible(id, isVisible);
        if (isVisible) sendRect(id, el);
      });
    }, { threshold: 0 });
    io.observe(el);

    // Scroll-hide on any ancestor scroll
    function onScroll() {
      if (isVisible) sendVisible(id, false);
      clearTimeout(scrollTimer);
      scrollTimer = setTimeout(function () {
        requestAnimationFrame(function () {
          sendRect(id, el);
          sendVisible(id, true);
          isVisible = true;
        });
      }, 100);
    }
    window.addEventListener('scroll', onScroll, { passive: true, capture: true });

    // Initial rect after first layout
    requestAnimationFrame(function () { sendRect(id, el); });

    _tracked[id] = { ro: ro, io: io, onScroll: onScroll };

    // Check if a higher-z sibling already covers this newly registered element
    checkZSiblings();
  }

  function scanDOM() {
    var els = document.querySelectorAll('[data-anyar-pinhole]');
    for (var i = 0; i < els.length; i++) {
      var id = els[i].getAttribute('data-anyar-pinhole');
      if (id) setupElement(id, els[i]);
    }
  }

  // MutationObserver: track DOM additions and removals
  var mo = new MutationObserver(function (mutations) {
    mutations.forEach(function (m) {
      // Removed nodes → signal dom_detached, clean up observers
      m.removedNodes.forEach(function (n) {
        if (n.nodeType !== 1) return;
        var rid = n.getAttribute && n.getAttribute('data-anyar-pinhole');
        if (rid) _detachTracked(rid);
        var rnest = n.querySelectorAll && n.querySelectorAll('[data-anyar-pinhole]');
        if (rnest) for (var i = 0; i < rnest.length; i++) {
          var rnid = rnest[i].getAttribute('data-anyar-pinhole');
          if (rnid) _detachTracked(rnid);
        }
      });
      // Added nodes → set up tracking
      m.addedNodes.forEach(function (n) {
        if (n.nodeType !== 1) return;
        var id = n.getAttribute && n.getAttribute('data-anyar-pinhole');
        if (id) setupElement(id, n);
        var nested = n.querySelectorAll && n.querySelectorAll('[data-anyar-pinhole]');
        if (nested) {
          for (var i = 0; i < nested.length; i++) {
            var nid = nested[i].getAttribute('data-anyar-pinhole');
            if (nid) setupElement(nid, nested[i]);
          }
        }
      });
    });
  });

  function boot() {
    scanDOM();
    mo.observe(document.documentElement, { childList: true, subtree: true });
    // Re-scan on navigation (SPA hash/history changes)
    window.addEventListener('popstate', scanDOM);
    window.addEventListener('hashchange', scanDOM);
  }

  if (document.readyState === 'loading') {
    document.addEventListener('DOMContentLoaded', boot);
  } else {
    boot();
  }
})();
)js";
}

// ── init_from_window (Bridge: Window → Pinhole::Impl) ─────────────────────────
// Implemented as Pinhole::Impl::init_from_window() above (same TU).
// The Window::create_pinhole() free function calls it by passing the overlay
// pointer extracted from Window::Impl.

void Pinhole::platform_init(const std::string& id, const PinholeOptions& opts,
                             void* overlay,
                             std::function<void(const std::string&)> eval_fn)
{
    impl_->init_from_window(id, opts,
                             static_cast<GtkOverlay*>(overlay),
                             std::move(eval_fn));
}

void Pinhole::override_eval_fn_for_test(std::function<void(const std::string&)> fn)
{
    if (!impl_->fb_state_) {
        impl_->fb_state_     = std::make_shared<FallbackState>();
        impl_->fb_state_->id = impl_->id_;
    }
    std::lock_guard<std::mutex> lk(impl_->fb_state_->mu);
    impl_->fb_state_->eval = std::move(fn);
}

} // namespace anyar

#endif // __linux__
