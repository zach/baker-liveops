#include "publish.hpp"

#include <static/static.hpp>

#include <baker/parse.hpp>
#include <baker/std_interop.hpp>

#include <sodium.h>

#include <cstdio>
#include <cstdlib>

#if defined(_WIN32)
#define popen _popen
#define pclose _pclose
#else
#include <sys/wait.h>
#endif

namespace baker_liveops {

// --- Keys.

SecretKey generate_key(std::string& seed_hex) {
    if (sodium_init() < 0) std::abort();
    unsigned char seed[crypto_sign_SEEDBYTES];
    randombytes_buf(seed, sizeof seed);
    seed_hex = to_hex(seed);
    sodium_memzero(seed, sizeof seed);
    return *key_from_seed(seed_hex);
}

std::optional<SecretKey> key_from_seed(std::string_view seed_hex) {
    if (sodium_init() < 0) return std::nullopt;
    while (!seed_hex.empty() && (seed_hex.back() == '\n' || seed_hex.back() == '\r'))
        seed_hex.remove_suffix(1);
    unsigned char seed[crypto_sign_SEEDBYTES];
    if (!from_hex(seed_hex, seed)) return std::nullopt;
    SecretKey k;
    crypto_sign_seed_keypair(k.pub.bytes.data(), k.bytes.data(), seed);
    sodium_memzero(seed, sizeof seed);
    return k;
}

std::string sign(Manifest m, const SecretKey& key) {
    m.key_id = key.pub.id();
    const std::string body = signed_part(m);
    unsigned char sig[crypto_sign_BYTES];
    crypto_sign_detached(sig, nullptr, reinterpret_cast<const unsigned char*>(body.data()),
                         body.size(), key.bytes.data());
    return body + "signature " + to_hex(sig) + "\n";
}

// --- Backends.

namespace {

class StaticBackend final : public Backend {
public:
    explicit StaticBackend(std::filesystem::path root) : root_(std::move(root)) {}
    std::optional<std::string> get(std::string_view key, bool& failed) override {
        failed = false;
        return static_backend::read_file(root_ / key);
    }
    bool put(std::string_view key, std::string_view bytes) override {
        return static_backend::write_file(root_ / key, bytes);
    }

private:
    std::filesystem::path root_;
};

int exit_code(int status) {
#if defined(_WIN32)
    return status;
#else
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
#endif
}

class CommandBackend final : public Backend {
public:
    explicit CommandBackend(std::string command) : command_(std::move(command)) {}
    std::optional<std::string> get(std::string_view key, bool& failed) override {
        failed = true;
        std::FILE* p = popen((command_ + " get '" + std::string(key) + "'").c_str(), "r");
        if (!p) return std::nullopt;
        std::string out;
        char buf[4096];
        for (size_t n; (n = std::fread(buf, 1, sizeof buf, p)) > 0;) out.append(buf, n);
        const int code = exit_code(pclose(p));
        failed = code != 0 && code != 3;
        if (code != 0) return std::nullopt;
        return out;
    }
    bool put(std::string_view key, std::string_view bytes) override {
        std::FILE* p = popen((command_ + " put '" + std::string(key) + "'").c_str(), "w");
        if (!p) return false;
        const bool wrote = std::fwrite(bytes.data(), 1, bytes.size(), p) == bytes.size();
        return exit_code(pclose(p)) == 0 && wrote;
    }

private:
    std::string command_;
};

bool is_name(std::string_view s) {
    if (s.empty() || s.size() > 128) return false;
    for (char c : s)
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
              c == '_' || c == '-' || c == '.'))
            return false;
    return true;
}

}  // namespace

std::unique_ptr<Backend> open_backend(std::string_view spec, std::string& error) {
    if (spec.starts_with("static:")) return std::make_unique<StaticBackend>(spec.substr(7));
    if (spec.starts_with("cmd:")) return std::make_unique<CommandBackend>(std::string(spec.substr(4)));
    error = "a backend is static:DIR or cmd:PROGRAM, not '" + std::string(spec) + "'";
    return nullptr;
}

