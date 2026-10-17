// baker-liveops: checks, signs, uploads, withdraws, and retires published overlays.
#include "publish.hpp"
#include "stack.hpp"

#include "../retire/retire.hpp"

#include <static/static.hpp>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#if !defined(_WIN32)
#include <sys/stat.h>
#endif

namespace bl = baker_liveops;
namespace fs = std::filesystem;

namespace {

constexpr const char* kUsage = R"(usage: baker-liveops COMMAND [options]

  keygen --out FILE           a new signing key (its seed, in hex) into FILE, which must not
                              exist; prints the public key a game trusts
  pubkey [--key FILE]         the public key of a signing key
  check   --game G --base DIR --overlay F --policy P [--stack BIN --stack-data DIR]
                              what the overlay changes, and whether the game would take it
  publish --game G --base DIR --overlay F --policy P --backend B --env E [--key FILE]
          [--trust PUBKEY]... [--stack BIN --stack-data DIR] [--dry-run]
                              check, then sign and upload: overlay first, manifest last
  withdraw NAME --backend B --env E [--key FILE] [--trust PUBKEY]...
                              a manifest without NAME: clients remove it at their next boundary
  show --backend B --env E --trust PUBKEY...
                              the environment's manifest, verified
  retire --game G --base DIR --overlay F [--into LAYER] --patch OUT
  retire --stack BIN --stack-data DIR --overlay F [--into LAYER] --patch OUT
                              the overlay's effect written into the base as a unified diff;
                              writes nothing else, and pushes nothing

  G is the game's live-ops description (liveops.dta); B is static:DIR or cmd:PROGRAM.
  The signing key comes from --key FILE, or LIVEOPS_SIGNING_KEY (the seed, in hex). Keep it
  out of repositories and logs; it is never printed.
  Exit codes: 0 done; 1 refused (the reasons are printed); 2 a usage or I/O error.
)";

struct Args {
    std::string command, positional;
    std::map<std::string, std::string> one;
    std::vector<std::string> trust;
    bool dry_run = false;

    [[nodiscard]] std::string get(const std::string& k) const {
        auto it = one.find(k);
        return it == one.end() ? std::string() : it->second;
    }
};

[[noreturn]] void usage(const std::string& why) {
    std::fprintf(stderr, "baker-liveops: %s\n\n%s", why.c_str(), kUsage);
    std::exit(2);
}

int fail(const std::string& why, int code = 2) {
    std::fprintf(stderr, "baker-liveops: %s\n", why.c_str());
    return code;
}

Args parse(int argc, char** argv) {
    if (argc < 2) usage("no command");
    Args a;
    a.command = argv[1];
    for (int i = 2; i < argc; ++i) {
        const std::string s = argv[i];
        if (s == "--dry-run") {
            a.dry_run = true;
        } else if (s.starts_with("--") && i + 1 < argc) {
            if (s == "--trust") a.trust.emplace_back(argv[++i]);
            else a.one[s.substr(2)] = argv[++i];
        } else if (!s.starts_with("--") && a.positional.empty()) {
            a.positional = s;
        } else {
            usage("unexpected " + s);
        }
    }
    return a;
}

std::string need(const Args& a, const char* key) {
    std::string v = a.get(key);
    if (v.empty()) usage(std::string("--") + key + " is required for " + a.command);
    return v;
}

std::optional<bl::SecretKey> signing_key(const Args& a) {
    std::string seed;
    if (const std::string file = a.get("key"); !file.empty()) {
        auto text = bl::static_backend::read_file(file);
        if (!text) return fail("cannot read the key " + file), std::nullopt;
        seed = *text;
    } else if (const char* env = std::getenv("LIVEOPS_SIGNING_KEY")) {
        seed = env;
    } else {
        return fail("no signing key: --key FILE or LIVEOPS_SIGNING_KEY"), std::nullopt;
    }
    auto key = bl::key_from_seed(seed);
    if (!key) fail("the signing key is not a 32-byte seed in hex");
    return key;
}

