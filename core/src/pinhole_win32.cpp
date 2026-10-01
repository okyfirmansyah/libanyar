/// @file pinhole_win32.cpp
/// @brief Pinhole native overlay — Windows (DirectComposition + D3D11).
///
/// Composition (ADR-012):
///
///   top-level HWND
///     └─ "webview_widget" HWND  ← DComp target (topmost = FALSE) → root visual
///          │                        ├─ pinhole visual (swap chain)  z ascending
///          │                        └─ …
///          └─ WebView2 child HWND (transparent background, drawn ABOVE)
///
/// HTML therefore composites over the native surfaces exactly like the Linux
/// GtkOverlay layering.  No CoreWebView2CompositionController (visual
/// hosting) migration is needed.
///
/// Threading: every D3D11 / DComp call runs on the UI thread.  Work posted
/// from other threads captures a weak_ptr to the pinhole's State, so a
/// destroyed pinhole is never touched.
///
/// Fallback (force_fallback, or no D3D11/DComp): CPU render into a
/// SharedBuffer + an injected 2D <canvas>, fetched over HTTP.

#ifdef _WIN32

#include <anyar/pinhole.h>
#include <anyar/frame_mailbox.h>
#include <anyar/main_thread.h>
#include <anyar/shared_buffer.h>

#include "pinhole_cpu.h"
#include "pinhole_win32.h"
#include "platform.h"
#include "win32_util.h"

#include <objbase.h>
#include <WebView2.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <dcomp.h>
#include <dxgi1_2.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <iostream>
#include <map>
#include <mutex>
#include <vector>

namespace anyar {

using win32::ComPtr;

namespace {

void log_warn(const std::string& msg) {
    std::cerr << "[LibAnyar] Pinhole: " << msg << std::endl;
}

// ── Shaders ──────────────────────────────────────────────────────────────────
// Full-surface quad from SV_VertexID (triangle strip, no vertex buffer).
// Modes: 0 rgba/bgra (format-aware sampling), 1 grayscale (R8),
// 2 yuv420 (Y, U, V planes), 3 nv12 (Y + UV), 4 nv21 (Y + VU).
// YUV: BT.601 full range — same constants as the GLSL / CPU paths.

constexpr const char* kShaderSrc = R"hlsl(
cbuffer Params : register(b0) { int mode; int3 pad; };
Texture2D t0 : register(t0);
Texture2D t1 : register(t1);
Texture2D t2 : register(t2);
SamplerState s0 : register(s0);

struct VSOut { float4 pos : SV_Position; float2 uv : TEXCOORD0; };

// Strip order TL, TR, BL, BR (clockwise); p is in image space (y down).
VSOut vs_main(uint id : SV_VertexID) {
    float2 p = float2((id & 1) ? 1.0 : 0.0, (id & 2) ? 1.0 : 0.0);
    VSOut o;
    o.pos = float4(p.x * 2.0 - 1.0, 1.0 - p.y * 2.0, 0.0, 1.0);
    o.uv  = p;
    return o;
}

float3 yuv2rgb(float y, float u, float v) {
    u -= 0.5; v -= 0.5;
    return saturate(float3(y + 1.40200 * v,
                           y - 0.34414 * u - 0.71414 * v,
                           y + 1.77200 * u));
}

float4 ps_main(VSOut i) : SV_Target {
    // The swap chain is premultiplied-alpha; images arrive straight-alpha.
    if (mode == 0) { float4 c = t0.Sample(s0, i.uv); return float4(c.rgb * c.a, c.a); }
    if (mode == 1) { float g = t0.Sample(s0, i.uv).r; return float4(g, g, g, 1.0); }
    float y = t0.Sample(s0, i.uv).r;
    if (mode == 2) return float4(yuv2rgb(y, t1.Sample(s0, i.uv).r, t2.Sample(s0, i.uv).r), 1.0);
    float2 c = t1.Sample(s0, i.uv).rg;
    if (mode == 3) return float4(yuv2rgb(y, c.x, c.y), 1.0);
    return float4(yuv2rgb(y, c.y, c.x), 1.0);
}
)hlsl";

struct ShaderParams {
    int32_t mode;
    int32_t pad[3];
};

/// A dynamic texture plane re-created only when size/format change.
struct Plane {
    ComPtr<ID3D11Texture2D> tex;
    ComPtr<ID3D11ShaderResourceView> srv;
    int w = 0, h = 0;
    DXGI_FORMAT fmt = DXGI_FORMAT_UNKNOWN;

    void reset() { srv.reset(); tex.reset(); w = h = 0; fmt = DXGI_FORMAT_UNKNOWN; }

    bool ensure(ID3D11Device* dev, int width, int height, DXGI_FORMAT format) {
        if (tex && w == width && h == height && fmt == format) return true;
        reset();
        D3D11_TEXTURE2D_DESC d{};
        d.Width = static_cast<UINT>(width);
        d.Height = static_cast<UINT>(height);
        d.MipLevels = 1;
        d.ArraySize = 1;
        d.Format = format;
        d.SampleDesc.Count = 1;
        d.Usage = D3D11_USAGE_DYNAMIC;
        d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        d.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        if (FAILED(dev->CreateTexture2D(&d, nullptr, tex.put())) ||
            FAILED(dev->CreateShaderResourceView(tex.get(), nullptr, srv.put()))) {
            reset();
            return false;
        }
        w = width;
        h = height;
        fmt = format;
        return true;
    }

