// KeystorePlugin — Encrypted SQLite key storage implementation
//
// See keystore_plugin.h for file format and command reference.

#include "keystore_plugin.h"

#include <sqlite3.h>
#include <openssl/evp.h>
#include <openssl/rand.h>
#include <openssl/err.h>

#include <nlohmann/json.hpp>

#include <fstream>
#include <filesystem>
#include <iostream>
#include <cstring>
#include <stdexcept>
#include <algorithm>

using json = nlohmann::json;
namespace fs = std::filesystem;

namespace keystore {

// ═══════════════════════════════════════════════════════════════════════════════
// Crypto helpers
// ═══════════════════════════════════════════════════════════════════════════════

std::vector<uint8_t> KeystorePlugin::generate_salt(int bytes) {
    std::vector<uint8_t> salt(bytes);
    if (RAND_bytes(salt.data(), bytes) != 1)
        throw std::runtime_error("RAND_bytes failed");
    return salt;
}

std::vector<uint8_t> KeystorePlugin::generate_iv(int bytes) {
    return generate_salt(bytes);  // same CSPRNG
}

std::vector<uint8_t> KeystorePlugin::derive_key(
    const std::string& password,
    const std::vector<uint8_t>& salt,
    int iterations)
{
    std::vector<uint8_t> key(32); // 256-bit
    if (PKCS5_PBKDF2_HMAC(password.c_str(), static_cast<int>(password.size()),
                           salt.data(), static_cast<int>(salt.size()),
                           iterations, EVP_sha256(),
                           32, key.data()) != 1) {
        throw std::runtime_error("PBKDF2 key derivation failed");
    }
    return key;
}

std::tuple<std::vector<uint8_t>, std::vector<uint8_t>, std::vector<uint8_t>>
KeystorePlugin::encrypt_aes_gcm(const std::vector<uint8_t>& key,
                                const std::vector<uint8_t>& plaintext)
{
    auto iv = generate_iv(12);
    std::vector<uint8_t> ciphertext(plaintext.size());
    std::vector<uint8_t> tag(16);

    EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
    if (!ctx) throw std::runtime_error("EVP_CIPHER_CTX_new failed");

    try {
        if (EVP_EncryptInit_ex(ctx, EVP_aes_256_gcm(), nullptr, nullptr, nullptr) != 1)
            throw std::runtime_error("EncryptInit_ex failed");
        if (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_IVLEN, 12, nullptr) != 1)
            throw std::runtime_error("Set IV len failed");
        if (EVP_EncryptInit_ex(ctx, nullptr, nullptr, key.data(), iv.data()) != 1)
            throw std::runtime_error("EncryptInit_ex key/iv failed");

        int len = 0;
        if (EVP_EncryptUpdate(ctx, ciphertext.data(), &len,
                              plaintext.data(), static_cast<int>(plaintext.size())) != 1)
            throw std::runtime_error("EncryptUpdate failed");

        int final_len = 0;
        if (EVP_EncryptFinal_ex(ctx, ciphertext.data() + len, &final_len) != 1)
            throw std::runtime_error("EncryptFinal_ex failed");

        if (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_GET_TAG, 16, tag.data()) != 1)
            throw std::runtime_error("Get tag failed");

        ciphertext.resize(len + final_len);
        EVP_CIPHER_CTX_free(ctx);
    } catch (...) {
        EVP_CIPHER_CTX_free(ctx);
        throw;
    }

    return {ciphertext, tag, iv};
}

std::vector<uint8_t> KeystorePlugin::decrypt_aes_gcm(
    const std::vector<uint8_t>& key,
    const std::vector<uint8_t>& ciphertext,
    const std::vector<uint8_t>& iv,
    const std::vector<uint8_t>& tag)
{
    std::vector<uint8_t> plaintext(ciphertext.size());

    EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
    if (!ctx) throw std::runtime_error("EVP_CIPHER_CTX_new failed");

    try {
        if (EVP_DecryptInit_ex(ctx, EVP_aes_256_gcm(), nullptr, nullptr, nullptr) != 1)
            throw std::runtime_error("DecryptInit_ex failed");
        if (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_IVLEN, 12, nullptr) != 1)
            throw std::runtime_error("Set IV len failed");
        if (EVP_DecryptInit_ex(ctx, nullptr, nullptr, key.data(), iv.data()) != 1)
            throw std::runtime_error("DecryptInit_ex key/iv failed");

        int len = 0;
        if (EVP_DecryptUpdate(ctx, plaintext.data(), &len,
                              ciphertext.data(), static_cast<int>(ciphertext.size())) != 1)
            throw std::runtime_error("DecryptUpdate failed");

        // Set expected tag BEFORE DecryptFinal
        if (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_TAG, 16,
                                const_cast<uint8_t*>(tag.data())) != 1)
            throw std::runtime_error("Set tag failed");

        int final_len = 0;
        if (EVP_DecryptFinal_ex(ctx, plaintext.data() + len, &final_len) != 1)
            throw std::runtime_error("Authentication failed — wrong password or corrupted file");

        plaintext.resize(len + final_len);
        EVP_CIPHER_CTX_free(ctx);
    } catch (...) {
        EVP_CIPHER_CTX_free(ctx);
        throw;
    }

    return plaintext;
}

