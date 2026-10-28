// Anatomy atlas example — ANATPAK2 reader + search unit tests (tier 1).

#define CATCH_CONFIG_NO_POSIX_SIGNALS
#include <catch2/catch.hpp>

#include "atlas_pack.h"

#include <cctype>
#include <cmath>
#include <fstream>
#include <iterator>
#include <set>
#include <string>

namespace {
const atlas::Pack& sample() {
    static const atlas::Pack p = [] {
        std::ifstream in(ATLAS_SAMPLE_PACK, std::ios::binary);
        return atlas::Pack::parse(std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>()));
    }();
    return p;
}
size_t find(const std::string& name) {
    for (size_t i = 0; i < sample().parts().size(); ++i)
        if (sample().parts()[i].name == name) return i;
    FAIL("no part named " << name);
    return 0;
}
} // namespace

TEST_CASE("sample pack header and parts", "[atlas][pack]") {
    const auto& p = sample();
    CHECK(p.parts().size() == 2234);
    CHECK(p.lod_count() == 1);
    CHECK(p.source().at("license") == "CC-BY-4.0");
    CHECK(p.source().at("attribution").get<std::string>().rfind("BodyParts3D, ©", 0) == 0);
    std::set<std::string> systems;
    for (const auto& part : p.parts()) systems.insert(part.system);
    for (const char* s : {"skeletal", "muscular", "cardiovascular", "nervous", "respiratory", "digestive"})
        CHECK(systems.count(s) == 1);
    // Body frame: soles at y = 0, about 1.6–1.9 m tall.
    double h = p.header().at("height");
    CHECK(h > 150.0);
    CHECK(h < 200.0);
}

TEST_CASE("decode produces consistent geometry", "[atlas][pack]") {
    const auto& p = sample();
    atlas::Geometry g;
    const size_t femur = find("Left femur"), heart_part = find("Wall of right atrium");
    p.decode(femur, 0, g);
    const size_t after_first = g.positions.size() / 3;
    p.decode(heart_part, 5, g);   // LOD is clamped to what the part has
    const size_t nv = g.positions.size() / 3;
    REQUIRE(after_first > 0);
    REQUIRE(nv > after_first);
    REQUIRE(g.normals.size() == g.positions.size());
    REQUIRE(g.part.size() == nv);
    CHECK(g.part.front() == femur);
    CHECK(g.part.back() == heart_part);
    for (uint32_t i : g.indices) REQUIRE(i < nv);
    for (size_t i = 0; i < g.normals.size(); i += 3)
        REQUIRE(std::sqrt(g.normals[i] * g.normals[i] + g.normals[i + 1] * g.normals[i + 1] +
                          g.normals[i + 2] * g.normals[i + 2]) == Approx(1.0f).margin(1e-3));
    // Left femur sits on the patient's left (+x), in the lower half of the body.
    const auto& info = p.parts()[femur];
    CHECK((info.min[0] + info.max[0]) / 2 > 0);
    CHECK(info.max[1] < p.header().at("height").get<float>() * 0.6f);
    CHECK(info.system == "skeletal");
    CHECK(info.region == "lower_limb");
}

TEST_CASE("search ranks names and ids", "[atlas][search]") {
    const auto& p = sample();
    auto r = p.search("left femur", 5);
    REQUIRE_FALSE(r.empty());
    CHECK(p.parts()[r.front()].name == "Left femur");
    auto by_id = p.search(p.parts()[r.front()].id, 3);
    REQUIRE_FALSE(by_id.empty());
    CHECK(by_id.front() == r.front());
    CHECK(p.search("fem", 50).size() >= 2);          // prefix match
    CHECK(p.search("zzzz-nothing", 5).empty());
    // Tags: "heart" has no part named so, but finds the heart chambers.
    auto heart = p.search("heart", 50);
    REQUIRE_FALSE(heart.empty());
    for (size_t i : heart) CHECK(p.parts()[i].kind == "heart");
    // Name matches outrank tag-only matches.
    auto artery = p.search("artery", 5);
    REQUIRE_FALSE(artery.empty());
    std::string top = p.parts()[artery.front()].name;
    for (auto& ch : top) ch = char(std::tolower(static_cast<unsigned char>(ch)));
    CHECK(top.find("artery") != std::string::npos);
    CHECK(p.search("", 5).empty());
}

TEST_CASE("malformed packs are rejected", "[atlas][pack]") {
    CHECK_THROWS(atlas::Pack::parse("garbage"));
    CHECK_THROWS(atlas::Pack::parse(std::string("ANATPAK2\xff\xff\x00\x00", 12)));
}