    /// Copy @p rows rows of @p row_bytes each (tightly packed source).
    void upload(ID3D11DeviceContext* ctx, const uint8_t* src, size_t row_bytes, int rows) {
        D3D11_MAPPED_SUBRESOURCE m{};
        if (FAILED(ctx->Map(tex.get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) return;
        auto* dst = static_cast<uint8_t*>(m.pData);
        if (m.RowPitch == row_bytes) {
            std::memcpy(dst, src, row_bytes * static_cast<size_t>(rows));
        } else {
            for (int r = 0; r < rows; ++r) {
                std::memcpy(dst + static_cast<size_t>(r) * m.RowPitch,
                            src + static_cast<size_t>(r) * row_bytes, row_bytes);
            }
        }
        ctx->Unmap(tex.get(), 0);
    }
};

} // namespace

struct PinholeState;

// ── PinholeHost (one per window) ─────────────────────────────────────────────

struct PinholeHost {
    HWND widget = nullptr;
    bool alive = true;  // false once the window is destroyed (UI thread)

    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> ctx;
    ComPtr<IDXGIFactory2> factory;
    ComPtr<IDCompositionDevice> dcomp;
    ComPtr<IDCompositionTarget> target;
    ComPtr<IDCompositionVisual> root;

    ComPtr<ID3D11VertexShader> vs;
    ComPtr<ID3D11PixelShader> ps;
    ComPtr<ID3D11Buffer> params;
    ComPtr<ID3D11SamplerState> sampler;
    ComPtr<ID3D11RasterizerState> raster;   // no culling
    ComPtr<ID3D11BlendState> blend;         // opaque overwrite

    std::vector<std::weak_ptr<PinholeState>> pins;  // UI thread

    double dpr() const {
        UINT dpi = widget ? GetDpiForWindow(widget) : USER_DEFAULT_SCREEN_DPI;
        return dpi / static_cast<double>(USER_DEFAULT_SCREEN_DPI);
    }

    void commit() {
        if (dcomp) dcomp->Commit();
    }

    void restack();  // defined after PinholeState
};

// ── Per-pinhole state (shared with posted UI-thread work) ────────────────────

struct PinholeState {
    std::string id;
    PinholeOptions opts;
    bool native = false;
    std::atomic<bool> dead{false};          // pinhole or window gone
    std::atomic<bool> redraw_pending{false};
    std::atomic<int> z{0};
    uint64_t order = 0;                     // creation order (z ties)

    // Callbacks (cb_mu)
    std::mutex cb_mu;
    Pinhole::RenderFn render_fn;
    std::function<void(int, int, double)> resize_fn;
    std::function<void(bool)> visibility_fn;
    std::function<void()> dom_detached_fn;
    std::function<void()> reorder_fn;

    // Geometry / flags (UI thread)
    int x_css = 0, y_css = 0, w_css = 0, h_css = 0;
    int w_px = 0, h_px = 0;
    double dpr = 1.0;
    bool user_visible = true;
    bool window_active = true;
    bool continuous = false;
    UINT_PTR timer = 0;

    // Native resources (UI thread)
    std::shared_ptr<PinholeHost> host;
    ComPtr<IDCompositionVisual> visual;
    ComPtr<IDXGISwapChain1> swapchain;
    ComPtr<ID3D11RenderTargetView> rtv;
    Plane planes[3];
    std::vector<uint8_t> scratch;  // rgb → rgba expansion

    // Canvas fallback (fb_mu)
    std::mutex fb_mu;
    std::function<void(const std::string&)> eval;
    std::shared_ptr<SharedBuffer> fb_buf;
    int fb_w = 0, fb_h = 0;
    bool fb_injected = false;

    bool showing() const {
        return native && !dead && host && host->alive && visual && user_visible &&
               w_px > 0 && h_px > 0;
    }