// ═══════════════════════════════════════════════════════════════════════════════
// File I/O — encrypted container format
// ═══════════════════════════════════════════════════════════════════════════════

void KeystorePlugin::write_encrypted_file(const std::string& path) {
    // Read the temporary SQLite file into memory
    std::ifstream in(tmp_db_path_, std::ios::binary);
    if (!in) throw std::runtime_error("Cannot read temp database");
    std::vector<uint8_t> plaintext((std::istreambuf_iterator<char>(in)),
                                    std::istreambuf_iterator<char>());
    in.close();

    auto [ciphertext, tag, iv] = encrypt_aes_gcm(derived_key_, plaintext);

    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) throw std::runtime_error("Cannot write to " + path);

    // Header
    out.write(MAGIC, 8);  // includes null terminator → 8 bytes

    uint32_t ver = VERSION;
    out.write(reinterpret_cast<const char*>(&ver), 4);

    uint32_t iters = PBKDF2_ITERS;
    out.write(reinterpret_cast<const char*>(&iters), 4);

    out.write(reinterpret_cast<const char*>(salt_.data()), 32);
    out.write(reinterpret_cast<const char*>(iv.data()), 12);
    out.write(reinterpret_cast<const char*>(tag.data()), 16);

    uint32_t payload_len = static_cast<uint32_t>(ciphertext.size());
    out.write(reinterpret_cast<const char*>(&payload_len), 4);

    out.write(reinterpret_cast<const char*>(ciphertext.data()), ciphertext.size());
    out.close();
}

void KeystorePlugin::read_encrypted_file(const std::string& path,
                                         const std::string& password) {
    std::ifstream in(path, std::ios::binary);
    if (!in) throw std::runtime_error("Cannot open file: " + path);

    // Read magic
    char magic[8];
    in.read(magic, 8);
    if (std::memcmp(magic, MAGIC, 8) != 0)
        throw std::runtime_error("Not a valid AnyarKS file");

    // Read version
    uint32_t ver;
    in.read(reinterpret_cast<char*>(&ver), 4);
    if (ver != VERSION)
        throw std::runtime_error("Unsupported file version: " + std::to_string(ver));

    // Read PBKDF2 iterations
    uint32_t iters;
    in.read(reinterpret_cast<char*>(&iters), 4);

    // Read salt (32 bytes)
    std::vector<uint8_t> salt(32);
    in.read(reinterpret_cast<char*>(salt.data()), 32);

    // Read IV (12 bytes)
    std::vector<uint8_t> iv(12);
    in.read(reinterpret_cast<char*>(iv.data()), 12);

    // Read tag (16 bytes)
    std::vector<uint8_t> tag(16);
    in.read(reinterpret_cast<char*>(tag.data()), 16);

    // Read payload length
    uint32_t payload_len;
    in.read(reinterpret_cast<char*>(&payload_len), 4);

    // Read ciphertext
    std::vector<uint8_t> ciphertext(payload_len);
    in.read(reinterpret_cast<char*>(ciphertext.data()), payload_len);
    in.close();

    // Derive key and decrypt
    auto key = derive_key(password, salt, static_cast<int>(iters));
    auto plaintext = decrypt_aes_gcm(key, ciphertext, iv, tag);

    // Store state
    salt_ = salt;
    derived_key_ = key;
    file_path_ = path;

    // Write decrypted SQLite to temp file
    cleanup_tmp();
    tmp_db_path_ = fs::temp_directory_path() / ("anyarks_" + std::to_string(getpid()) + ".db");
    std::ofstream out(tmp_db_path_, std::ios::binary | std::ios::trunc);
    if (!out) throw std::runtime_error("Cannot create temp database");
    out.write(reinterpret_cast<const char*>(plaintext.data()), plaintext.size());
    out.close();
}

// ═══════════════════════════════════════════════════════════════════════════════
// SQLite helpers
// ═══════════════════════════════════════════════════════════════════════════════

void KeystorePlugin::open_db() {
    if (db_) return;
    int rc = sqlite3_open(tmp_db_path_.c_str(), &db_);
    if (rc != SQLITE_OK) {
        std::string err = sqlite3_errmsg(db_);
        sqlite3_close(db_);
        db_ = nullptr;
        throw std::runtime_error("SQLite open failed: " + err);
    }
    // Enable WAL mode and foreign keys
    sqlite3_exec(db_, "PRAGMA journal_mode=WAL;", nullptr, nullptr, nullptr);
    sqlite3_exec(db_, "PRAGMA foreign_keys=ON;", nullptr, nullptr, nullptr);
}

