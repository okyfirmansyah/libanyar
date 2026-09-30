#pragma once

/// @file pixel_format.h
/// @brief Raw pixel formats shared by Pinhole, FrameMailbox and the JS
///        FrameRenderer.

#include <cstddef>
#include <optional>
#include <string>

namespace anyar {

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

/// Tightly-packed byte size of a @p width × @p height image in @p fmt
/// (no row padding; 4:2:0 chroma planes are ⌈width/2⌉ × ⌈height/2⌉ — the
/// layout draw_image() and the JS FrameRenderer read).
std::size_t pixel_format_byte_size(pixel_format fmt, int width, int height);

/// Canonical lowercase name ("rgba", "yuv420", …) — the same strings the JS
/// FrameRenderer accepts.
const char* pixel_format_name(pixel_format fmt);

/// Parse a name produced by pixel_format_name(). Returns nullopt if unknown.
std::optional<pixel_format> pixel_format_from_name(const std::string& name);

} // namespace anyar