    /// UI thread: drop every native resource (window or pinhole gone).
    void release_native() {
        if (timer) {
            KillTimer(nullptr, timer);
            timer = 0;
        }
        for (auto& p : planes) p.reset();
        rtv.reset();
        if (visual) visual->SetContent(nullptr);
        swapchain.reset();
        visual.reset();
    }
};

void PinholeHost::restack() {
    if (!alive || !root) return;
    std::vector<std::shared_ptr<PinholeState>> live;
    pins.erase(std::remove_if(pins.begin(), pins.end(),
                              [&](const std::weak_ptr<PinholeState>& w) {
                                  auto s = w.lock();
                                  if (!s || s->dead) return true;
                                  if (s->showing()) live.push_back(std::move(s));
                                  return false;
                              }),
               pins.end());
    std::stable_sort(live.begin(), live.end(), [](const auto& a, const auto& b) {
        int za = a->z.load(), zb = b->z.load();
        return za != zb ? za < zb : a->order < b->order;
    });
    root->RemoveAllVisuals();
    for (auto& s : live) {
        // referenceVisual == nullptr: insertAbove FALSE places the visual on
        // TOP of its siblings (TRUE would place it at the bottom), so adding
        // in ascending z leaves the highest z on top.
        root->AddVisual(s->visual.get(), FALSE, nullptr);
    }
    commit();
}

std::shared_ptr<PinholeHost> create_pinhole_host(void* widget_hwnd, void* controller) {
    auto host = std::make_shared<PinholeHost>();
    host->widget = static_cast<HWND>(widget_hwnd);
    if (!host->widget) return nullptr;

    // Transparent webview background: pages without a background now show
    // what is composed below them (the pinholes) — matches Linux.
    if (auto* ctl = static_cast<ICoreWebView2Controller*>(controller)) {
        ComPtr<ICoreWebView2Controller2> ctl2;
        if (SUCCEEDED(ctl->QueryInterface(IID_PPV_ARGS(ctl2.put())))) {
            COREWEBVIEW2_COLOR transparent{0, 0, 0, 0};
            ctl2->put_DefaultBackgroundColor(transparent);
        }
    }

    HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
                                   D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0,
                                   D3D11_SDK_VERSION, host->device.put(), nullptr,
                                   host->ctx.put());
    if (FAILED(hr)) {
        // WARP keeps "native" working on GPU-less machines / RDP.
        hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr,
                               D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0,
                               D3D11_SDK_VERSION, host->device.put(), nullptr,
                               host->ctx.put());
    }
    if (FAILED(hr)) {
        log_warn("D3D11 unavailable (" + win32::error_message(static_cast<DWORD>(hr)) +
                 ") — canvas fallback");
        return nullptr;
    }

    ComPtr<IDXGIDevice> dxgi;
    ComPtr<IDXGIAdapter> adapter;
    if (FAILED(host->device->QueryInterface(IID_PPV_ARGS(dxgi.put()))) ||
        FAILED(dxgi->GetAdapter(adapter.put())) ||
        FAILED(adapter->GetParent(IID_PPV_ARGS(host->factory.put()))) ||
        FAILED(DCompositionCreateDevice(dxgi.get(), IID_PPV_ARGS(host->dcomp.put()))) ||
        FAILED(host->dcomp->CreateTargetForHwnd(host->widget, FALSE, host->target.put())) ||
        FAILED(host->dcomp->CreateVisual(host->root.put())) ||
        FAILED(host->target->SetRoot(host->root.get()))) {
        log_warn("DirectComposition unavailable — canvas fallback");
        return nullptr;
    }

    // Pipeline
    ComPtr<ID3DBlob> vsb, psb, err;
    hr = D3DCompile(kShaderSrc, std::strlen(kShaderSrc), "pinhole", nullptr, nullptr,
                    "vs_main", "vs_4_0", 0, 0, vsb.put(), err.put());
    if (SUCCEEDED(hr)) {
        hr = D3DCompile(kShaderSrc, std::strlen(kShaderSrc), "pinhole", nullptr, nullptr,
                        "ps_main", "ps_4_0", 0, 0, psb.put(), err.put());
    }
    if (FAILED(hr)) {
        std::string msg = err ? std::string(static_cast<const char*>(err->GetBufferPointer()),
                                            err->GetBufferSize())
                              : win32::error_message(static_cast<DWORD>(hr));
        log_warn("shader compile failed: " + msg + " — canvas fallback");
        return nullptr;
    }
    host->device->CreateVertexShader(vsb->GetBufferPointer(), vsb->GetBufferSize(), nullptr,
                                     host->vs.put());
    host->device->CreatePixelShader(psb->GetBufferPointer(), psb->GetBufferSize(), nullptr,
                                    host->ps.put());

    D3D11_BUFFER_DESC bd{};
    bd.ByteWidth = sizeof(ShaderParams);
    bd.Usage = D3D11_USAGE_DEFAULT;
    bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    host->device->CreateBuffer(&bd, nullptr, host->params.put());

    D3D11_SAMPLER_DESC sd{};
    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.MaxLOD = D3D11_FLOAT32_MAX;
    host->device->CreateSamplerState(&sd, host->sampler.put());

    D3D11_RASTERIZER_DESC rd{};
    rd.FillMode = D3D11_FILL_SOLID;
    rd.CullMode = D3D11_CULL_NONE;
    rd.DepthClipEnable = TRUE;
    host->device->CreateRasterizerState(&rd, host->raster.put());

    D3D11_BLEND_DESC bld{};
    bld.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    host->device->CreateBlendState(&bld, host->blend.put());

    if (!host->vs || !host->ps || !host->params || !host->sampler || !host->raster ||
        !host->blend) {
        log_warn("D3D11 pipeline creation failed — canvas fallback");
        return nullptr;
    }
    host->commit();
    return host;
}

void pinhole_host_window_destroyed(PinholeHost& host) {
    host.alive = false;
    for (auto& w : host.pins) {
        if (auto s = w.lock()) {
            s->dead = true;
            s->release_native();
        }
    }
    host.pins.clear();
    if (host.root) host.root->RemoveAllVisuals();
    host.root.reset();
    host.target.reset();
    if (host.dcomp) host.dcomp->Commit();
    host.dcomp.reset();
}

// ── PinholeRenderContext::Impl ──────────────────────────────────────────────