std::vector<bl::PublicKey> trusted(const Args& a, const bl::SecretKey* own) {
    std::vector<bl::PublicKey> keys;
    if (own) keys.push_back(own->pub);
    for (const std::string& t : a.trust) {
        std::string hex = t;
        if (auto text = bl::static_backend::read_file(t)) hex = *text;  // a file, or the key
        if (auto k = bl::parse_public_key(hex)) keys.push_back(*k);
        else usage("--trust " + t + " is not a public key");
    }
    return keys;
}

// The environment's manifest as it stands, verified; nothing if there is none yet.
bool current_manifest(bl::Backend& backend, const std::string& env, std::span<const bl::PublicKey> keys,
                      std::optional<bl::Manifest>& out) {
    bool failed = false;
    auto text = backend.get(bl::static_backend::manifest_key(env), failed);
    if (failed) return fail("the backend did not answer for " + env), false;
    if (!text) return true;
    std::string error;
    out = bl::verify_manifest(*text, keys, error);
    if (!out) return fail("the current manifest of " + env + " does not verify: " + error), false;
    return true;
}

struct Base {
    bl::Game game;
    baker::Config cfg{{.tweaks = nullptr}};
};

std::unique_ptr<Base> load(const Args& a) {
    auto b = std::make_unique<Base>();
    std::string error;
    auto game = bl::load_game(need(a, "game"), error);
    if (!game) return fail(error), nullptr;
    b->game = std::move(*game);
    if (!bl::load_base(b->cfg, b->game, need(a, "base"), error)) return fail(error), nullptr;
    return b;
}

// The gate: the client's own check, then the stack's scenarios if given.
std::optional<bl::Candidate> gate(const Args& a, const Base& base) {
    const std::string file = need(a, "overlay");
    auto text = bl::static_backend::read_file(file);
    if (!text) return fail("cannot read " + file), std::nullopt;
    std::vector<std::string> problems;
    auto c = bl::check_overlay(base.cfg, base.game, std::move(*text), need(a, "policy"), problems);
    if (c) {
        std::printf("%s %s (policy %s, %llu bytes, sha256 %s)%s\n", c->entry.name.c_str(), c->entry.version.c_str(),
                    c->entry.policy.c_str(), static_cast<unsigned long long>(c->entry.size), c->entry.sha256.c_str(),
                    c->origin.empty() ? "" : (", origin \"" + c->origin + "\"").c_str());
        std::printf("%s", bl::describe(c->report).c_str());
        if (c->origin.empty())
            std::printf("  note: no (origin ...): source() will name the layer and file, not where it was made\n");
    }
    for (const std::string& p : problems) std::printf("  refused: %s\n", p.c_str());
    if (!c) return std::nullopt;
    if (const std::string stack = a.get("stack"); !stack.empty()) {
        const auto r = bl::run_scenarios(stack, need(a, "stack-data"), c->text);
        if (r.code != 0) {
            std::printf("  refused: %s\n%s\n",
                        r.code == 1 ? "an objective that passes on the base breaks with it" : "the scenarios did not run",
                        r.report.c_str());
            return std::nullopt;
        }
        std::printf("  scenarios: no objective broke\n");
    }
    return c;
}

int keygen(const Args& a) {
    const std::string out = need(a, "out");
    if (fs::exists(out)) return fail(out + " exists: a key is never overwritten");
    std::string seed;
    const bl::SecretKey key = bl::generate_key(seed);
    if (!bl::static_backend::write_file(out, seed + "\n")) return fail("cannot write " + out);
#if !defined(_WIN32)
    ::chmod(out.c_str(), 0600);
#endif
    std::printf("public key %s (id %s)\n", bl::to_hex(key.pub.bytes).c_str(), key.pub.id().c_str());
    return 0;
}

