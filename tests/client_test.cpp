// The client against the test game, with a directory standing in for storage. Runs in
// development and shipping builds alike.
#include "support.hpp"

#include <baker/config.hpp>

#include <cstdlib>
#include <map>

namespace {

struct Host {
    baker::Config cfg{{.live_layers = true, .tweaks = nullptr}};
    bl::Game game;
    std::vector<bl::NamedPolicy> policies;
    bl::static_backend::DirTransport transport;
    bl::static_backend::DirStore store;
    std::vector<std::string> log;
    bl::Client client;

    Host(const fs::path& cdn, const fs::path& user, std::span<const bl::PublicKey> keys,
         bl::ApplyMode mode = bl::ApplyMode::Path, const char* description = "tests/game/liveops.dta")
        : game(load(description)),
          policies(game.named()),
          transport(cdn),
          store(user),
          client(cfg, {.environment = "prod", .keys = keys, .policies = policies,
                       .transport = &transport, .store = &store, .mode = mode,
                       .log = [this](std::string_view l) { log.emplace_back(l); }}) {
        if (mode == bl::ApplyMode::Path) client.attach(cfg.session());
        std::string error;
        if (!bl::load_base(cfg, game, source_dir() / "tests/game", error)) std::printf("%s\n", error.c_str());
        cfg.freeze();  // the shipped build's boundary: only layers change from here
    }

    static bl::Game load(const char* description) {
        std::string error;
        auto g = bl::load_game(source_dir() / description, error);
        if (!g) std::printf("%s\n", error.c_str());
        return std::move(*g);
    }

    bool logged(std::string_view needle) const {
        for (const std::string& l : log)
            if (l.find(needle) != std::string::npos) return true;
        return false;
    }