struct PinholeRenderContext::Impl {
    int width_px = 0;
    int height_px = 0;
    double dpr = 1.0;

    // Native mode
    PinholeState* state = nullptr;
    D3D11_VIEWPORT viewport{};

    // CPU fallback mode
    bool cpu_mode = false;
    uint8_t* cpu_rgba = nullptr;
};

// ── Pinhole::Impl ───────────────────────────────────────────────────────────

struct Pinhole::Impl {
    std::shared_ptr<PinholeState> s = std::make_shared<PinholeState>();

    static void invoke(const RenderFn& fn, PinholeRenderContext::Impl* ci) {
        PinholeRenderContext ctx(ci);
        fn(ctx);
    }
};

namespace {

std::mutex g_timer_mu;
std::map<UINT_PTR, std::weak_ptr<PinholeState>> g_timers;

void render_native(PinholeState& s);

void CALLBACK continuous_tick(HWND, UINT, UINT_PTR id, DWORD) {
    std::shared_ptr<PinholeState> s;
    {
        std::lock_guard<std::mutex> lk(g_timer_mu);
        auto it = g_timers.find(id);
        if (it != g_timers.end()) s = it->second.lock();
    }
    if (s) render_native(*s);
}

/// UI thread: (re)create the swap chain for the current pixel size.
bool ensure_swapchain(PinholeState& s) {
    PinholeHost& h = *s.host;
    if (!s.visual) {
        if (FAILED(h.dcomp->CreateVisual(s.visual.put()))) return false;
    }
    if (!s.swapchain) {
        DXGI_SWAP_CHAIN_DESC1 d{};
        d.Width = static_cast<UINT>(s.w_px);
        d.Height = static_cast<UINT>(s.h_px);
        d.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        d.SampleDesc.Count = 1;
        d.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        d.BufferCount = 2;
        d.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
        d.AlphaMode = DXGI_ALPHA_MODE_PREMULTIPLIED;
        if (FAILED(h.factory->CreateSwapChainForComposition(h.device.get(), &d, nullptr,
                                                            s.swapchain.put()))) {
            return false;
        }
        s.visual->SetContent(s.swapchain.get());
    } else {
        DXGI_SWAP_CHAIN_DESC1 cur{};
        s.swapchain->GetDesc1(&cur);
        if (cur.Width != static_cast<UINT>(s.w_px) || cur.Height != static_cast<UINT>(s.h_px)) {
            s.rtv.reset();
            h.ctx->OMSetRenderTargets(0, nullptr, nullptr);
            if (FAILED(s.swapchain->ResizeBuffers(0, static_cast<UINT>(s.w_px),
                                                  static_cast<UINT>(s.h_px),
                                                  DXGI_FORMAT_UNKNOWN, 0))) {
                return false;
            }
        }
    }
    if (!s.rtv) {
        ComPtr<ID3D11Texture2D> back;
        if (FAILED(s.swapchain->GetBuffer(0, IID_PPV_ARGS(back.put()))) ||
            FAILED(h.device->CreateRenderTargetView(back.get(), nullptr, s.rtv.put()))) {
            return false;
        }
    }
    return true;
}

/// UI thread: apply the CSS rect + current DPI to the visual / swap chain.
void apply_geometry(PinholeState& s) {
    if (!s.native || s.dead || !s.host || !s.host->alive) return;
    const double dpr = s.host->dpr();
    const int w_px = std::max(0, static_cast<int>(std::lround(s.w_css * dpr)));
    const int h_px = std::max(0, static_cast<int>(std::lround(s.h_css * dpr)));
    const bool resized = (w_px != s.w_px || h_px != s.h_px || dpr != s.dpr);
    s.w_px = w_px;
    s.h_px = h_px;
    s.dpr = dpr;

    if (w_px > 0 && h_px > 0) {
        if (!ensure_swapchain(s)) {
            log_warn("'" + s.id + "': swap chain setup failed");
            return;
        }
        s.visual->SetOffsetX(static_cast<float>(std::lround(s.x_css * dpr)));
        s.visual->SetOffsetY(static_cast<float>(std::lround(s.y_css * dpr)));
    }
    s.host->restack();  // shows/hides (zero size), commits

    if (resized) {
        std::function<void(int, int, double)> cb;
        {
            std::lock_guard<std::mutex> lk(s.cb_mu);
            cb = s.resize_fn;
        }
        if (cb) cb(w_px, h_px, dpr);
    }
}

void render_native(PinholeState& s) {
    s.redraw_pending.store(false);
    if (!s.window_active || !s.showing()) return;
    if (s.host->dpr() != s.dpr) apply_geometry(s);  // moved to another monitor
    if (!s.rtv) return;

    Pinhole::RenderFn fn;
    {
        std::lock_guard<std::mutex> lk(s.cb_mu);
        fn = s.render_fn;
    }
    if (!fn) return;

    ID3D11DeviceContext* ctx = s.host->ctx.get();
    ID3D11RenderTargetView* rtv = s.rtv.get();
    ctx->OMSetRenderTargets(1, &rtv, nullptr);

    PinholeRenderContext::Impl ci{};
    ci.width_px = s.w_px;
    ci.height_px = s.h_px;
    ci.dpr = s.dpr;
    ci.state = &s;
    ci.viewport = D3D11_VIEWPORT{0.f, 0.f, static_cast<float>(s.w_px),
                                 static_cast<float>(s.h_px), 0.f, 1.f};
    ctx->RSSetViewports(1, &ci.viewport);
    try {
        Pinhole::Impl::invoke(fn, &ci);
    } catch (const std::exception& e) {
        log_warn("'" + s.id + "': render callback threw: " + e.what() + " — frame dropped");
        ctx->OMSetRenderTargets(0, nullptr, nullptr);
        return;
    } catch (...) {
        log_warn("'" + s.id + "': render callback threw — frame dropped");
        ctx->OMSetRenderTargets(0, nullptr, nullptr);
        return;
    }
    ctx->OMSetRenderTargets(0, nullptr, nullptr);
    // Sync interval 0: never block the UI thread on vsync; DComp shows the
    // latest presented buffer at the next composition.
    s.swapchain->Present(0, 0);
}

// ── Canvas fallback ──────────────────────────────────────────────────────────

/// Allocate/resize the RGBA buffer and inject/update the page canvas.
void activate_fallback_canvas(PinholeState& s, int w, int h) {
    if (w <= 0 || h <= 0) return;
    std::function<void(const std::string&)> eval;
    bool need_js = false;
    {
        std::lock_guard<std::mutex> lk(s.fb_mu);
        if (!s.eval) return;
        if (s.fb_w != w || s.fb_h != h || !s.fb_buf) {
            if (s.fb_buf) {
                SharedBufferRegistry::instance().remove(s.fb_buf->name());
                s.fb_buf.reset();
            }
            s.fb_buf = SharedBuffer::create("__anyar_fb_" + s.id,
                                            static_cast<size_t>(w) * h * 4);
            s.fb_w = w;
            s.fb_h = h;
            need_js = true;
        } else if (!s.fb_injected) {
            need_js = true;
        }
        eval = s.eval;
    }
    if (!need_js) return;

    // Same canvas protocol as Linux, but frames come over HTTP (no
    // anyar-shm:// on WebView2); same origin as the page.
    std::string js;
    js += "(function(){";
    js += "if(!window.__anyar_pb_renderers){";
    js += "window.__anyar_pb_renderers={};";
    js += "window.__anyar_pb_frame=function(id){";
    js += "var info=window.__anyar_pb_renderers[id];";
    js += "if(!info||!info.canvas)return;";
    js += "fetch('/__anyar__/buffer/__anyar_fb_'+encodeURIComponent(id))";
    js += ".then(function(r){return r.arrayBuffer();})";
    js += ".then(function(buf){";
    js += "info.ctx.putImageData(new ImageData(new Uint8ClampedArray(buf),info.w,info.h),0,0);";
    js += "}).catch(function(){});};";
    js += "}";
    js += "var id='" + s.id + "',w=" + std::to_string(w) + ",h=" + std::to_string(h) + ";";
    js += "var div=document.querySelector('[data-anyar-pinhole=\"'+id+'\"]');";
    js += "if(!div)return;";
    js += "var cv=document.getElementById('__anyar_fb_'+id);";
    js += "if(!cv){cv=document.createElement('canvas');";
    js += "cv.id='__anyar_fb_'+id;";
    js += "cv.style.cssText='position:absolute;top:0;left:0;width:100%;height:100%;pointer-events:none;';";
    js += "if(getComputedStyle(div).position==='static')div.style.position='relative';";
    js += "div.appendChild(cv);";
    js += "console.warn('[anyar] Pinhole \"'+id+'\": native overlay unavailable, using 2D canvas fallback.');";
    js += "}";
    js += "cv.width=w;cv.height=h;";
    js += "window.__anyar_pb_renderers[id]={canvas:cv,ctx:cv.getContext('2d'),w:w,h:h};";
    js += "})();";
    {
        std::lock_guard<std::mutex> lk(s.fb_mu);
        s.fb_injected = true;
    }
    eval(js);
}

void render_fallback(PinholeState& s) {
    s.redraw_pending.store(false);
    if (s.dead) return;
    Pinhole::RenderFn fn;
    {
        std::lock_guard<std::mutex> lk(s.cb_mu);
        fn = s.render_fn;
    }
    std::shared_ptr<SharedBuffer> buf;
    std::function<void(const std::string&)> eval;
    int w, h;
    {
        std::lock_guard<std::mutex> lk(s.fb_mu);
        buf = s.fb_buf;
        eval = s.eval;
        w = s.fb_w;
        h = s.fb_h;
    }
    if (!fn || !buf || !eval || w <= 0 || h <= 0) return;

    std::memset(buf->data(), 0, static_cast<size_t>(w) * h * 4);
    PinholeRenderContext::Impl ci{};
    ci.width_px = w;
    ci.height_px = h;
    ci.dpr = 1.0;
    ci.cpu_mode = true;
    ci.cpu_rgba = buf->data();
    try {
        Pinhole::Impl::invoke(fn, &ci);
    } catch (const std::exception& e) {
        log_warn("'" + s.id + "': fallback render callback threw: " + e.what() +
                 " — frame dropped");
        return;
    } catch (...) {
        log_warn("'" + s.id + "': fallback render callback threw — frame dropped");
        return;
    }
    if (!s.dead) eval("if(window.__anyar_pb_frame)window.__anyar_pb_frame('" + s.id + "')");
}

/// Run @p fn(state) on the UI thread if the pinhole is still alive.
template <typename F>
void post_ui(const std::shared_ptr<PinholeState>& s, F&& fn) {
    std::weak_ptr<PinholeState> weak = s;
    auto task = [weak, fn = std::forward<F>(fn)]() mutable {
        if (auto st = weak.lock()) {
            if (!st->dead) fn(*st);
        }
    };
    if (platform::is_main_thread()) {
        task();
    } else {
        post_to_main_thread(std::move(task));
    }
}

void schedule_redraw(const std::shared_ptr<PinholeState>& s) {
    if (s->dead || s->redraw_pending.exchange(true)) return;  // coalesce
    std::weak_ptr<PinholeState> weak = s;
    post_to_main_thread([weak] {  // always deferred: never re-enter on_render
        auto st = weak.lock();
        if (!st) return;
        if (st->native) render_native(*st);
        else render_fallback(*st);
    });
}

void update_timer(PinholeState& s, const std::shared_ptr<PinholeState>& self) {
    const bool want = s.native && s.continuous && s.window_active && !s.dead;
    if (want && !s.timer) {
        s.timer = SetTimer(nullptr, 0, 16, continuous_tick);
        std::lock_guard<std::mutex> lk(g_timer_mu);
        g_timers[s.timer] = self;
    } else if (!want && s.timer) {
        KillTimer(nullptr, s.timer);
        std::lock_guard<std::mutex> lk(g_timer_mu);
        g_timers.erase(s.timer);
        s.timer = 0;
    }
}

} // namespace

