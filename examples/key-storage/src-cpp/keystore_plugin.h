#pragma once

// KeystorePlugin — Encrypted SQLite key storage
//
// Encryption:
//   Master password → PBKDF2-HMAC-SHA256 (100 000 iterations, 32-byte salt)
//   → AES-256-GCM encryption of the SQLite database file
//
// File format:
//   bytes  0–7     magic: "ANYARKS\0"
//   bytes  8–11    uint32 LE: version (1)
//   bytes 12–15    uint32 LE: PBKDF2 iterations
//   bytes 16–47    32 bytes: PBKDF2 salt
//   bytes 48–59    12 bytes: AES-GCM IV/nonce
//   bytes 60–75    16 bytes: AES-GCM authentication tag
//   bytes 76–79    uint32 LE: encrypted payload length
//   bytes 80+      encrypted SQLite database
//
// Commands:
//   ks:new       { path, password }                → { ok }
//   ks:open      { path, password }                → { ok }
//   ks:save      {}                                → { ok }
//   ks:close     {}                                → { ok }
//   ks:lock      {}                                → { ok }
//   ks:change_password { oldPassword, newPassword } → { ok }
//
//   ks:groups         {}                     → { groups: [...] }
//   ks:create_group   { parentId?, name }    → { id }
//   ks:rename_group   { id, name }           → { ok }
//   ks:delete_group   { id }                 → { ok }
//   ks:move_group     { id, newParentId? }   → { ok }
//
//   ks:entries        { groupId? }           → { entries: [...] }
//   ks:get_entry      { id }                 → { entry }
//   ks:create_entry   { groupId?, ... }      → { id }
//   ks:update_entry   { id, ... }            → { ok }
//   ks:delete_entry   { id }                 → { ok }
//   ks:move_entry     { id, groupId? }       → { ok }
//
//   ks:add_attachment    { entryId, name, dataBase64 } → { id }
//   ks:get_attachment    { id }                        → { name, dataBase64 }
//   ks:delete_attachment { id }                        → { ok }
//
//   ks:set_icon         { entryId, dataBase64 }       → { ok }
//   ks:get_icon         { entryId }                   → { dataBase64 }

#include <anyar/plugin.h>

#include <string>
#include <vector>
#include <memory>

struct sqlite3;  // forward declaration

namespace keystore {

class KeystorePlugin : public anyar::IAnyarPlugin {
public:
    std::string name() const override { return "keystore"; }
    void initialize(anyar::PluginContext& ctx) override;
    void shutdown() override;

private:
    // ── Database state ──────────────────────────────────────────────────────
    sqlite3*    db_         = nullptr;
    std::string file_path_;             // path to .anyarks file on disk
    std::string tmp_db_path_;           // temporary decrypted SQLite file
    std::vector<uint8_t> derived_key_;  // 32-byte AES key (kept in memory while open)
    std::vector<uint8_t> salt_;         // 32-byte PBKDF2 salt
    bool        dirty_      = false;    // unsaved changes

    // ── Crypto helpers ──────────────────────────────────────────────────────
    static std::vector<uint8_t> derive_key(const std::string& password,
                                           const std::vector<uint8_t>& salt,
                                           int iterations = 100000);
    static std::vector<uint8_t> generate_salt(int bytes = 32);
    static std::vector<uint8_t> generate_iv(int bytes = 12);

    /// Encrypt plaintext with AES-256-GCM. Returns {ciphertext, tag(16), iv(12)}.
    static std::tuple<std::vector<uint8_t>,
                      std::vector<uint8_t>,
                      std::vector<uint8_t>>
    encrypt_aes_gcm(const std::vector<uint8_t>& key,
                    const std::vector<uint8_t>& plaintext);

    /// Decrypt ciphertext with AES-256-GCM. Throws on auth failure.
    static std::vector<uint8_t>
    decrypt_aes_gcm(const std::vector<uint8_t>& key,
                    const std::vector<uint8_t>& ciphertext,
                    const std::vector<uint8_t>& iv,
                    const std::vector<uint8_t>& tag);

    // ── File I/O ────────────────────────────────────────────────────────────
    void write_encrypted_file(const std::string& path);
    void read_encrypted_file(const std::string& path, const std::string& password);

    // ── SQLite helpers ──────────────────────────────────────────────────────
    void open_db();
    void close_db();
    void init_schema();
    void cleanup_tmp();

    static constexpr const char* MAGIC = "ANYARKS";
    static constexpr uint32_t VERSION  = 1;
    static constexpr int PBKDF2_ITERS  = 100000;
};

} // namespace keystore
