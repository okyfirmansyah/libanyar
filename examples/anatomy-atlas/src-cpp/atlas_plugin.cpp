// AtlasPlugin — pack loading, search and geometry streaming via SharedBuffer.

#include "atlas_plugin.h"
#include "atlas_pack.h"

#include <anyar/app_config.h>
#include <anyar/path.h>
#include <anyar/task.h>

#include <cmrc/cmrc.hpp>
#include <libasyik/service.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <iterator>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <stdexcept>

CMRC_DECLARE(atlas_assets);

namespace atlas {

namespace {

using Clock = std::chrono::steady_clock;

double ms_since(Clock::time_point t0) {
    return std::round(std::chrono::duration<double, std::milli>(Clock::now() - t0).count() * 10) / 10;
}

std::string read_file(const std::filesystem::path& p) {
    std::ifstream in(p, std::ios::binary);
    if (!in) throw std::runtime_error("cannot open " + anyar::path_to_utf8(p));
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

/// The full pack: $ANATOMY_PACK, or anatomy/ next to the cwd or the exe.
std::filesystem::path find_full_pack() {
    if (const char* env = std::getenv("ANATOMY_PACK"); env && *env) return anyar::path_from_utf8(env);
    auto dir = anyar::resolve_dist_path("./anatomy");
    if (dir.path.empty()) return {};
    auto p = anyar::path_from_utf8(dir.path) / "bodyparts3d-full.pack";
    return std::filesystem::exists(p) ? p : std::filesystem::path{};
}

} // namespace

/// Scope guard counting a command as in flight; throws once shutting down.
class AtlasPlugin::InFlight {
public:
    explicit InFlight(AtlasPlugin& p) : p_(p) {
        std::lock_guard<std::mutex> l(p_.jobs_mu_);
        if (p_.stopping_) throw std::runtime_error("atlas is shutting down");
        ++p_.jobs_;
    }
    ~InFlight() {
        {
            std::lock_guard<std::mutex> l(p_.jobs_mu_);
            --p_.jobs_;
        }
        p_.jobs_cv_.notify_all();
    }
    InFlight(const InFlight&) = delete;
    InFlight& operator=(const InFlight&) = delete;

private:
    AtlasPlugin& p_;
};

AtlasPlugin::AtlasPlugin() = default;
AtlasPlugin::~AtlasPlugin() = default;

const Pack& AtlasPlugin::pack() {
    while (loading_) asyik::sleep_for(std::chrono::milliseconds(5));   // another fiber is loading it
    if (pack_) return *pack_;
    loading_ = true;
    try {
        events_->emit("atlas:progress", {{"stage", "Reading anatomy pack"}, {"p", 0.05}});
        auto full = find_full_pack();
        struct Loaded { Pack pack; std::string kind, path; anyar::json manifest; };
        auto loaded = anyar::run_blocking(service_, [full] {
            if (!full.empty()) {
                anyar::json manifest;
                auto mpath = full;
                mpath += ".manifest.json";
                if (std::filesystem::exists(mpath)) manifest = anyar::json::parse(read_file(mpath));
                return Loaded{Pack::parse(read_file(full)), "full", anyar::path_to_utf8(full), manifest};
            }
            auto fs = cmrc::atlas_assets::get_filesystem();
            auto f = fs.open("sample.pack");
            auto m = fs.open("MANIFEST.json");
            return Loaded{Pack::parse(std::string(f.begin(), f.end())), "sample", "(embedded sample)",
                          anyar::json::parse(std::string(m.begin(), m.end()))};
        });
        pack_ = std::make_unique<Pack>(std::move(loaded.pack));
        // A pack with several LODs is "full", whatever path it came from.
        pack_kind_ = pack_->lod_count() > 1 ? "full" : "sample";
        pack_path_ = loaded.path;
        manifest_ = loaded.manifest;
        std::cout << "[atlas] " << pack_kind_ << " pack: " << pack_path_ << ", " << pack_->parts().size()
                  << " parts, " << pack_->lod_count() << " LOD(s)" << std::endl;
    } catch (...) {
        loading_ = false;
        throw;
    }
    loading_ = false;
    return *pack_;
}

void AtlasPlugin::initialize(anyar::PluginContext& ctx) {
    service_ = ctx.service;
    events_ = &ctx.events;

    ctx.commands.add("atlas:info", [this](const anyar::json&) {
        InFlight job(*this);
        const Pack& p = pack();
        return anyar::json{{"pack", pack_kind_},
                           {"path", pack_path_},
                           {"parts", p.parts().size()},
                           {"lods", p.lod_count()},
                           {"lods_mm", p.header().value("lods_mm", anyar::json::array())},
                           {"height", p.header().value("height", 0.0)},
                           {"systems", p.header().at("systems")},
                           {"source", p.source()},
                           {"manifest", manifest_}};
    });

    ctx.commands.add("atlas:parts", [this](const anyar::json&) {
        InFlight job(*this);
        const Pack& p = pack();
        anyar::json rows = anyar::json::array();
        for (const auto& info : p.parts()) {
            anyar::json tris = anyar::json::array();
            for (const auto& l : info.lods) tris.push_back(l.triangles);
            rows.push_back({info.id, info.fma, info.name, info.system, info.kind, info.region,
                            info.min[0], info.min[1], info.min[2], info.max[0], info.max[1], info.max[2], tris});
        }
        return anyar::json{{"fields", {"id", "fma", "name", "system", "kind", "region", "minx", "miny", "minz",
                                       "maxx", "maxy", "maxz", "triangles"}},
                           {"parts", std::move(rows)}};
    });

    ctx.commands.add("atlas:search", [this](const anyar::json& args) {
        InFlight job(*this);
        const Pack& p = pack();
        const auto t0 = Clock::now();
        auto hits = p.search(args.value("q", ""), args.value("limit", 30));
        return anyar::json{{"results", hits}, {"ms", ms_since(t0)}};
    });

    ctx.commands.add("atlas:load", [this](const anyar::json& args) {
        InFlight job(*this);
        const Pack& p = pack();
        const std::string system = args.at("system").get<std::string>();
        const size_t lod = args.value("lod", size_t(0));
        const auto t0 = Clock::now();
        Geometry g = anyar::run_blocking(service_, [this, &p, system, lod] {
            Geometry out;
            for (size_t i = 0; i < p.parts().size() && !stopping_; ++i)
                if (p.parts()[i].system == system) p.decode(i, lod, out);
            return out;
        });
        return publish(g, ms_since(t0));
    });

    ctx.commands.add("atlas:load_parts", [this](const anyar::json& args) {
        InFlight job(*this);
        const Pack& p = pack();
        auto ids = args.at("parts").get<std::vector<size_t>>();
        const size_t lod = args.value("lod", size_t(0));
        for (size_t i : ids)
            if (i >= p.parts().size()) throw std::invalid_argument("unknown part index " + std::to_string(i));
        const auto t0 = Clock::now();
        Geometry g = anyar::run_blocking(service_, [this, &p, ids, lod] {
            Geometry out;
            for (size_t i : ids) {
                if (stopping_) break;
                p.decode(i, lod, out);
            }
            return out;
        });
        return publish(g, ms_since(t0));
    });

    ctx.commands.add("atlas:release", [this](const anyar::json& args) {
        const std::string name = args.at("buffer").get<std::string>();
        std::lock_guard<std::mutex> lock(buffers_mu_);
        if (buffers_.erase(name)) anyar::SharedBufferRegistry::instance().remove(name);
        return anyar::json{{"ok", true}};
    });
}

anyar::json AtlasPlugin::publish(const Geometry& g, double ms) {
    const size_t nv = g.positions.size() / 3;
    const size_t pos_bytes = g.positions.size() * 4, part_bytes = (nv * 2 + 3) / 4 * 4;
    const size_t pos = 0, nrm = pos_bytes, part = nrm + pos_bytes, idx = part + part_bytes;
    const size_t bytes = std::max<size_t>(idx + g.indices.size() * 4, 4);

    const std::string name = "atlas-geo-" + std::to_string(++next_buffer_);
    auto buf = anyar::SharedBuffer::create(name, bytes);
    uint8_t* d = buf->data();
    std::memcpy(d + pos, g.positions.data(), pos_bytes);
    std::memcpy(d + nrm, g.normals.data(), pos_bytes);
    std::memcpy(d + part, g.part.data(), nv * 2);
    std::memcpy(d + idx, g.indices.data(), g.indices.size() * 4);
    {
        std::lock_guard<std::mutex> lock(buffers_mu_);
        buffers_[name] = buf;
    }
    if (std::getenv("ATLAS_DEBUG"))
        std::cout << "[atlas] " << name << ": " << g.indices.size() / 3 << " triangles, " << bytes / 1024 << " KiB, "
                  << ms << " ms" << std::endl;

    return anyar::json{{"buffer", name},
                       {"id", buf->id()},
                       {"bytes", bytes},
                       {"vertices", nv},
                       {"triangles", g.indices.size() / 3},
                       {"ms", ms},
                       {"layout", {{"positions", pos}, {"normals", nrm}, {"part", part}, {"indices", idx}}}};
}

void AtlasPlugin::shutdown() {
    {
        // The service is still running here, so in-flight commands can finish.
        std::unique_lock<std::mutex> l(jobs_mu_);
        stopping_ = true;
        if (!jobs_cv_.wait_for(l, std::chrono::seconds(10), [this] { return jobs_ == 0; }))
            std::cerr << "[atlas] shutdown: " << jobs_ << " load(s) still running after 10 s" << std::endl;
    }
    // shutdown() runs on the main thread while the service is still serving
    // commands (late atlas:release calls), hence the lock.
    std::lock_guard<std::mutex> lock(buffers_mu_);
    for (const auto& [name, buf] : buffers_) anyar::SharedBufferRegistry::instance().remove(name);
    buffers_.clear();
}

} // namespace atlas