// ── PinholeRenderContext ────────────────────────────────────────────────────

PinholeRenderContext::PinholeRenderContext(Impl* impl) : impl_(impl) {}
PinholeRenderContext::~PinholeRenderContext() = default;

std::pair<int, int> PinholeRenderContext::size_px() const {
    return {impl_->width_px, impl_->height_px};
}

double PinholeRenderContext::dpr() const { return impl_->dpr; }

void PinholeRenderContext::clear(float r, float g, float b, float a) {
    if (impl_->cpu_mode) {
        const auto to_byte = [](float f) {
            return static_cast<uint8_t>(std::clamp(f * 255.f, 0.f, 255.f));
        };
        const uint8_t px[4] = {to_byte(r), to_byte(g), to_byte(b), to_byte(a)};
        const int n = impl_->width_px * impl_->height_px;
        for (int i = 0; i < n; ++i) std::memcpy(impl_->cpu_rgba + i * 4, px, 4);
        return;
    }
    PinholeState& s = *impl_->state;
    const float premul[4] = {r * a, g * a, b * a, a};  // swap chain is premultiplied
    s.host->ctx->ClearRenderTargetView(s.rtv.get(), premul);
}

void PinholeRenderContext::draw_frame(const Frame& frame, bool preserve_aspect) {
    const int vw = impl_->width_px;
    const int vh = impl_->height_px;
    if (!preserve_aspect || impl_->cpu_mode || vw <= 0 || vh <= 0 ||
        frame.width <= 0 || frame.height <= 0) {
        draw_image(frame.data.data(), frame.data.size(), frame.width, frame.height,
                   frame.format);
        return;
    }
    int lx = 0, ly = 0, lw = vw, lh = vh;
    const int64_t img_w = frame.width, img_h = frame.height;
    if (img_w * vh >= img_h * vw) {
        lh = static_cast<int>((img_h * vw + img_w / 2) / img_w);  // bars top/bottom
        ly = (vh - lh) / 2;
    } else {
        lw = static_cast<int>((img_w * vh + img_h / 2) / img_h);  // bars left/right
        lx = (vw - lw) / 2;
    }
    const D3D11_VIEWPORT full = impl_->viewport;
    impl_->viewport = D3D11_VIEWPORT{static_cast<float>(lx), static_cast<float>(ly),
                                     static_cast<float>(lw), static_cast<float>(lh), 0.f, 1.f};
    draw_image(frame.data.data(), frame.data.size(), frame.width, frame.height, frame.format);
    impl_->viewport = full;
}

