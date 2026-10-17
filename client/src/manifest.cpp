#include <baker_liveops/manifest.hpp>

#include <sodium.h>

#include <charconv>

namespace baker_liveops {
namespace {

constexpr std::string_view kHeader = "baker-liveops manifest 1";
constexpr size_t kMaxManifest = 1 << 20;

bool ready() {
    static const bool ok = sodium_init() >= 0;
    return ok;
}

bool is_hex(std::string_view s, size_t n) {
    if (s.size() != n) return false;
    for (char c : s)
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
    return true;
}

bool is_name(std::string_view s) {
    if (s.empty() || s.size() > 128) return false;
    for (char c : s)
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
              c == '_' || c == '-' || c == '.'))
            return false;
    return true;
}

bool to_u64(std::string_view s, uint64_t& out) {
    auto [p, ec] = std::from_chars(s.data(), s.data() + s.size(), out);
    return ec == std::errc() && p == s.data() + s.size() && !s.empty();
}

}  // namespace

std::string to_hex(std::span<const unsigned char> bytes) {
    static constexpr char digits[] = "0123456789abcdef";
    std::string s;
    s.reserve(bytes.size() * 2);
    for (unsigned char b : bytes) {
        s += digits[b >> 4];
        s += digits[b & 15];
    }
    return s;
}

bool from_hex(std::string_view hex, std::span<unsigned char> out) {
    if (!is_hex(hex, out.size() * 2)) return false;
    auto nibble = [](char c) { return c <= '9' ? c - '0' : c - 'a' + 10; };
    for (size_t i = 0; i < out.size(); ++i)
        out[i] = static_cast<unsigned char>(nibble(hex[2 * i]) << 4 | nibble(hex[2 * i + 1]));
    return true;
}

std::string sha256_hex(std::string_view bytes) {
    (void)ready();
    unsigned char digest[crypto_hash_sha256_BYTES];
    crypto_hash_sha256(digest, reinterpret_cast<const unsigned char*>(bytes.data()), bytes.size());
    return to_hex(digest);
}

std::string PublicKey::id() const { return to_hex(std::span(bytes).first(8)); }

std::optional<PublicKey> parse_public_key(std::string_view hex) {
    while (!hex.empty() && (hex.back() == '\n' || hex.back() == '\r' || hex.back() == ' '))
        hex.remove_suffix(1);
    PublicKey k;
    if (!from_hex(hex, k.bytes)) return std::nullopt;
    return k;
}

std::optional<Manifest> parse_manifest(std::string_view text, std::string& error) {
    auto fail = [&](std::string why, size_t line) {
        error = "manifest line " + std::to_string(line) + ": " + std::move(why);
        return std::nullopt;
    };
    if (text.size() > kMaxManifest) return fail("larger than 1 MB", 0);
    Manifest m;
    bool header = false, sequence = false;
    size_t n = 0;
    while (!text.empty()) {
        const size_t eol = text.find('\n');
        if (eol == std::string_view::npos) return fail("does not end in a newline", n + 1);
        const std::string_view line = text.substr(0, eol);
        text.remove_prefix(eol + 1);
        ++n;
        if (!header) {
            if (line != kHeader) return fail("not a baker-liveops manifest, version 1", n);
            header = true;
            continue;
        }
        const size_t sp = line.find(' ');
        if (sp == std::string_view::npos) return fail("expected a name and a value", n);
        const std::string_view key = line.substr(0, sp), value = line.substr(sp + 1);
        if (!m.signature.empty()) return fail("follows the signature", n);
        Entry* e = m.overlays.empty() ? nullptr : &m.overlays.back();
        auto need_entry = [&] { return e != nullptr && m.key_id.empty(); };
        if (key == "environment" && m.overlays.empty() && is_name(value)) {
            m.environment = value;
        } else if (key == "sequence" && m.overlays.empty() && to_u64(value, m.sequence)) {
            sequence = true;
        } else if (key == "overlay" && is_name(value) && m.key_id.empty()) {
            for (const Entry& o : m.overlays)
                if (o.name == value) return fail("overlay " + std::string(value) + " twice", n);
            m.overlays.push_back(Entry{.name = std::string(value)});
        } else if (key == "version" && need_entry() && is_name(value)) {
            e->version = value;
        } else if (key == "requires" && need_entry() && !value.empty() && value.size() < 256) {
            e->requires_.emplace_back(value);
        } else if (key == "sha256" && need_entry() && is_hex(value, 64)) {
            e->sha256 = value;
        } else if (key == "size" && need_entry() && to_u64(value, e->size)) {
        } else if (key == "policy" && need_entry() && is_name(value)) {
            e->policy = value;
        } else if (key == "key" && m.key_id.empty() && is_hex(value, 16)) {
            m.key_id = value;
        } else if (key == "signature" && !m.key_id.empty() && is_hex(value, 128)) {
            m.signature = value;
        } else {
            return fail("unexpected or malformed '" + std::string(key) + "'", n);
        }
    }
    if (m.environment.empty() || !sequence) return fail("no environment or sequence", n);
    for (const Entry& e : m.overlays)
        if (e.version.empty() || e.sha256.empty() || e.policy.empty() || e.size == 0)
            return fail("overlay " + e.name + " lacks a version, sha256, size, or policy", n);
    if (m.signature.empty()) return fail("no key and signature", n);
    return m;
}

std::string signed_part(const Manifest& m) {
    std::string s(kHeader);
    s += "\nenvironment " + m.environment + "\nsequence " + std::to_string(m.sequence) + "\n";
    for (const Entry& e : m.overlays) {
        s += "overlay " + e.name + "\nversion " + e.version + "\n";
        for (const std::string& r : e.requires_) s += "requires " + r + "\n";
        s += "sha256 " + e.sha256 + "\nsize " + std::to_string(e.size) + "\npolicy " + e.policy +
             "\n";
    }
    s += "key " + m.key_id + "\n";
    return s;
}

std::string_view signed_part(std::string_view text) {
    const size_t at = text.rfind("\nsignature ");
    return at == std::string_view::npos ? std::string_view() : text.substr(0, at + 1);
}

std::optional<Manifest> verify_manifest(std::string_view text, std::span<const PublicKey> keys,
                                        std::string& error) {
    if (!ready()) return error = "libsodium did not initialize", std::nullopt;
    auto m = parse_manifest(text, error);
    if (!m) return std::nullopt;
    const PublicKey* key = nullptr;
    for (const PublicKey& k : keys)
        if (k.id() == m->key_id) key = &k;
    if (!key) return error = "manifest signed by an unknown key " + m->key_id, std::nullopt;
    unsigned char sig[crypto_sign_BYTES];
    const std::string_view body = signed_part(text);
    if (!from_hex(m->signature, sig) ||
        crypto_sign_verify_detached(sig, reinterpret_cast<const unsigned char*>(body.data()),
                                    body.size(), key->bytes.data()) != 0)
        return error = "manifest signature does not verify", std::nullopt;
    return m;
}

bool verify_overlay(const Entry& entry, std::string_view text, std::string& error) {
    if (text.size() != entry.size) {
        error = entry.name + ": " + std::to_string(text.size()) + " bytes, the manifest says " +
                std::to_string(entry.size);
        return false;
    }
    if (sha256_hex(text) != entry.sha256) {
        error = entry.name + ": SHA-256 does not match the manifest";
        return false;
    }
    return true;
}

}  // namespace baker_liveops