std::optional<std::string> BackendTransport::fetch_manifest(std::string_view environment) {
    bool failed = false;
    return backend_.get(static_backend::manifest_key(environment), failed);
}

std::optional<std::string> BackendTransport::fetch_overlay(std::string_view environment,
                                                           const Entry& entry) {
    bool failed = false;
    return backend_.get(static_backend::overlay_key(environment, entry), failed);
}

// --- The gate.

std::optional<Candidate> check_overlay(const baker::Config& base, const Game& game,
                                       std::string text, std::string_view policy,
                                       std::vector<std::string>& problems,
                                       std::string_view prefix) {
    const GamePolicy* p = game.policy(policy);
    if (!p) problems.push_back("the game has no policy named " + std::string(policy));

    // What the overlay declares, read as the game reads it: the stack's session, text only.
    baker::LoadSession session = base.read_session();
    session.text_only = true;
    session.clear_autorun();
    const size_t packs_before = session.packs().size();
    auto tree = baker::read_string(text, "overlay.dta", session);
    if (!tree) {
        problems.emplace_back(baker::to_string(tree.error()));
        return std::nullopt;
    }
    const baker::Pack* pack = session.packs().size() > packs_before ? &session.packs().back() : nullptr;
    if (!pack || pack->version.str().empty()) {
        problems.emplace_back(
            "an overlay declares itself a pack with a version, first: "
            "(pack NAME (level L) (version \"N\") (requires (base \"V\")))");
        return std::nullopt;
    }
    Candidate c;
    c.entry.name = pack->name.str();
    c.entry.version = pack->version.str();
    for (size_t i = 0; i < pack->requires_packs.size(); ++i) {
        std::string r(pack->requires_packs[i].str());
        if (i < pack->requires_versions.size() && !pack->requires_versions[i].str().empty())
            r += " " + std::string(pack->requires_versions[i].str());
        c.entry.requires_.push_back(std::move(r));
    }
    if (!is_name(c.entry.name) || !is_name(c.entry.version))
        problems.push_back("pack name and version are letters, digits, '_', '-' and '.'");
    if (const baker::Origin* o = session.origin_of(pack->file)) c.origin = o->source.str();
    c.entry.sha256 = sha256_hex(text);
    c.entry.size = text.size();
    c.entry.policy = policy;
    if (p) {
        auto report = base.check_layer_text(std::string(prefix) + c.entry.name + ".dta", text, p->policy);
        if (!report) {
            problems.emplace_back(baker::to_string(report.error()));
        } else {
            for (const baker::Error& e : report->refusals) problems.emplace_back(baker::to_string(e));
            c.report = std::move(*report);
        }
    }
    c.text = std::move(text);
    if (!problems.empty()) return std::nullopt;
    return c;
}

Manifest with_overlay(std::optional<Manifest> current, std::string_view environment,
                      const Entry& entry) {
    Manifest m = current ? std::move(*current) : Manifest{};
    m.environment = environment;
    m.sequence += 1;
    m.signature.clear();
    for (Entry& e : m.overlays)
        if (e.name == entry.name) return e = entry, m;
    m.overlays.push_back(entry);
    return m;
}

bool without_overlay(Manifest& m, std::string_view name) {
    const size_t before = m.overlays.size();
    std::erase_if(m.overlays, [&](const Entry& e) { return e.name == name; });
    if (m.overlays.size() == before) return false;
    m.sequence += 1;
    return true;
}

std::string describe(const baker::LayerReport& report) {
    std::string s;
    for (const baker::LayerEffectEntry& e : report.effect)
        s += "  " + std::string(e.path.str()) + ": " +
             (e.below ? baker::to_string(*e.below) : std::string("(none)")) + " -> " +
             baker::to_string(e.value) + "\n";
    for (const baker::Error& e : report.warnings) s += "  warning: " + std::string(baker::to_string(e)) + "\n";
    for (const baker::Error& e : report.refusals) s += "  refused: " + std::string(baker::to_string(e)) + "\n";
    return s;
}

}  // namespace baker_liveops