void PinholeRenderContext::draw_image(const uint8_t* data, std::size_t size,
                                      int width, int height, pixel_format fmt) {
    if (width <= 0 || height <= 0) return;
    const std::size_t expected = pixel_format_byte_size(fmt, width, height);
    if (size < expected) {
        log_warn("draw_image: buffer too small (" + std::to_string(size) + " bytes, need " +
                 std::to_string(expected) + ") — frame skipped");
        return;
    }
    if (impl_->cpu_mode) {
        detail::cpu_draw_image(impl_->cpu_rgba, impl_->width_px, impl_->height_px, data, width,
                               height, fmt);
        return;
    }

    PinholeState& s = *impl_->state;
    PinholeHost& h = *s.host;
    ID3D11Device* dev = h.device.get();
    ID3D11DeviceContext* ctx = h.ctx.get();
    const size_t w = static_cast<size_t>(width);
    const int cw = (width + 1) / 2, ch = (height + 1) / 2;

    int mode = 0;
    int nplanes = 1;
    switch (fmt) {
    case pixel_format::rgba:
    case pixel_format::bgra: {
        DXGI_FORMAT f = fmt == pixel_format::rgba ? DXGI_FORMAT_R8G8B8A8_UNORM
                                                  : DXGI_FORMAT_B8G8R8A8_UNORM;
        if (!s.planes[0].ensure(dev, width, height, f)) return;
        s.planes[0].upload(ctx, data, w * 4, height);
        break;
    }
    case pixel_format::rgb: {
        // No 24-bit texture format in D3D11: expand to RGBA on the CPU.
        s.scratch.resize(w * height * 4);
        detail::cpu_draw_image(s.scratch.data(), width, height, data, width, height, fmt);
        if (!s.planes[0].ensure(dev, width, height, DXGI_FORMAT_R8G8B8A8_UNORM)) return;
        s.planes[0].upload(ctx, s.scratch.data(), w * 4, height);
        break;
    }
    case pixel_format::grayscale:
        mode = 1;
        if (!s.planes[0].ensure(dev, width, height, DXGI_FORMAT_R8_UNORM)) return;
        s.planes[0].upload(ctx, data, w, height);
        break;
    case pixel_format::yuv420: {
        mode = 2;
        nplanes = 3;
        const uint8_t* y = data;
        const uint8_t* u = y + w * height;
        const uint8_t* v = u + static_cast<size_t>(cw) * ch;
        if (!s.planes[0].ensure(dev, width, height, DXGI_FORMAT_R8_UNORM) ||
            !s.planes[1].ensure(dev, cw, ch, DXGI_FORMAT_R8_UNORM) ||
            !s.planes[2].ensure(dev, cw, ch, DXGI_FORMAT_R8_UNORM)) {
            return;
        }
        s.planes[0].upload(ctx, y, w, height);
        s.planes[1].upload(ctx, u, static_cast<size_t>(cw), ch);
        s.planes[2].upload(ctx, v, static_cast<size_t>(cw), ch);
        break;
    }
    case pixel_format::nv12:
    case pixel_format::nv21: {
        mode = fmt == pixel_format::nv12 ? 3 : 4;
        nplanes = 2;
        const uint8_t* y = data;
        const uint8_t* uv = y + w * height;
        if (!s.planes[0].ensure(dev, width, height, DXGI_FORMAT_R8_UNORM) ||
            !s.planes[1].ensure(dev, cw, ch, DXGI_FORMAT_R8G8_UNORM)) {
            return;
        }
        s.planes[0].upload(ctx, y, w, height);
        s.planes[1].upload(ctx, uv, static_cast<size_t>(cw) * 2, ch);
        break;
    }
    }

    const ShaderParams params{mode, {0, 0, 0}};
    ctx->UpdateSubresource(h.params.get(), 0, nullptr, &params, 0, 0);
    ID3D11ShaderResourceView* srvs[3] = {nullptr, nullptr, nullptr};
    for (int i = 0; i < nplanes; ++i) srvs[i] = s.planes[i].srv.get();
    ID3D11Buffer* cb = h.params.get();
    ID3D11SamplerState* smp = h.sampler.get();

    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
    ctx->IASetInputLayout(nullptr);
    ctx->RSSetState(h.raster.get());
    ctx->OMSetBlendState(h.blend.get(), nullptr, 0xFFFFFFFF);
    ctx->VSSetShader(h.vs.get(), nullptr, 0);
    ctx->PSSetShader(h.ps.get(), nullptr, 0);
    ctx->PSSetConstantBuffers(0, 1, &cb);
    ctx->PSSetShaderResources(0, 3, srvs);
    ctx->PSSetSamplers(0, 1, &smp);
    ctx->RSSetViewports(1, &impl_->viewport);
    ctx->Draw(4, 0);

    ID3D11ShaderResourceView* none[3] = {nullptr, nullptr, nullptr};
    ctx->PSSetShaderResources(0, 3, none);
}

