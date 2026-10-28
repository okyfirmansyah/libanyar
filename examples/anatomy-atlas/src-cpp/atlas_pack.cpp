// AtlasPack — ANATPAK2 decode, normals, search.

#include "atlas_pack.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <stdexcept>

namespace atlas {

namespace {

std::vector<std::string> words_of(std::string_view s) {
    std::vector<std::string> out;
    std::string cur;
    for (char ch : s) {
        if (std::isalnum(static_cast<unsigned char>(ch))) {
            cur += char(std::tolower(static_cast<unsigned char>(ch)));
        } else if (!cur.empty()) {
            out.push_back(std::move(cur));
            cur.clear();
        }
    }
    if (!cur.empty()) out.push_back(std::move(cur));
    return out;
}

std::string lower(std::string_view s) {
    std::string out(s);
    for (auto& ch : out) ch = char(std::tolower(static_cast<unsigned char>(ch)));
    return out;
}

} // namespace

Pack Pack::parse(std::string bytes) {
    Pack p;
    p.bytes_ = std::move(bytes);
    const std::string& b = p.bytes_;
    if (b.size() < 12 || b.compare(0, 8, "ANATPAK2") != 0) throw std::runtime_error("not an ANATPAK2 anatomy pack");
    uint32_t hlen = 0;
    std::memcpy(&hlen, b.data() + 8, 4);
    if (12 + size_t(hlen) > b.size()) throw std::runtime_error("truncated anatomy pack header");
    p.header_ = json::parse(b.begin() + 12, b.begin() + 12 + hlen);
    p.blob_ = 12 + hlen;
    const size_t blob_size = b.size() - p.blob_;

    for (const auto& j : p.header_.at("parts")) {
        PartInfo info;
        info.id = j.at("id").get<std::string>();
        info.fma = j.value("fma", "");
        info.name = j.value("name", info.id);
        if (info.name.empty()) info.name = info.id;
        info.system = j.at("system").get<std::string>();
        info.kind = j.value("kind", "");
        info.region = j.value("region", "");
        for (int a = 0; a < 3; ++a) {
            info.min[a] = j.at("min")[a];
            info.max[a] = j.at("max")[a];
        }
        for (const auto& l : j.at("lods")) {
            LodRef r;
            r.vertices = l.at("vertices");
            r.triangles = l.at("triangles");
            r.positions = l.at("positions");
            r.indices = l.at("indices");
            r.index_bits = l.at("index_bits");
            const size_t ib = r.index_bits == 16 ? 2 : 4;
            if (r.index_bits != 16 && r.index_bits != 32) throw std::runtime_error("bad index width in " + info.id);
            if (r.positions + size_t(r.vertices) * 6 > blob_size || r.indices + size_t(r.triangles) * 3 * ib > blob_size)
                throw std::runtime_error("anatomy pack part out of range: " + info.id);
            info.lods.push_back(r);
        }
        if (info.lods.empty()) throw std::runtime_error("part without geometry: " + info.id);
        p.lod_count_ = std::max(p.lod_count_, info.lods.size());
        p.words_.push_back(words_of(info.name));
        p.tags_.push_back(words_of(info.kind + " " + info.system + " " + info.region));
        p.parts_.push_back(std::move(info));
    }
    if (p.parts_.size() > 65535) throw std::runtime_error("too many parts for 16-bit part ids");
    return p;
}

void Pack::decode(size_t part, size_t lod, Geometry& out) const {
    const PartInfo& info = parts_.at(part);
    const LodRef& r = info.lods[std::min(lod, info.lods.size() - 1)];
    const char* blob = bytes_.data() + blob_;
    const uint32_t base = uint32_t(out.positions.size() / 3);

    float scale[3];
    for (int a = 0; a < 3; ++a) scale[a] = (info.max[a] - info.min[a]) / 65535.0f;
    for (size_t i = 0; i < size_t(r.vertices) * 3; ++i) {
        uint16_t q;
        std::memcpy(&q, blob + r.positions + i * 2, 2);
        out.positions.push_back(info.min[i % 3] + float(q) * scale[i % 3]);
    }
    const size_t first_index = out.indices.size();
    for (size_t i = 0; i < size_t(r.triangles) * 3; ++i) {
        uint32_t v;
        if (r.index_bits == 16) {
            uint16_t q;
            std::memcpy(&q, blob + r.indices + i * 2, 2);
            v = q;
        } else {
            std::memcpy(&v, blob + r.indices + i * 4, 4);
        }
        if (v >= r.vertices) throw std::runtime_error("anatomy pack index out of range: " + info.id);
        out.indices.push_back(base + v);
    }
    out.part.insert(out.part.end(), r.vertices, uint16_t(part));

    // Area-weighted smooth normals for the new vertices.
    out.normals.resize(out.positions.size(), 0.0f);
    const float* P = out.positions.data();
    float* N = out.normals.data();
    for (size_t t = first_index; t < out.indices.size(); t += 3) {
        const uint32_t i0 = out.indices[t], i1 = out.indices[t + 1], i2 = out.indices[t + 2];
        const float* a = P + i0 * 3;
        const float* b = P + i1 * 3;
        const float* c = P + i2 * 3;
        const float e1[3] = {b[0] - a[0], b[1] - a[1], b[2] - a[2]};
        const float e2[3] = {c[0] - a[0], c[1] - a[1], c[2] - a[2]};
        const float n[3] = {e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2], e1[0] * e2[1] - e1[1] * e2[0]};
        for (uint32_t i : {i0, i1, i2})
            for (int k = 0; k < 3; ++k) N[i * 3 + k] += n[k];
    }
    for (size_t i = size_t(base) * 3; i < out.normals.size(); i += 3) {
        const float l = std::sqrt(N[i] * N[i] + N[i + 1] * N[i + 1] + N[i + 2] * N[i + 2]);
        if (l > 1e-20f) {
            N[i] /= l;
            N[i + 1] /= l;
            N[i + 2] /= l;
        } else {
            N[i + 1] = 1.0f;
        }
    }
}

