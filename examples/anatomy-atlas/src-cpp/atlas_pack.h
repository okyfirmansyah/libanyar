#pragma once

// AtlasPack — reader for ANATPAK2 whole-body anatomy packs.
//
// A pack (built by tools/build_atlas_pack.py from BodyParts3D) holds ~2,200
// FMA-labelled parts, each at one or more levels of detail, with 16-bit
// quantised positions.  This module decodes parts to float geometry,
// computes smooth normals and answers name searches.  It has no LibAnyar
// dependency so it can be unit-tested directly.

#include <nlohmann/json.hpp>

#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace atlas {

using json = nlohmann::json;

/// One level of detail of one part, as stored in the pack.
struct LodRef {
    uint32_t vertices = 0;
    uint32_t triangles = 0;
    size_t positions = 0;   ///< byte offset of u16 xyz, relative to the blobs
    size_t indices = 0;     ///< byte offset of u16/u32 indices
    int index_bits = 16;
};

/// Metadata of one anatomical part.
struct PartInfo {
    std::string id;         ///< BodyParts3D element id, e.g. "FJ3262"
    std::string fma;        ///< FMA concept id, e.g. "FMA7088"
    std::string name;       ///< English name
    std::string system;     ///< skeletal, muscular, cardiovascular, …
    std::string kind;       ///< bone, muscle, artery, vein, …
    std::string region;     ///< head, thorax, abdomen, pelvis, upper_limb, lower_limb
    std::array<float, 3> min{}, max{};
    std::vector<LodRef> lods;
};

/// Decoded, render-ready geometry (several parts may be appended).
struct Geometry {
    std::vector<float> positions;    ///< xyz per vertex
    std::vector<float> normals;      ///< xyz per vertex (unit)
    std::vector<uint16_t> part;      ///< part index per vertex
    std::vector<uint32_t> indices;   ///< 3 per triangle
};

/// A loaded pack.  Holds the whole file in memory (≤ ~100 MB).
class Pack {
public:
    /// Parse a pack.
    /// @param bytes  Whole file contents (moved in).
    /// @returns      The pack.
    /// @throws std::runtime_error on a malformed file.
    static Pack parse(std::string bytes);

    /// Source/licence block from the header (name, licence, attribution, …).
    const json& source() const { return header_.at("source"); }

    /// Full header (units, frame, height, systems, lods_mm).
    const json& header() const { return header_; }

    /// All parts, in pack order (a part's index is its position here).
    const std::vector<PartInfo>& parts() const { return parts_; }

    /// Number of LODs stored for the part with the most.
    size_t lod_count() const { return lod_count_; }

    /// Decode one part at @p lod (clamped to what the part has) and append
    /// it to @p out, with smooth normals and its part index on every vertex.
    /// @param part  Index into parts().
    /// @param lod   0 = finest.
    /// @param out   Geometry to append to.
    void decode(size_t part, size_t lod, Geometry& out) const;

    /// Rank parts by how well their name / id / FMA id matches @p query.
    /// Every word of the query must prefix-match a word of the name or of
    /// the part's tags (kind, system, region — so "heart" finds the heart
    /// chambers), case-insensitive.  Name matches rank above tag-only
    /// matches; exact and leading name matches rank first.
    /// @param query  Free text, e.g. "left femur" or "FMA7088".
    /// @param limit  Maximum number of results.
    /// @returns      Part indices, best first.
    std::vector<size_t> search(std::string_view query, size_t limit) const;

private:
    std::string bytes_;
    size_t blob_ = 0;
    json header_;
    std::vector<PartInfo> parts_;
    std::vector<std::vector<std::string>> words_;   ///< lower-cased name words per part
    std::vector<std::vector<std::string>> tags_;    ///< kind/system/region words per part
    size_t lod_count_ = 0;
};

} // namespace atlas
