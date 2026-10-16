// A stand-in for a game: builds its Config from a live-ops description, takes the overlays
// an environment publishes through the client, and prints what is live. The end-to-end tests
// drive it as a game would run: start (restore, apply), poll (refresh), boundary (apply).
//
//   liveops_host --game G --base DIR --backend B --env E --trust PUBKEY [--store DIR]
//                [--mode path|tree] [--offline] [--rollback NAME] [--get PATH]...
#include "../publish/publish.hpp"

#include <baker_liveops/client.hpp>
#include <static/static.hpp>

#include <baker/std_interop.hpp>

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace bl = baker_liveops;

int main(int argc, char** argv) {
    std::string game_file, base, backend_spec, env, store_dir, rollback;
    std::vector<std::string> trust, gets;
    bool offline = false;
    bl::ApplyMode mode = bl::ApplyMode::Path;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        const char* v = i + 1 < argc ? argv[i + 1] : "";
        if (a == "--offline") { offline = true; continue; }
        ++i;
        if (a == "--game") game_file = v;
        else if (a == "--base") base = v;
        else if (a == "--backend") backend_spec = v;
        else if (a == "--env") env = v;
        else if (a == "--trust") trust.emplace_back(v);
        else if (a == "--store") store_dir = v;
        else if (a == "--rollback") rollback = v;
        else if (a == "--get") gets.emplace_back(v);
        else if (a == "--mode") mode = std::strcmp(v, "tree") ? bl::ApplyMode::Path : bl::ApplyMode::Tree;
        else return std::fprintf(stderr, "unknown %s\n", a.c_str()), 2;
    }
    std::string error;
    auto game = bl::load_game(game_file, error);
    auto backend = bl::open_backend(backend_spec, error);
    if (!game || !backend) return std::fprintf(stderr, "%s\n", error.c_str()), 2;
    std::vector<bl::PublicKey> keys;
    for (const std::string& t : trust)
        if (auto k = bl::parse_public_key(t)) keys.push_back(*k);
    const auto policies = game->named();
    bl::BackendTransport transport(*backend);
    bl::static_backend::DirStore store(store_dir);
    baker::Config cfg({.live_layers = true, .tweaks = nullptr});
    bl::Client client(cfg, {.environment = env, .keys = keys, .policies = policies, .transport = &transport,
                            .store = store_dir.empty() ? nullptr : &store, .mode = mode,
                            .log = [](std::string_view l) { std::printf("log: %.*s\n", int(l.size()), l.data()); }});
    if (mode == bl::ApplyMode::Path) client.attach(cfg.session());
    if (!bl::load_base(cfg, *game, base, error)) return std::fprintf(stderr, "%s\n", error.c_str()), 2;
    cfg.freeze();

    bool ok = client.restore() && client.apply();  // start: the last good set
    if (!offline && client.refresh()) ok = client.apply() && ok;  // then the boundary
    if (!rollback.empty()) ok = client.rollback(rollback) && ok;

    for (const bl::LiveOverlay& o : client.live())
        std::printf("live %s %s origin=%s\n", o.layer.c_str(), o.version.c_str(), o.origin.c_str());
    for (const std::string& p : gets) {
        const baker::Array* a = cfg.root()->try_get(baker::Path(p));
        auto s = cfg.source(baker::Path(p));
        std::printf("value %s = %s (layer %s, %s:%d, origin %s)\n", p.c_str(),
                    a ? baker::to_string(*a).c_str() : "(none)", s ? s->layer.c_str() : "?",
                    s ? s->file.c_str() : "?", s ? int(s->line) : 0, s ? s->origin.c_str() : "?");
    }
    return ok ? 0 : 1;
}