std::vector<size_t> Pack::search(std::string_view query, size_t limit) const {
    const auto q = words_of(query);
    if (q.empty() || limit == 0) return {};
    const std::string ql = lower(query);
    struct Hit { int score; size_t index; };
    std::vector<Hit> hits;
    for (size_t i = 0; i < parts_.size(); ++i) {
        const auto& info = parts_[i];
        int score = 0;
        if (lower(info.id) == ql || lower(info.fma) == ql) {
            score = 1000;
        } else {
            const auto& w = words_[i];
            auto has = [&](const std::vector<std::string>& words, const std::string& term) {
                for (const auto& word : words)
                    if (word.compare(0, term.size(), term) == 0) return true;
                return false;
            };
            bool all = true;
            int tag_only = 0;
            for (const auto& term : q) {
                if (has(w, term)) continue;
                if (has(tags_[i], term)) { ++tag_only; continue; }
                all = false;
                break;
            }
            if (!all) continue;
            const std::string nl = lower(info.name);
            score = 100 - 60 * tag_only;
            if (nl == ql) score += 500;
            else if (nl.compare(0, ql.size(), ql) == 0) score += 200;
            else if (!w.empty() && w[0].compare(0, q[0].size(), q[0]) == 0) score += 50;
            score -= int(std::min<size_t>(w.size(), 40));   // shorter names first
        }
        hits.push_back({score, i});
    }
    std::stable_sort(hits.begin(), hits.end(), [](const Hit& a, const Hit& b) { return a.score > b.score; });
    std::vector<size_t> out;
    for (size_t k = 0; k < hits.size() && k < limit; ++k) out.push_back(hits[k].index);
    return out;
}

} // namespace atlas
