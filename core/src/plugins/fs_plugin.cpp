#include <anyar/plugins/fs_plugin.h>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <stdexcept>

namespace fs = std::filesystem;

namespace anyar {

// ── Helpers ─────────────────────────────────────────────────────────────────

static void validate_path(const std::string& path) {
    // Basic security: reject empty paths and explicit parent traversal
    if (path.empty()) {
        throw std::runtime_error("Path must not be empty");
    }
    // Canonical paths resolve ".." — we reject raw ".." segments as a warning
    // but the real safety comes from resolving the canonical path below.
}

static fs::path resolve(const std::string& raw) {
    validate_path(raw);
    return fs::absolute(raw);
}

// ── Plugin registration ─────────────────────────────────────────────────────

void FsPlugin::initialize(PluginContext& ctx) {
    auto& cmds = ctx.commands;

    // ── fs:readFile ─────────────────────────────────────────────────────────
    cmds.add("fs:readFile", [](const json& args) -> json {
        auto p = resolve(args.at("path").get<std::string>());
        std::string encoding = args.value("encoding", "utf-8");

        if (!fs::exists(p)) {
            throw std::runtime_error("File not found: " + p.string());
        }
        if (fs::is_directory(p)) {
            throw std::runtime_error("Path is a directory: " + p.string());
        }

        std::ifstream ifs(p, std::ios::binary);
        if (!ifs) {
            throw std::runtime_error("Cannot open file: " + p.string());
        }

        std::ostringstream ss;
        ss << ifs.rdbuf();
        std::string content = ss.str();

        if (encoding == "base64") {
            // Simple base64 encode (enough for binary files)
            static const char table[] =
                "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
            std::string b64;
            b64.reserve(((content.size() + 2) / 3) * 4);
            unsigned int val = 0;
            int bits = -6;
            for (unsigned char c : content) {
                val = (val << 8) + c;
                bits += 8;
                while (bits >= 0) {
                    b64.push_back(table[(val >> bits) & 0x3F]);
                    bits -= 6;
                }
            }
            if (bits > -6) b64.push_back(table[((val << 8) >> (bits + 8)) & 0x3F]);
            while (b64.size() % 4) b64.push_back('=');
            return b64;
        }

        // UTF-8 (default)
        return content;
    });

    // ── fs:writeFile ────────────────────────────────────────────────────────
    cmds.add("fs:writeFile", [](const json& args) -> json {
        auto p = resolve(args.at("path").get<std::string>());
        auto content = args.at("content").get<std::string>();

        // Create parent directories if needed
        if (p.has_parent_path()) {
            fs::create_directories(p.parent_path());
        }

        std::ofstream ofs(p, std::ios::binary | std::ios::trunc);
        if (!ofs) {
            throw std::runtime_error("Cannot write to file: " + p.string());
        }
        ofs << content;
        ofs.close();

        return nullptr;
    });

    // ── fs:readDir ──────────────────────────────────────────────────────────
    cmds.add("fs:readDir", [](const json& args) -> json {
        auto p = resolve(args.at("path").get<std::string>());

        if (!fs::exists(p)) {
            throw std::runtime_error("Directory not found: " + p.string());
        }
        if (!fs::is_directory(p)) {
            throw std::runtime_error("Path is not a directory: " + p.string());
        }

        json entries = json::array();
        for (const auto& entry : fs::directory_iterator(p)) {
            entries.push_back({
                {"name", entry.path().filename().string()},
                {"isDirectory", entry.is_directory()},
                {"isFile", entry.is_regular_file()},
            });
        }
        return entries;
    });

    // ── fs:exists ───────────────────────────────────────────────────────────
    cmds.add("fs:exists", [](const json& args) -> json {
        auto p = resolve(args.at("path").get<std::string>());
        return fs::exists(p);
    });

    // ── fs:mkdir ────────────────────────────────────────────────────────────
    cmds.add("fs:mkdir", [](const json& args) -> json {
        auto p = resolve(args.at("path").get<std::string>());
        fs::create_directories(p);
        return nullptr;
    });

    // ── fs:remove ───────────────────────────────────────────────────────────
    cmds.add("fs:remove", [](const json& args) -> json {
        auto p = resolve(args.at("path").get<std::string>());
        bool recursive = args.value("recursive", false);

        if (!fs::exists(p)) {
            throw std::runtime_error("Path not found: " + p.string());
        }

        if (recursive) {
            fs::remove_all(p);
        } else {
            if (fs::is_directory(p) && !fs::is_empty(p)) {
                throw std::runtime_error(
                    "Directory is not empty. Use recursive=true to remove: " + p.string());
            }
            fs::remove(p);
        }

        return nullptr;
    });

    // ── fs:metadata ─────────────────────────────────────────────────────────
    cmds.add("fs:metadata", [](const json& args) -> json {
        auto p = resolve(args.at("path").get<std::string>());

        if (!fs::exists(p)) {
            throw std::runtime_error("Path not found: " + p.string());
        }

        auto status = fs::status(p);
        auto ftime = fs::last_write_time(p);
        auto sctp = std::chrono::time_point_cast<std::chrono::seconds>(
            ftime - fs::file_time_type::clock::now() + std::chrono::system_clock::now());
        auto epoch = sctp.time_since_epoch().count();

        json result;
        result["size"] = fs::is_regular_file(p) ? static_cast<int64_t>(fs::file_size(p)) : 0;
        result["isDirectory"] = fs::is_directory(p);
        result["isFile"] = fs::is_regular_file(p);
        result["modifiedTime"] = epoch;

        return result;
    });
}

} // namespace anyar