void KeystorePlugin::close_db() {
    if (db_) {
        sqlite3_close(db_);
        db_ = nullptr;
    }
}

void KeystorePlugin::cleanup_tmp() {
    if (!tmp_db_path_.empty() && fs::exists(tmp_db_path_)) {
        // Overwrite with zeros before deleting for security
        auto sz = fs::file_size(tmp_db_path_);
        if (sz > 0) {
            std::ofstream zer(tmp_db_path_, std::ios::binary | std::ios::trunc);
            std::vector<char> zeros(std::min<size_t>(sz, 65536), '\0');
            size_t remaining = sz;
            while (remaining > 0) {
                auto chunk = std::min(remaining, zeros.size());
                zer.write(zeros.data(), chunk);
                remaining -= chunk;
            }
        }
        fs::remove(tmp_db_path_);
        // Also remove WAL and SHM files
        fs::remove(tmp_db_path_ + "-wal");
        fs::remove(tmp_db_path_ + "-shm");
        tmp_db_path_.clear();
    }
}

void KeystorePlugin::init_schema() {
    const char* sql = R"SQL(
        CREATE TABLE IF NOT EXISTS groups (
            id          INTEGER PRIMARY KEY AUTOINCREMENT,
            parent_id   INTEGER REFERENCES groups(id) ON DELETE CASCADE,
            name        TEXT NOT NULL DEFAULT 'New Group',
            created_at  TEXT NOT NULL DEFAULT (datetime('now')),
            updated_at  TEXT NOT NULL DEFAULT (datetime('now'))
        );

        CREATE TABLE IF NOT EXISTS entries (
            id           INTEGER PRIMARY KEY AUTOINCREMENT,
            group_id     INTEGER REFERENCES groups(id) ON DELETE SET NULL,
            title        TEXT NOT NULL DEFAULT '',
            username     TEXT NOT NULL DEFAULT '',
            password     TEXT NOT NULL DEFAULT '',
            url          TEXT NOT NULL DEFAULT '',
            notes        TEXT NOT NULL DEFAULT '',
            expires_at   TEXT,
            icon_data    BLOB,
            icon_mime    TEXT,
            created_at   TEXT NOT NULL DEFAULT (datetime('now')),
            updated_at   TEXT NOT NULL DEFAULT (datetime('now'))
        );

        CREATE TABLE IF NOT EXISTS attachments (
            id          INTEGER PRIMARY KEY AUTOINCREMENT,
            entry_id    INTEGER NOT NULL REFERENCES entries(id) ON DELETE CASCADE,
            name        TEXT NOT NULL,
            data        BLOB NOT NULL,
            mime_type   TEXT NOT NULL DEFAULT 'application/octet-stream',
            size        INTEGER NOT NULL DEFAULT 0,
            created_at  TEXT NOT NULL DEFAULT (datetime('now'))
        );

        -- Seed a root group if empty
        INSERT OR IGNORE INTO groups (id, parent_id, name) VALUES (1, NULL, 'Root');
    )SQL";

    char* err = nullptr;
    if (sqlite3_exec(db_, sql, nullptr, nullptr, &err) != SQLITE_OK) {
        std::string msg = err ? err : "unknown error";
        sqlite3_free(err);
        throw std::runtime_error("Schema init failed: " + msg);
    }
}

// ═══════════════════════════════════════════════════════════════════════════════
// Helper: base64 encode/decode (using OpenSSL)
// ═══════════════════════════════════════════════════════════════════════════════

static std::string base64_encode(const uint8_t* data, size_t len) {
    if (len == 0) return "";
    size_t out_len = 4 * ((len + 2) / 3);
    std::string out(out_len + 1, '\0');
    int written = EVP_EncodeBlock(reinterpret_cast<unsigned char*>(out.data()),
                                  data, static_cast<int>(len));
    out.resize(written);
    return out;
}

static std::vector<uint8_t> base64_decode(const std::string& b64) {
    if (b64.empty()) return {};
    size_t out_len = 3 * b64.size() / 4 + 4;
    std::vector<uint8_t> out(out_len);
    int written = EVP_DecodeBlock(out.data(),
                                  reinterpret_cast<const unsigned char*>(b64.c_str()),
                                  static_cast<int>(b64.size()));
    if (written < 0) throw std::runtime_error("base64 decode failed");
    // EVP_DecodeBlock may pad; adjust for trailing '=' in input
    if (b64.size() >= 2 && b64[b64.size()-1] == '=') written--;
    if (b64.size() >= 3 && b64[b64.size()-2] == '=') written--;
    out.resize(written);
    return out;
}