int publish(const Args& a) {
    auto base = load(a);
    if (!base) return 2;
    auto c = gate(a, *base);
    if (!c) return 1;
    const std::string env = need(a, "env");
    auto key = signing_key(a);
    if (!key) return 2;
    std::string error;
    auto backend = bl::open_backend(need(a, "backend"), error);
    if (!backend) usage(error);
    const auto keys = trusted(a, &*key);
    std::optional<bl::Manifest> current;
    if (!current_manifest(*backend, env, keys, current)) return 2;
    const bl::Manifest next = bl::with_overlay(std::move(current), env, c->entry);
    const std::string manifest = bl::sign(next, *key);
    if (a.dry_run) return std::printf("dry run: would upload\n%s", manifest.c_str()), 0;
    if (!backend->put(bl::static_backend::overlay_key(env, c->entry), c->text))
        return fail("could not upload the overlay");
    if (!backend->put(bl::static_backend::manifest_key(env), manifest)) return fail("could not upload the manifest");
    std::printf("published %s %s to %s, manifest sequence %llu\n", c->entry.name.c_str(), c->entry.version.c_str(),
                env.c_str(), static_cast<unsigned long long>(next.sequence));
    return 0;
}

int withdraw(const Args& a) {
    if (a.positional.empty()) usage("withdraw names the overlay");
    const std::string env = need(a, "env");
    auto key = signing_key(a);
    if (!key) return 2;
    std::string error;
    auto backend = bl::open_backend(need(a, "backend"), error);
    if (!backend) usage(error);
    std::optional<bl::Manifest> current;
    if (!current_manifest(*backend, env, trusted(a, &*key), current)) return 2;
    if (!current || !bl::without_overlay(*current, a.positional))
        return fail(env + " has no overlay " + a.positional, 1);
    if (!backend->put(bl::static_backend::manifest_key(env), bl::sign(*current, *key)))
        return fail("could not upload the manifest");
    std::printf("withdrew %s from %s, manifest sequence %llu\n", a.positional.c_str(), env.c_str(),
                static_cast<unsigned long long>(current->sequence));
    return 0;
}

int show(const Args& a) {
    std::string error;
    auto backend = bl::open_backend(need(a, "backend"), error);
    if (!backend) usage(error);
    const auto keys = trusted(a, nullptr);
    if (keys.empty()) usage("show verifies: --trust PUBKEY");
    std::optional<bl::Manifest> m;
    const std::string env = need(a, "env");
    if (!current_manifest(*backend, env, keys, m)) return 1;
    if (!m) return std::printf("%s has no manifest\n", env.c_str()), 0;
    std::printf("%s, sequence %llu, signed by %s\n", env.c_str(), static_cast<unsigned long long>(m->sequence),
                m->key_id.c_str());
    for (const bl::Entry& e : m->overlays)
        std::printf("  %s %s (policy %s, %llu bytes)\n", e.name.c_str(), e.version.c_str(), e.policy.c_str(),
                    static_cast<unsigned long long>(e.size));
    return 0;
}

int retire(const Args& a) {
    const std::string out = need(a, "patch"), into = a.get("into").empty() ? "default" : a.get("into");
    std::string patch, message;
    int code;
    if (const std::string stack = a.get("stack"); !stack.empty()) {
        auto text = bl::static_backend::read_file(need(a, "overlay"));
        if (!text) return fail("cannot read the overlay");
        code = bl::retire_with_stack(stack, need(a, "stack-data"), *text, into, patch, message);
    } else {
        std::string error;
        auto game = bl::load_game(need(a, "game"), error);
        if (!game) return fail(error);
        code = bl::retire_with_config(*game, need(a, "base"), need(a, "overlay"), into, patch, message);
    }
    std::printf("%s", message.c_str());
    if (code != 0) return code;
    if (!bl::static_backend::write_file(out, patch)) return fail("cannot write " + out);
    std::printf("wrote %s: apply it in the base checkout with git apply -p1 on a branch of its own, and send\n"
                "it for review. Once a build with it ships, withdraw the overlay.\n",
                out.c_str());
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    const Args a = parse(argc, argv);
    if (a.command == "keygen") return keygen(a);
    if (a.command == "pubkey") {
        auto key = signing_key(a);
        if (!key) return 2;
        return std::printf("%s\n", bl::to_hex(key->pub.bytes).c_str()), 0;
    }
    if (a.command == "check") {
        auto base = load(a);
        if (!base) return 2;
        return gate(a, *base) ? 0 : 1;
    }
    if (a.command == "publish") return publish(a);
    if (a.command == "withdraw") return withdraw(a);
    if (a.command == "show") return show(a);
    if (a.command == "retire") return retire(a);
    usage("unknown command " + a.command);
}
