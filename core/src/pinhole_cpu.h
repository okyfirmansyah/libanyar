#pragma once

// LibAnyar — private: CPU conversion of any pixel_format to packed RGBA,
// used by the Pinhole canvas-fallback renderers (and the Win32 rgb upload).

#include <anyar/pixel_format.h>

#include <cstdint>

namespace anyar::detail {

/// Draw @p src (@p src_w × @p src_h, format @p fmt) into the RGBA buffer
/// @p dst (@p dst_w × @p dst_h × 4 bytes), nearest-neighbour stretched.
/// 4:2:0 formats use ⌈w/2⌉×⌈h/2⌉ chroma, BT.601 full range.
void cpu_draw_image(uint8_t* dst, int dst_w, int dst_h,
                    const uint8_t* src, int src_w, int src_h,
                    pixel_format fmt);

} // namespace anyar::detail