// ═══════════════════════════════════════════════════════════════════════════════
// Helper: SQLite query helpers
// ═══════════════════════════════════════════════════════════════════════════════

static json groups_to_json(sqlite3* db) {
    json groups = json::array();
    sqlite3_stmt* stmt = nullptr;
    sqlite3_prepare_v2(db, "SELECT id, parent_id, name, created_at, updated_at FROM groups ORDER BY name",
                       -1, &stmt, nullptr);
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        json g;
        g["id"] = sqlite3_column_int(stmt, 0);
        if (sqlite3_column_type(stmt, 1) == SQLITE_NULL)
            g["parentId"] = nullptr;
        else
            g["parentId"] = sqlite3_column_int(stmt, 1);
        g["name"] = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2));
        g["createdAt"] = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 3));
        g["updatedAt"] = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 4));
        groups.push_back(g);
    }
    sqlite3_finalize(stmt);
    return groups;
}

static json entry_row_to_json(sqlite3_stmt* stmt) {
    json e;
    e["id"]        = sqlite3_column_int(stmt, 0);
    if (sqlite3_column_type(stmt, 1) == SQLITE_NULL) e["groupId"] = nullptr;
    else e["groupId"] = sqlite3_column_int(stmt, 1);
    e["title"]     = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2) ?: reinterpret_cast<const unsigned char*>(""));
    e["username"]  = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 3) ?: reinterpret_cast<const unsigned char*>(""));
    e["password"]  = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 4) ?: reinterpret_cast<const unsigned char*>(""));
    e["url"]       = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 5) ?: reinterpret_cast<const unsigned char*>(""));
    e["notes"]     = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 6) ?: reinterpret_cast<const unsigned char*>(""));
    if (sqlite3_column_type(stmt, 7) == SQLITE_NULL) e["expiresAt"] = nullptr;
    else e["expiresAt"] = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 7));
    e["hasIcon"]   = (sqlite3_column_type(stmt, 8) != SQLITE_NULL && sqlite3_column_bytes(stmt, 8) > 0);
    e["createdAt"] = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 9) ?: reinterpret_cast<const unsigned char*>(""));
    e["updatedAt"] = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 10) ?: reinterpret_cast<const unsigned char*>(""));
    return e;
}

static json entries_for_group(sqlite3* db, int group_id, bool all) {
    json entries = json::array();
    sqlite3_stmt* stmt = nullptr;
    if (all) {
        sqlite3_prepare_v2(db,
            "SELECT id, group_id, title, username, password, url, notes, expires_at, icon_data, created_at, updated_at "
            "FROM entries ORDER BY title", -1, &stmt, nullptr);
    } else {
        sqlite3_prepare_v2(db,
            "SELECT id, group_id, title, username, password, url, notes, expires_at, icon_data, created_at, updated_at "
            "FROM entries WHERE group_id = ? ORDER BY title", -1, &stmt, nullptr);
        sqlite3_bind_int(stmt, 1, group_id);
    }
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        entries.push_back(entry_row_to_json(stmt));
    }
    sqlite3_finalize(stmt);
    return entries;
}

static json attachments_for_entry(sqlite3* db, int entry_id) {
    json atts = json::array();
    sqlite3_stmt* stmt = nullptr;
    sqlite3_prepare_v2(db,
        "SELECT id, name, mime_type, size, created_at FROM attachments WHERE entry_id = ? ORDER BY name",
        -1, &stmt, nullptr);
    sqlite3_bind_int(stmt, 1, entry_id);
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        json a;
        a["id"]       = sqlite3_column_int(stmt, 0);
        a["name"]     = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
        a["mimeType"] = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2));
        a["size"]     = sqlite3_column_int(stmt, 3);
        a["createdAt"]= reinterpret_cast<const char*>(sqlite3_column_text(stmt, 4));
        atts.push_back(a);
    }
    sqlite3_finalize(stmt);
    return atts;
}

// ═══════════════════════════════════════════════════════════════════════════════
// Plugin lifecycle
// ═══════════════════════════════════════════════════════════════════════════════

void KeystorePlugin::shutdown() {
    close_db();
    cleanup_tmp();
    derived_key_.clear();
    salt_.clear();
    std::cout << "[KeystorePlugin] Shutdown — temp files cleaned" << std::endl;
}

