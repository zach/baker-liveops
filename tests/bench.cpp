// How long the main thread spends taking an overlay: verifying it, staging it
// (check_layer_text) and applying it (push_layer under the policy), for a 10 KB overlay over
// a 2,000-item table, as Baker measured its check (D161). Medians of many runs.
//
//   liveops_bench [runs]
#include "support.hpp"

#include <baker/shape.hpp>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <map>

namespace {

using Clock = std::chrono::steady_clock;

double ms(Clock::duration d) { return std::chrono::duration<double, std::milli>(d).count(); }

struct Stats {
    std::vector<double> v;
    void add(double x) { v.push_back(x); }
    double median() {
        std::sort(v.begin(), v.end());
        return v[v.size() / 2];
    }
    double p90() {
        std::sort(v.begin(), v.end());
        return v[v.size() * 9 / 10];
    }
};

class MemoryTransport final : public bl::Transport {
public:
    std::string manifest;
    std::map<std::string, std::string> overlays;  // by sha256
    std::optional<std::string> fetch_manifest(std::string_view) override { return manifest; }
    std::optional<std::string> fetch_overlay(std::string_view, const bl::Entry& e) override {
        return overlays.at(e.sha256);
    }
};

std::string base_text(int items) {
    std::string s = "(pack game_data (level game) (version \"2.0\"))\n(shapes\n   (item (at items) (keyed)\n"
                    "      (price int (range 0 100000))\n      (weight float (range 0 1000))\n"
                    "      (stack int (range 1 999))\n      (rarity symbol)))\n(items\n";
    for (int i = 0; i < items; ++i) {
        char line[128];
        std::snprintf(line, sizeof line, "   (item_%04d (price %d) (weight %.1f) (stack %d) (rarity common))\n", i,
                      10 + i % 500, 0.5 + (i % 40) * 0.25, 1 + i % 99);
        s += line;
    }
    return s + ")\n";
}

// About `bytes` of changes to existing items' prices and weights.
std::string overlay_text(size_t bytes, int version) {
    std::string s = "(pack balance (level live) (version \"" + std::to_string(version) +
                    "\") (requires (game_data \"2.0\")))\n(origin \"ops/balance.dta\" (tool baker-liveops))\n(items\n";
    for (int i = 0; s.size() < bytes - 2; i += 7) {
        char line[96];
        std::snprintf(line, sizeof line, "   (item_%04d (price %d) (weight %.2f))\n", i % 2000, 11 + i % 400 + version,
                      0.75 + (i % 30) * 0.5);
        s += line;
    }
    return s + ")\n";
}

}  // namespace

int main(int argc, char** argv) {
    const int runs = argc > 1 ? std::atoi(argv[1]) : 200;
    const fs::path dir = scratch("bench");
    bl::static_backend::write_file(dir / "default.dta", base_text(2000));

    const baker::Path allow[] = {baker::Path("items")};
    const baker::LayerPolicy policy{.data_only = true, .scalars_only = true, .existing_only = true,
                                    .warnings_refuse = true, .allow = allow};
    const bl::NamedPolicy policies[] = {{"hotfix", policy}};
    std::string seed;
    const bl::SecretKey key = bl::generate_key(seed);

    for (bl::Recheck recheck : {bl::Recheck::Always, bl::Recheck::IfStackChanged})
    for (bool shapes : {false, true})
        for (bl::ApplyMode mode : {bl::ApplyMode::Path, bl::ApplyMode::Tree}) {
            baker::Config cfg({.live_layers = true, .tweaks = nullptr});
            MemoryTransport transport;
            bl::Client client(cfg, {.environment = "prod", .keys = std::span(&key.pub, 1),
                                    .policies = policies, .transport = &transport, .mode = mode,
                                    .recheck = recheck});
            if (mode == bl::ApplyMode::Path) client.attach(cfg.session());
            cfg.session().set_pack_levels({"game", "live"});
            if (shapes) cfg.add_validator(baker::shape::validator(cfg));
            if (!cfg.push_layer("default", (dir / "default.dta").string())) return 2;
            cfg.freeze();

            Stats verify, stage, apply, remove;
            uint64_t sequence = 0;
            size_t size = 0;
            for (int r = 0; r < runs; ++r) {
                const std::string text = overlay_text(10 * 1024, r % 2 + 1);
                size = text.size();
                bl::Manifest m{.environment = "prod", .sequence = ++sequence, .overlays = {}, .key_id = {}, .signature = {}};
                bl::Entry e;
                e.name = "balance";
                e.version = std::to_string(r % 2 + 1);
                e.sha256 = bl::sha256_hex(text);
                e.size = text.size();
                e.policy = "hotfix";
                m.overlays.push_back(e);
                transport.manifest = bl::sign(m, key);
                transport.overlays[e.sha256] = text;

                // Verification alone: the manifest's signature, the overlay's hash.
                std::string error;
                auto t0 = Clock::now();
                auto vm = bl::verify_manifest(transport.manifest, std::span(&key.pub, 1), error);
                const bool ok = vm && bl::verify_overlay(vm->overlays[0], text, error);
                verify.add(ms(Clock::now() - t0));
                if (!ok) return std::printf("verify: %s\n", error.c_str()), 2;

                // refresh() is verification plus the check; the check is what remains.
                t0 = Clock::now();
                if (!client.refresh()) return std::printf("refresh refused\n"), 2;
                stage.add(ms(Clock::now() - t0) - verify.v.back());
                t0 = Clock::now();
                if (!client.apply()) return std::printf("apply refused\n"), 2;
                apply.add(ms(Clock::now() - t0));

                // Back to the base for the next run, through a withdrawal.
                m.overlays.clear();
                m.sequence = ++sequence;
                transport.manifest = bl::sign(m, key);
                t0 = Clock::now();
                if (!client.refresh() || !client.apply()) return std::printf("withdraw failed\n"), 2;
                remove.add(ms(Clock::now() - t0));
            }
            std::printf("%-7s %-5s %-6s overlay %zu B, %d runs | verify %.3f ms | stage (check) %.3f ms (p90 %.3f) | "
                        "apply %.3f ms (p90 %.3f) | withdraw %.3f ms | %s build\n",
                        recheck == bl::Recheck::Always ? "recheck" : "once", mode == bl::ApplyMode::Path ? "path" : "tree", shapes ? "shapes" : "plain", size, runs,
                        verify.median(), stage.median(), stage.p90(), apply.median(), apply.p90(), remove.median(),
                        baker::kShipping ? "shipping" : "development");
        }
    return 0;
}