// ── Pinhole ─────────────────────────────────────────────────────────────────

Pinhole::Pinhole() : impl_(std::make_unique<Impl>()) {}

Pinhole::~Pinhole() {
    if (!impl_) return;  // moved-from
    std::shared_ptr<PinholeState> s = impl_->s;
    s->dead = true;
    {
        std::lock_guard<std::mutex> lk(s->fb_mu);
        s->eval = nullptr;
        if (s->fb_buf) {
            SharedBufferRegistry::instance().remove(s->fb_buf->name());
            s->fb_buf.reset();
        }
    }
    // COM objects are released on the UI thread (they belong to it).
    auto cleanup = [s] {
        if (s->timer) {
            std::lock_guard<std::mutex> lk(g_timer_mu);
            g_timers.erase(s->timer);
        }
        s->release_native();
        if (s->host) s->host->restack();
        s->host.reset();
    };
    if (platform::is_main_thread()) cleanup();
    else post_to_main_thread(cleanup);
}

Pinhole::Pinhole(Pinhole&&) noexcept = default;
Pinhole& Pinhole::operator=(Pinhole&&) noexcept = default;

const std::string& Pinhole::id() const { return impl_->s->id; }
bool Pinhole::is_native() const { return impl_->s->native; }

void Pinhole::on_render(RenderFn fn) {
    std::lock_guard<std::mutex> lk(impl_->s->cb_mu);
    impl_->s->render_fn = std::move(fn);
}