void KeystorePlugin::initialize(anyar::PluginContext& ctx) {
    auto& cmds = ctx.commands;

    // ── ks:new — create a new encrypted database ────────────────────────────
    cmds.add("ks:new", [this](const json& args) -> json {
        std::string path     = args.at("path");
        std::string password = args.at("password");

        close_db();
        cleanup_tmp();

        // Generate salt and derive key
        salt_ = generate_salt();
        derived_key_ = derive_key(password, salt_);
        file_path_ = path;

        // Create temp SQLite file
        tmp_db_path_ = fs::temp_directory_path() / ("anyarks_" + std::to_string(getpid()) + ".db");
        open_db();
        init_schema();

        // Save to encrypted file
        close_db();
        write_encrypted_file(path);
        open_db();
        dirty_ = false;

        return {{"ok", true}};
    });

    // ── ks:open — open an existing encrypted database ───────────────────────
    cmds.add("ks:open", [this](const json& args) -> json {
        std::string path     = args.at("path");
        std::string password = args.at("password");

        close_db();
        cleanup_tmp();

        read_encrypted_file(path, password);
        open_db();
        dirty_ = false;

        return {{"ok", true}};
    });

    // ── ks:save — save changes back to encrypted file ───────────────────────
    cmds.add("ks:save", [this](const json& /*args*/) -> json {
        if (!db_ || file_path_.empty())
            throw std::runtime_error("No database open");

        // Flush WAL to main DB file before reading
        sqlite3_exec(db_, "PRAGMA wal_checkpoint(TRUNCATE);", nullptr, nullptr, nullptr);

        write_encrypted_file(file_path_);
        dirty_ = false;
        return {{"ok", true}};
    });

    // ── ks:close — close the database ───────────────────────────────────────
    cmds.add("ks:close", [this](const json& /*args*/) -> json {
        close_db();
        cleanup_tmp();
        derived_key_.clear();
        salt_.clear();
        file_path_.clear();
        dirty_ = false;
        return {{"ok", true}};
    });

    // ── ks:lock — close DB but keep file path ───────────────────────────────
    cmds.add("ks:lock", [this](const json& /*args*/) -> json {
        if (!file_path_.empty() && db_ && dirty_) {
            sqlite3_exec(db_, "PRAGMA wal_checkpoint(TRUNCATE);", nullptr, nullptr, nullptr);
            write_encrypted_file(file_path_);
        }
        close_db();
        cleanup_tmp();
        derived_key_.clear();
        salt_.clear();
        dirty_ = false;
        // Keep file_path_ so frontend can re-prompt password
        return {{"ok", true}, {"path", file_path_}};
    });

    // ── ks:change_password ──────────────────────────────────────────────────
    cmds.add("ks:change_password", [this](const json& args) -> json {
        if (!db_) throw std::runtime_error("No database open");
        std::string old_pw = args.at("oldPassword");
        std::string new_pw = args.at("newPassword");

        // Verify old password
        auto test_key = derive_key(old_pw, salt_);
        if (test_key != derived_key_)
            throw std::runtime_error("Current password is incorrect");

        // Generate new salt and derive new key
        salt_ = generate_salt();
        derived_key_ = derive_key(new_pw, salt_);

        // Re-encrypt
        sqlite3_exec(db_, "PRAGMA wal_checkpoint(TRUNCATE);", nullptr, nullptr, nullptr);
        write_encrypted_file(file_path_);
        dirty_ = false;

        return {{"ok", true}};
    });

    // ── ks:groups — list all groups ─────────────────────────────────────────
    cmds.add("ks:groups", [this](const json& /*args*/) -> json {
        if (!db_) throw std::runtime_error("No database open");
        return {{"groups", groups_to_json(db_)}};
    });

    // ── ks:create_group ─────────────────────────────────────────────────────
    cmds.add("ks:create_group", [this](const json& args) -> json {
        if (!db_) throw std::runtime_error("No database open");
        std::string name = args.value("name", "New Group");
        int parent_id = args.value("parentId", 1); // default to root

        sqlite3_stmt* stmt = nullptr;
        sqlite3_prepare_v2(db_, "INSERT INTO groups (parent_id, name) VALUES (?, ?)",
                           -1, &stmt, nullptr);
        sqlite3_bind_int(stmt, 1, parent_id);
        sqlite3_bind_text(stmt, 2, name.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_step(stmt);
        int id = static_cast<int>(sqlite3_last_insert_rowid(db_));
        sqlite3_finalize(stmt);
        dirty_ = true;

        return {{"id", id}};
    });

    // ── ks:rename_group ─────────────────────────────────────────────────────
    cmds.add("ks:rename_group", [this](const json& args) -> json {
        if (!db_) throw std::runtime_error("No database open");
        int id = args.at("id");
        std::string name = args.at("name");

        sqlite3_stmt* stmt = nullptr;
        sqlite3_prepare_v2(db_, "UPDATE groups SET name = ?, updated_at = datetime('now') WHERE id = ?",
                           -1, &stmt, nullptr);
        sqlite3_bind_text(stmt, 1, name.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int(stmt, 2, id);
        sqlite3_step(stmt);
        sqlite3_finalize(stmt);
        dirty_ = true;

        return {{"ok", true}};
    });

    // ── ks:delete_group ─────────────────────────────────────────────────────
    cmds.add("ks:delete_group", [this](const json& args) -> json {
        if (!db_) throw std::runtime_error("No database open");
        int id = args.at("id");
        if (id == 1) throw std::runtime_error("Cannot delete root group");

        sqlite3_stmt* stmt = nullptr;
        // Move child entries to root before deleting group
        sqlite3_prepare_v2(db_, "UPDATE entries SET group_id = 1 WHERE group_id = ?",
                           -1, &stmt, nullptr);
        sqlite3_bind_int(stmt, 1, id);
        sqlite3_step(stmt);
        sqlite3_finalize(stmt);

        // Move child groups to parent of deleted group
        sqlite3_prepare_v2(db_,
            "UPDATE groups SET parent_id = (SELECT parent_id FROM groups WHERE id = ?) WHERE parent_id = ?",
            -1, &stmt, nullptr);
        sqlite3_bind_int(stmt, 1, id);
        sqlite3_bind_int(stmt, 2, id);
        sqlite3_step(stmt);
        sqlite3_finalize(stmt);

        sqlite3_prepare_v2(db_, "DELETE FROM groups WHERE id = ?", -1, &stmt, nullptr);
        sqlite3_bind_int(stmt, 1, id);
        sqlite3_step(stmt);
        sqlite3_finalize(stmt);
        dirty_ = true;

        return {{"ok", true}};
    });

    // ── ks:move_group ───────────────────────────────────────────────────────
    cmds.add("ks:move_group", [this](const json& args) -> json {
        if (!db_) throw std::runtime_error("No database open");
        int id = args.at("id");
        int new_parent = args.value("newParentId", 1);

        sqlite3_stmt* stmt = nullptr;
        sqlite3_prepare_v2(db_,
            "UPDATE groups SET parent_id = ?, updated_at = datetime('now') WHERE id = ?",
            -1, &stmt, nullptr);
        sqlite3_bind_int(stmt, 1, new_parent);
        sqlite3_bind_int(stmt, 2, id);
        sqlite3_step(stmt);
        sqlite3_finalize(stmt);
        dirty_ = true;

        return {{"ok", true}};
    });

    // ── ks:entries — list entries for a group (or all) ──────────────────────
    cmds.add("ks:entries", [this](const json& args) -> json {
        if (!db_) throw std::runtime_error("No database open");
        bool all = !args.contains("groupId") || args["groupId"].is_null();
        int gid = args.value("groupId", 0);
        return {{"entries", entries_for_group(db_, gid, all)}};
    });

    // ── ks:get_entry ────────────────────────────────────────────────────────
    cmds.add("ks:get_entry", [this](const json& args) -> json {
        if (!db_) throw std::runtime_error("No database open");
        int id = args.at("id");

        sqlite3_stmt* stmt = nullptr;
        sqlite3_prepare_v2(db_,
            "SELECT id, group_id, title, username, password, url, notes, expires_at, icon_data, created_at, updated_at "
            "FROM entries WHERE id = ?", -1, &stmt, nullptr);
        sqlite3_bind_int(stmt, 1, id);

        json entry;
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            entry = entry_row_to_json(stmt);
            entry["attachments"] = attachments_for_entry(db_, id);
        }
        sqlite3_finalize(stmt);

        if (entry.is_null()) throw std::runtime_error("Entry not found");
        return {{"entry", entry}};
    });

    // ── ks:create_entry ─────────────────────────────────────────────────────
    cmds.add("ks:create_entry", [this](const json& args) -> json {
        if (!db_) throw std::runtime_error("No database open");

        int gid       = args.value("groupId", 1);
        std::string title    = args.value("title", "");
        std::string username = args.value("username", "");
        std::string password = args.value("password", "");
        std::string url      = args.value("url", "");
        std::string notes    = args.value("notes", "");
        std::string expires  = args.value("expiresAt", "");

        sqlite3_stmt* stmt = nullptr;
        sqlite3_prepare_v2(db_,
            "INSERT INTO entries (group_id, title, username, password, url, notes, expires_at) "
            "VALUES (?, ?, ?, ?, ?, ?, ?)", -1, &stmt, nullptr);
        sqlite3_bind_int(stmt, 1, gid);
        sqlite3_bind_text(stmt, 2, title.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 3, username.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 4, password.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 5, url.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 6, notes.c_str(), -1, SQLITE_TRANSIENT);
        if (expires.empty()) sqlite3_bind_null(stmt, 7);
        else sqlite3_bind_text(stmt, 7, expires.c_str(), -1, SQLITE_TRANSIENT);

        sqlite3_step(stmt);
        int id = static_cast<int>(sqlite3_last_insert_rowid(db_));
        sqlite3_finalize(stmt);
        dirty_ = true;

        return {{"id", id}};
    });

    // ── ks:update_entry ─────────────────────────────────────────────────────
    cmds.add("ks:update_entry", [this](const json& args) -> json {
        if (!db_) throw std::runtime_error("No database open");
        int id = args.at("id");

        // Build dynamic UPDATE
        std::vector<std::string> sets;
        auto maybe = [&](const char* col, const char* field) {
            if (args.contains(field)) sets.push_back(std::string(col) + " = :" + field);
        };
        maybe("title", "title");
        maybe("username", "username");
        maybe("password", "password");
        maybe("url", "url");
        maybe("notes", "notes");
        maybe("expires_at", "expiresAt");
        maybe("group_id", "groupId");

        if (sets.empty()) return {{"ok", true}};

        sets.push_back("updated_at = datetime('now')");
        std::string sql = "UPDATE entries SET ";
        for (size_t i = 0; i < sets.size(); ++i) {
            if (i) sql += ", ";
            sql += sets[i];
        }
        sql += " WHERE id = ?";

        sqlite3_stmt* stmt = nullptr;
        sqlite3_prepare_v2(db_, sql.c_str(), -1, &stmt, nullptr);

        // Bind named params
        int idx = 1;
        auto bind_if = [&](const char* field) {
            if (args.contains(field)) {
                if (args[field].is_null())
                    sqlite3_bind_null(stmt, idx);
                else if (args[field].is_number_integer())
                    sqlite3_bind_int(stmt, idx, args[field].get<int>());
                else
                    sqlite3_bind_text(stmt, idx, args[field].get<std::string>().c_str(), -1, SQLITE_TRANSIENT);
                idx++;
            }
        };
        bind_if("title");
        bind_if("username");
        bind_if("password");
        bind_if("url");
        bind_if("notes");
        bind_if("expiresAt");
        bind_if("groupId");

        // updated_at is auto, skip
        // WHERE id = ?
        sqlite3_bind_int(stmt, idx, id);
        sqlite3_step(stmt);
        sqlite3_finalize(stmt);
        dirty_ = true;

        return {{"ok", true}};
    });

    // ── ks:delete_entry ─────────────────────────────────────────────────────
    cmds.add("ks:delete_entry", [this](const json& args) -> json {
        if (!db_) throw std::runtime_error("No database open");
        int id = args.at("id");

        sqlite3_stmt* stmt = nullptr;
        sqlite3_prepare_v2(db_, "DELETE FROM entries WHERE id = ?", -1, &stmt, nullptr);
        sqlite3_bind_int(stmt, 1, id);
        sqlite3_step(stmt);
        sqlite3_finalize(stmt);
        dirty_ = true;

        return {{"ok", true}};
    });

    // ── ks:move_entry ───────────────────────────────────────────────────────
    cmds.add("ks:move_entry", [this](const json& args) -> json {
        if (!db_) throw std::runtime_error("No database open");
        int id = args.at("id");
        int gid = args.value("groupId", 1);

        sqlite3_stmt* stmt = nullptr;
        sqlite3_prepare_v2(db_,
            "UPDATE entries SET group_id = ?, updated_at = datetime('now') WHERE id = ?",
            -1, &stmt, nullptr);
        sqlite3_bind_int(stmt, 1, gid);
        sqlite3_bind_int(stmt, 2, id);
        sqlite3_step(stmt);
        sqlite3_finalize(stmt);
        dirty_ = true;

        return {{"ok", true}};
    });

    // ── ks:add_attachment ───────────────────────────────────────────────────
    cmds.add("ks:add_attachment", [this](const json& args) -> json {
        if (!db_) throw std::runtime_error("No database open");
        int entry_id      = args.at("entryId");
        std::string name  = args.at("name");
        std::string b64   = args.at("dataBase64");
        std::string mime  = args.value("mimeType", "application/octet-stream");

        auto data = base64_decode(b64);

        sqlite3_stmt* stmt = nullptr;
        sqlite3_prepare_v2(db_,
            "INSERT INTO attachments (entry_id, name, data, mime_type, size) VALUES (?, ?, ?, ?, ?)",
            -1, &stmt, nullptr);
        sqlite3_bind_int(stmt, 1, entry_id);
        sqlite3_bind_text(stmt, 2, name.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_blob(stmt, 3, data.data(), static_cast<int>(data.size()), SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 4, mime.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int(stmt, 5, static_cast<int>(data.size()));
        sqlite3_step(stmt);
        int id = static_cast<int>(sqlite3_last_insert_rowid(db_));
        sqlite3_finalize(stmt);
        dirty_ = true;

        return {{"id", id}};
    });

    // ── ks:get_attachment ───────────────────────────────────────────────────
    cmds.add("ks:get_attachment", [this](const json& args) -> json {
        if (!db_) throw std::runtime_error("No database open");
        int id = args.at("id");

        sqlite3_stmt* stmt = nullptr;
        sqlite3_prepare_v2(db_, "SELECT name, data, mime_type FROM attachments WHERE id = ?",
                           -1, &stmt, nullptr);
        sqlite3_bind_int(stmt, 1, id);

        json result;
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            result["name"] = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
            auto blob = static_cast<const uint8_t*>(sqlite3_column_blob(stmt, 1));
            int blob_len = sqlite3_column_bytes(stmt, 1);
            result["dataBase64"] = base64_encode(blob, blob_len);
            result["mimeType"] = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2));
        }
        sqlite3_finalize(stmt);

        if (result.is_null()) throw std::runtime_error("Attachment not found");
        return result;
    });

    // ── ks:delete_attachment ────────────────────────────────────────────────
    cmds.add("ks:delete_attachment", [this](const json& args) -> json {
        if (!db_) throw std::runtime_error("No database open");
        int id = args.at("id");

        sqlite3_stmt* stmt = nullptr;
        sqlite3_prepare_v2(db_, "DELETE FROM attachments WHERE id = ?", -1, &stmt, nullptr);
        sqlite3_bind_int(stmt, 1, id);
        sqlite3_step(stmt);
        sqlite3_finalize(stmt);
        dirty_ = true;

        return {{"ok", true}};
    });

    // ── ks:set_icon ─────────────────────────────────────────────────────────
    cmds.add("ks:set_icon", [this](const json& args) -> json {
        if (!db_) throw std::runtime_error("No database open");
        int entry_id = args.at("entryId");
        std::string b64  = args.at("dataBase64");
        std::string mime = args.value("mimeType", "image/png");

        auto data = base64_decode(b64);

        sqlite3_stmt* stmt = nullptr;
        sqlite3_prepare_v2(db_,
            "UPDATE entries SET icon_data = ?, icon_mime = ?, updated_at = datetime('now') WHERE id = ?",
            -1, &stmt, nullptr);
        sqlite3_bind_blob(stmt, 1, data.data(), static_cast<int>(data.size()), SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 2, mime.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int(stmt, 3, entry_id);
        sqlite3_step(stmt);
        sqlite3_finalize(stmt);
        dirty_ = true;

        return {{"ok", true}};
    });

    // ── ks:get_icon ─────────────────────────────────────────────────────────
    cmds.add("ks:get_icon", [this](const json& args) -> json {
        if (!db_) throw std::runtime_error("No database open");
        int entry_id = args.at("entryId");

        sqlite3_stmt* stmt = nullptr;
        sqlite3_prepare_v2(db_, "SELECT icon_data, icon_mime FROM entries WHERE id = ?",
                           -1, &stmt, nullptr);
        sqlite3_bind_int(stmt, 1, entry_id);

        json result;
        if (sqlite3_step(stmt) == SQLITE_ROW && sqlite3_column_type(stmt, 0) != SQLITE_NULL) {
            auto blob = static_cast<const uint8_t*>(sqlite3_column_blob(stmt, 0));
            int blob_len = sqlite3_column_bytes(stmt, 0);
            result["dataBase64"] = base64_encode(blob, blob_len);
            result["mimeType"]   = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1) ?: reinterpret_cast<const unsigned char*>("image/png"));
        }
        sqlite3_finalize(stmt);

        if (result.is_null()) return {{"dataBase64", nullptr}};
        return result;
    });

    // ── ks:status — check if DB is open, dirty, etc. ────────────────────────
    cmds.add("ks:status", [this](const json& /*args*/) -> json {
        return {
            {"open", db_ != nullptr},
            {"dirty", dirty_},
            {"path", file_path_},
            {"locked", !file_path_.empty() && db_ == nullptr}
        };
    });

    // ── ks:search — full-text search across entries ─────────────────────────
    cmds.add("ks:search", [this](const json& args) -> json {
        if (!db_) throw std::runtime_error("No database open");
        std::string query = args.at("query");
        std::string like = "%" + query + "%";

        sqlite3_stmt* stmt = nullptr;
        sqlite3_prepare_v2(db_,
            "SELECT id, group_id, title, username, password, url, notes, expires_at, icon_data, created_at, updated_at "
            "FROM entries WHERE title LIKE ? OR username LIKE ? OR url LIKE ? OR notes LIKE ? "
            "ORDER BY title", -1, &stmt, nullptr);
        for (int i = 1; i <= 4; ++i)
            sqlite3_bind_text(stmt, i, like.c_str(), -1, SQLITE_TRANSIENT);

        json entries = json::array();
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            entries.push_back(entry_row_to_json(stmt));
        }
        sqlite3_finalize(stmt);
        return {{"entries", entries}};
    });

    std::cout << "[KeystorePlugin] Initialized — ks:new, ks:open, ks:save, ks:close, ks:lock, "
              << "ks:groups, ks:entries, ks:search, ks:*_group, ks:*_entry, ks:*_attachment, ks:*_icon"
              << std::endl;
}

} // namespace keystore
