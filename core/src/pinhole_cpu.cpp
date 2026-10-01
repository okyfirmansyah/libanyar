// LibAnyar — Pinhole CPU pixel conversion (shared by every platform's
// canvas-fallback path; see pinhole_cpu.h).

#include "pinhole_cpu.h"

#include <algorithm>
#include <cstdint>
#include <cstring>

namespace anyar::detail {

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

void cpu_draw_image(uint8_t* dst, int dst_w, int dst_h,
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

} // namespace anyar::detail