void Pinhole::on_resize(std::function<void(int, int, double)> fn) {
    std::lock_guard<std::mutex> lk(impl_->s->cb_mu);
    impl_->s->resize_fn = std::move(fn);
}

void Pinhole::on_visibility(std::function<void(bool)> fn) {
    std::lock_guard<std::mutex> lk(impl_->s->cb_mu);
    impl_->s->visibility_fn = std::move(fn);
}

void Pinhole::on_dom_detached(std::function<void()> fn) {
    std::lock_guard<std::mutex> lk(impl_->s->cb_mu);
    impl_->s->dom_detached_fn = std::move(fn);
}

void Pinhole::notify_dom_detached() {
    std::function<void()> cb;
    {
        std::lock_guard<std::mutex> lk(impl_->s->cb_mu);
        cb = impl_->s->dom_detached_fn;
    }
    if (cb) cb();
}

void Pinhole::request_redraw() { schedule_redraw(impl_->s); }

void Pinhole::set_continuous(bool enabled) {
    auto self = impl_->s;
    if (!self->native) {
        if (enabled) {
            log_warn("'" + self->id + "': set_continuous(true) is not supported in canvas "
                     "fallback mode — use request_redraw()");
        }
        return;
    }
    post_ui(self, [enabled, self](PinholeState& s) {
        s.continuous = enabled;
        update_timer(s, self);
    });
}

void Pinhole::set_rect(int x_css, int y_css, int width_css, int height_css) {
    post_ui(impl_->s, [=](PinholeState& s) {
        s.x_css = x_css;
        s.y_css = y_css;
        s.w_css = width_css;
        s.h_css = height_css;
        if (s.native) {
            apply_geometry(s);
        } else {
            activate_fallback_canvas(s, width_css, height_css);
        }
    });
    schedule_redraw(impl_->s);
}

void Pinhole::set_visible(bool visible) {
    post_ui(impl_->s, [visible](PinholeState& s) {
        if (s.user_visible == visible) return;
        s.user_visible = visible;
        if (s.native && s.host) s.host->restack();
        std::function<void(bool)> cb;
        {
            std::lock_guard<std::mutex> lk(s.cb_mu);
            cb = s.visibility_fn;
        }
        if (cb) cb(visible);
    });
    if (visible) schedule_redraw(impl_->s);
}

void Pinhole::notify_window_destroyed() {
    // UI thread (Window teardown / WM_DESTROY).
    auto& s = *impl_->s;
    s.dead = true;
    {
        std::lock_guard<std::mutex> lk(s.fb_mu);
        s.eval = nullptr;
    }
    if (s.timer) {
        std::lock_guard<std::mutex> lk(g_timer_mu);
        g_timers.erase(s.timer);
    }
    s.release_native();
}

void Pinhole::set_z_index(int z) {
    impl_->s->z.store(z);
    std::function<void()> cb;
    {
        std::lock_guard<std::mutex> lk(impl_->s->cb_mu);
        cb = impl_->s->reorder_fn;
    }
    if (cb) cb();
}

int Pinhole::z_index() const { return impl_->s->z.load(); }

void Pinhole::set_window_active(bool active) {
    auto self = impl_->s;
    post_ui(self, [active, self](PinholeState& s) {
        if (s.window_active == active) return;
        s.window_active = active;
        update_timer(s, self);
    });
    if (active) schedule_redraw(self);
}

void Pinhole::set_reorder_callback(std::function<void()> fn) {
    std::lock_guard<std::mutex> lk(impl_->s->cb_mu);
    impl_->s->reorder_fn = std::move(fn);
}

void Pinhole::reorder_in_overlay() {
    post_ui(impl_->s, [](PinholeState& s) {
        if (s.native && s.host) s.host->restack();
    });
}

void Pinhole::platform_init(const std::string& id, const PinholeOptions& opts, void* overlay,
                            std::function<void(const std::string&)> eval_fn) {
    static std::atomic<uint64_t> next_order{0};
    auto& s = *impl_->s;
    s.id = id;
    s.opts = opts;
    s.order = ++next_order;
    s.continuous = opts.continuous;
    {
        std::lock_guard<std::mutex> lk(s.fb_mu);
        s.eval = std::move(eval_fn);
    }

    // `overlay` is a std::shared_ptr<PinholeHost>* owned by the Window.
    auto* host = static_cast<std::shared_ptr<PinholeHost>*>(overlay);
    if (!opts.force_fallback && host && *host && (*host)->alive) {
        s.native = true;
        s.host = *host;
        s.host->pins.push_back(impl_->s);
        s.dpr = s.host->dpr();
        update_timer(s, impl_->s);  // UI thread (create_pinhole)
    } else {
        s.native = false;
    }
}

void Pinhole::override_eval_fn_for_test(std::function<void(const std::string&)> fn) {
    std::lock_guard<std::mutex> lk(impl_->s->fb_mu);
    impl_->s->eval = std::move(fn);
}

} // namespace anyar

#endif // _WIN32
