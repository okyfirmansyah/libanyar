#pragma once

// AtlasPlugin — serves a whole-body anatomy pack to the frontend.
//
// Commands:
//   atlas:info        {}                    → { pack, path, bytes, parts, lods, lods_mm, height, systems, source, manifest }
//   atlas:parts       {}                    → { fields[], parts[][] }   (compact table, one row per part)
//   atlas:search      { q, limit? }         → { results[], ms }         (part indices, best first)
//   atlas:load        { system, lod }       → GeometryMeta              (one system at one LOD)
//   atlas:load_parts  { parts[], lod }      → GeometryMeta              (e.g. full detail for a focused part)
//   atlas:release     { buffer }            → { ok }                    (frontend uploaded it to the GPU)
//
// GeometryMeta = { buffer, id, bytes, vertices, triangles, ms,
//                  layout: { positions, normals, part, indices } }  (byte offsets)
//   positions/normals: f32 × 3 per vertex; part: u16 per vertex; indices: u32.
//
// Events:
//   atlas:progress  { stage, p }
//
// Pack lookup: $ANATOMY_PACK, then anatomy/bodyparts3d-full.pack next to the
// working directory or the executable (CMake puts the fetched pack there),
// then the embedded whole-body sample.

#include <anyar/plugin.h>
#include <anyar/shared_buffer.h>

#include <atomic>
#include <condition_variable>
#include <map>
#include <memory>
#include <mutex>
#include <string>

namespace atlas {

class Pack;
struct Geometry;

/// Plugin owning the anatomy pack and the geometry SharedBuffers.
class AtlasPlugin : public anyar::IAnyarPlugin {
public:
    AtlasPlugin();
    ~AtlasPlugin() override;

    /// @returns "atlas"
    std::string name() const override { return "atlas"; }

    /// Register the atlas:* commands.
    void initialize(anyar::PluginContext& ctx) override;

    /// Refuse new work, wait for in-flight loads (they run on the worker
    /// pool and must finish before the service stops), release buffers.
    void shutdown() override;

private:
    const Pack& pack();                       ///< loads lazily, off the service thread
    anyar::json publish(const Geometry& g, double ms);

    asyik::service_ptr service_;
    anyar::EventBus* events_ = nullptr;
    std::unique_ptr<Pack> pack_;
    bool loading_ = false;
    std::string pack_kind_;                   ///< "full" | "sample"
    std::string pack_path_;
    anyar::json manifest_;
    /// Live geometry buffers.  Guarded: commands touch it on the service
    /// thread, shutdown() on the main thread while commands still arrive.
    std::mutex buffers_mu_;
    std::map<std::string, std::shared_ptr<anyar::SharedBuffer>> buffers_;
    uint64_t next_buffer_ = 0;

    /// Commands in flight (each may be waiting on a worker-pool job).
    /// shutdown() waits for zero: a worker finishing after the service
    /// stopped would wake a fiber on a destroyed scheduler.
    class InFlight;
    std::atomic<bool> stopping_{false};
    std::mutex jobs_mu_;
    std::condition_variable jobs_cv_;
    int jobs_ = 0;
};

} // namespace atlas