    template <class T>
    T get(const char* path) const { return *cfg.get<T>(path); }
};

const FakePublisher::Item kBalance{"balance_2026_10", "3", overlay_text("balance_2026_10.dta"), "hotfix"};
const FakePublisher::Item kDoubleXp{"double_xp", "1", overlay_text("double_xp.dta"), "events"};

// A signed overlay is staged when it arrives and applies only at the host's boundary;
// source() names it, its version, and its origin.
void applies_at_boundary(bl::ApplyMode mode) {
    const fs::path dir = scratch("apply");
    FakePublisher pub(dir / "cdn");
    Host host(dir / "cdn", dir / "user", std::span(&pub.key.pub, 1), mode);
    pub.publish({kBalance});

    CHECK(host.client.restore());
    const uint64_t epoch = host.cfg.epoch();
    CHECK(host.client.refresh());
    CHECK(host.client.pending());
    CHECK(host.cfg.epoch() == epoch);  // staged: nothing changed yet
    CHECK(host.get<int64_t>("economy.gold_per_win") == 100);
    CHECK(host.client.report("balance_2026_10")->effect.size() == 2);

    CHECK(host.client.apply());  // the session boundary
    CHECK(!host.client.pending());
    CHECK(host.get<int64_t>("economy.gold_per_win") == 120);
    CHECK(host.get<double>("economy.shop_discount") == 0.1);
    auto s = host.cfg.source("economy.gold_per_win");
    CHECK(s && s->layer.str() == "balance_2026_10");
    CHECK(s && s->file.str().ends_with("liveops/balance_2026_10.dta"));
    if (mode == bl::ApplyMode::Path) CHECK(s && s->origin.str() == "ops/balance_2026_10.dta");
    const auto live = host.client.live();
    CHECK(live.size() == 1 && live[0].layer == "balance_2026_10" && live[0].version == "3");
    if (mode == bl::ApplyMode::Path) CHECK(live.size() == 1 && live[0].origin == "ops/balance_2026_10.dta");

    // A newer version replaces it at the next boundary.
    FakePublisher::Item v4 = kBalance;
    v4.version = "4";
    v4.text.replace(v4.text.find("\"3\""), 3, "\"4\"");
    v4.text.replace(v4.text.find("120"), 3, "130");
    pub.publish({v4});
    CHECK(host.client.refresh());
    CHECK(host.get<int64_t>("economy.gold_per_win") == 120);
    CHECK(host.client.apply());
    CHECK(host.get<int64_t>("economy.gold_per_win") == 130);
    CHECK(host.client.live().size() == 1 && host.client.live()[0].version == "4");
    CHECK(!host.client.refresh());  // the same manifest again: nothing new

    // Withdrawn from the manifest: removed at the next boundary.
    pub.publish({});
    CHECK(host.client.refresh());
    CHECK(host.client.apply());
    CHECK(host.get<int64_t>("economy.gold_per_win") == 100);
    CHECK(host.client.live().empty());
}

// Each of these is refused, and the Config is exactly as it was.
void refusals(bl::ApplyMode mode) {
    struct Case {
        const char* what;
        const char* file;
        const char* policy;
        const char* expect;  // in the log
    };
    const Case cases[] = {
        {"adds a command", "adds_command.dta", "hotfix", "data only"},
        {"outside its allow list", "outside_allow.dta", "hotfix", "outside the paths"},
        {"requires a newer base", "newer_base.dta", "hotfix", "game_data"},
        {"breaks a shape", "bad_shape.dta", "events", "warning refuses"},
        {"names a policy the game lacks", "double_xp.dta", "anything", "no policy named"},
    };
    for (const Case& c : cases) {
        const fs::path dir = scratch("refuse");
        FakePublisher pub(dir / "cdn");
        Host host(dir / "cdn", dir / "user", std::span(&pub.key.pub, 1), mode);
        const std::string before = root_text(host.cfg);
        const uint64_t epoch = host.cfg.epoch();
        const std::string text = overlay_text(c.file);
        pub.publish({{std::string(c.file).substr(0, std::string(c.file).size() - 4), "1", text, c.policy}});
        host.client.refresh();
        CHECK(host.client.apply());  // nothing staged, so nothing to fail
        CHECK(host.client.live().empty());
        CHECK(host.cfg.epoch() == epoch);
        CHECK(root_text(host.cfg) == before);
        CHECK(host.cfg.layers().size() == 2);  // default, scratch
        if (std::getenv("LIVEOPS_VERBOSE")) for (const std::string& l : host.log) std::printf("[%s] %s\n", c.what, l.c_str());
        if (!host.logged(c.expect)) {
            std::printf("%s: expected '%s' in the log:\n", c.what, c.expect);
            for (const std::string& l : host.log) std::printf("  %s\n", l.c_str());
            ++g_failures;
        }
    }
}

// What fails verification never reaches Baker.
void verification() {
    const fs::path dir = scratch("verify");
    FakePublisher pub(dir / "cdn");
    FakePublisher stranger(dir / "other");
    auto fresh = [&](const char* name) {
        return std::make_unique<Host>(dir / "cdn", dir / ("user-" + std::string(name)),
                                      std::span(&pub.key.pub, 1));
    };
    auto unchanged = [](Host& h, const char* expect) {
        const uint64_t epoch = h.cfg.epoch();
        h.client.refresh();
        CHECK(!h.client.pending());
        CHECK(h.client.apply());
        CHECK(h.cfg.epoch() == epoch && h.client.live().empty());
        if (!h.logged(expect)) {
            std::printf("expected '%s' in the log:\n", expect);
            for (const std::string& l : h.log) std::printf("  %s\n", l.c_str());
            ++g_failures;
        }
    };
    const fs::path manifest = dir / "cdn" / bl::static_backend::manifest_key("prod");

    // A signature that does not verify.
    std::string text = pub.publish({kBalance});
    std::string forged = text;
    char& digit = forged[forged.rfind("signature ") + 10];
    digit = digit == '0' ? '1' : '0';
    bl::static_backend::write_file(manifest, forged);
    unchanged(*fresh("sig"), "signature does not verify");

    // A manifest that verifies, edited after signing.
    std::string edited = text;
    edited.replace(edited.find("sequence 1"), 10, "sequence 9");
    bl::static_backend::write_file(manifest, edited);
    unchanged(*fresh("edited"), "signature does not verify");

    // Signed by a key the game does not trust.
    stranger.cdn = dir / "cdn";
    stranger.publish({kBalance});
    unchanged(*fresh("stranger"), "unknown key");

    // An overlay whose bytes are not the ones signed for.
    pub.publish({kBalance});
    bl::Manifest m;
    std::string error;
    m = *bl::parse_manifest(*bl::static_backend::read_file(manifest), error);
    const fs::path overlay = dir / "cdn" / bl::static_backend::overlay_key("prod", m.overlays[0]);
    std::string tampered = kBalance.text;
    tampered.replace(tampered.find("120"), 3, "999");
    bl::static_backend::write_file(overlay, tampered);
    unchanged(*fresh("bytes"), "SHA-256 does not match");

    // A manifest for another environment.
    pub.environment = "staging";
    std::string staging = pub.publish({kBalance});
    pub.environment = "prod";
    bl::static_backend::write_file(manifest, staging);
    unchanged(*fresh("env"), "for environment staging");

    // A replay: an older manifest after a newer one was taken.
    Host host(dir / "cdn", dir / "user-replay", std::span(&pub.key.pub, 1));
    const std::string old = pub.publish({kBalance});
    pub.publish({kBalance, kDoubleXp});
    CHECK(host.client.refresh() && host.client.apply());
    CHECK(host.client.live().size() == 2);
    bl::static_backend::write_file(manifest, old);
    CHECK(!host.client.refresh());
    CHECK(host.logged("a replay"));
    CHECK(!host.client.pending() && host.client.live().size() == 2);
}

// Rollback restores every value and source(), and stays rolled back across a restart.
void rollback(bl::ApplyMode mode) {
    const fs::path dir = scratch("rollback");
    FakePublisher pub(dir / "cdn");
    const char* paths[] = {"economy.gold_per_win", "economy.shop_discount", "events.xp_multiplier",
                           "events.drop_rate"};
    std::map<std::string, std::string> base;
    {
        Host host(dir / "cdn", dir / "user", std::span(&pub.key.pub, 1), mode);
        const std::string before = root_text(host.cfg);
        auto snapshot = [&] {
            std::map<std::string, std::string> out;
            for (const char* p : paths) {
                auto s = host.cfg.source(p);
                out[p] = baker::to_string(*host.cfg.root()->try_get(p)) + " @ " +
                         std::string(s->layer.str()) + " " + std::string(s->file.str()) + ":" +
                         std::to_string(s->line) + " " + std::string(s->origin.str());
            }
            return out;
        };
        base = snapshot();
        pub.publish({kBalance, kDoubleXp});
        CHECK(host.client.refresh() && host.client.apply());
        CHECK(host.client.live().size() == 2);
        CHECK(snapshot() != base);
        CHECK(host.cfg.get<std::string_view>("events.banner").has_value());

        CHECK(host.client.rollback("double_xp"));
        CHECK(host.get<double>("events.xp_multiplier") == 1);
        CHECK(!host.cfg.get<std::string_view>("events.banner").has_value());
        CHECK(host.get<int64_t>("economy.gold_per_win") == 120);
        CHECK(host.client.rollback("balance_2026_10"));
        CHECK(snapshot() == base);
        CHECK(root_text(host.cfg) == before);
        CHECK(host.cfg.layers().size() == 2);
        CHECK(!host.client.rollback("balance_2026_10"));
    }
    {   // After a restart, offline: the last good set without what was rolled back.
        Host host(dir / "nowhere", dir / "user", std::span(&pub.key.pub, 1), mode);
        CHECK(host.client.restore());
        CHECK(host.client.apply());
        CHECK(host.client.live().empty());
        CHECK(host.logged("rolled back here"));
    }
}

// The last good overlays come back at start without the network; one that no longer passes
// means starting on the base.
void last_good(bl::ApplyMode mode) {
    const fs::path dir = scratch("lastgood");
    FakePublisher pub(dir / "cdn");
    {
        Host host(dir / "cdn", dir / "user", std::span(&pub.key.pub, 1), mode);
        pub.publish({kBalance, kDoubleXp});
        CHECK(host.client.refresh() && host.client.apply());
    }
    {   // Offline start: both come back.
        Host host(dir / "nowhere", dir / "user", std::span(&pub.key.pub, 1), mode);
        CHECK(host.client.restore());
        CHECK(host.cfg.epoch() > 0 && !host.client.live().size());
        CHECK(host.client.apply());
        CHECK(host.client.live().size() == 2);
        CHECK(host.get<int64_t>("economy.gold_per_win") == 120);
        CHECK(!host.client.refresh());  // unreachable: what is live stays
        CHECK(host.client.live().size() == 2);
    }
    {   // A build whose policy no longer allows the balance overlay starts without it.
        Host host(dir / "nowhere", dir / "user", std::span(&pub.key.pub, 1), mode,
                  "tests/game/liveops_strict.dta");
        CHECK(host.client.restore() && host.client.apply());
        const auto live = host.client.live();
        CHECK(live.size() == 1 && live[0].layer == "double_xp");
        CHECK(host.get<int64_t>("economy.gold_per_win") == 100);
        CHECK(host.logged("refused balance_2026_10"));
    }
    {   // A stored overlay edited on disk is refused; the game starts on the base for it.
        bl::static_backend::write_file(dir / "user" / "overlay.double_xp", "(events (xp_multiplier 5))\n");
        Host host(dir / "nowhere", dir / "user", std::span(&pub.key.pub, 1), mode);
        CHECK(host.client.restore() && host.client.apply());
        CHECK(host.get<double>("events.xp_multiplier") == 1);
        CHECK(host.logged("double_xp: "));
    }
}

}  // namespace

int main() {
    for (bl::ApplyMode mode : {bl::ApplyMode::Path, bl::ApplyMode::Tree}) {
        applies_at_boundary(mode);
        refusals(mode);
        rollback(mode);
        last_good(mode);
    }
    verification();
    std::printf("%s: %d failure(s) (%s build)\n", g_failures ? "FAIL" : "ok", g_failures,
                baker::kShipping ? "shipping" : "development");
    return g_failures ? 1 : 0;
}
