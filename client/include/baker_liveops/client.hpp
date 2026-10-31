#pragma once

// The client: takes published overlays into a running game, safely.
//
//   baker_liveops::Client live(cfg, {.environment = "prod", .keys = keys,
//                                    .policies = policies, .transport = &cdn, .store = &disk});
//   cfg.push_layer("default", "data/default.dta");
//   cfg.freeze();                          // with ConfigOptions::live_layers
//   live.restore();                        // stage the last good overlays, offline
//   live.apply();                          // the first session starts with them
//   ...
//   live.refresh();                        // when the host polls: fetch, verify, stage
//   if (live.pending()) /* at the next session start or lobby */ live.apply();
//   live.rollback("balance_2026_10");
//
// The order is fixed: an overlay's manifest is verified (Ed25519, a key the game trusts),
// then its bytes (size, SHA-256), and only then does Baker read it, as received text (never
// a cache), checked under the policy the manifest names. apply() pushes it with
// Config::push_layer_text under the same policy and the staged report, so Baker checks again
// only if the stack moved since. Nothing changes the Config but apply() and rollback(), and a
// refused overlay changes nothing.
//
// Threads. Everything runs on the thread that owns the Config, except Refresh::run(), which
// fetches, verifies, and checks against a LayerChecker on any thread:
//
//   auto job = live.begin_refresh();       // the Config's thread
//   worker.post([&] { job.run(); });       // any thread: the transport is called here
//   live.finish_refresh(std::move(job));   // the Config's thread again
//
// Such a check runs only the validators added with {.any_thread = true}; the push runs the
// others first.

#include <baker_liveops/manifest.hpp>

#include <baker/config.hpp>

#include <functional>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace baker_liveops {

// Where overlays come from: the host implements it over HTTPS, a platform's storage, or a
// game server's RPC. Each call returns the whole body, or nothing if it cannot be had.
class Transport {
public:
    virtual ~Transport() = default;
    virtual std::optional<std::string> fetch_manifest(std::string_view environment) = 0;
    virtual std::optional<std::string> fetch_overlay(std::string_view environment,
                                                     const Entry& entry) = 0;
};

// Where the last good overlays are kept between runs (user storage). Optional.
class Store {
public:
    virtual ~Store() = default;
    virtual std::optional<std::string> load(std::string_view key) = 0;
    virtual bool save(std::string_view key, std::string_view bytes) = 0;
};

struct NamedPolicy {
    std::string_view name;
    baker::LayerPolicy policy;
};

struct ClientOptions {
    std::string environment;
    std::span<const PublicKey> keys;
    std::span<const NamedPolicy> policies;
    Transport* transport = nullptr;
    Store* store = nullptr;
    // Each overlay's layer, which source() names: prefix + name + "@" + version + ".dta".
    std::string prefix = "liveops/";
    uint64_t max_overlay_bytes = 4 << 20;
    // The names one overlay may add to the program (symbols live until it ends). Baker's
    // received-text reads do not apply a cap of their own yet (requirements/0005), so the
    // client reads each verified overlay once under this one before the check.
    uint64_t max_new_symbols = 16384;
    std::function<void(std::string_view)> log;  // one line per event; default: none
};

// What is live, as Baker reports it.
struct LiveOverlay {
    std::string name;     // the overlay's
    std::string version;  // its pack's
    std::string layer;    // its layer in the Config: liveops/balance_2026_10@3.dta
    std::string origin, tool;  // its #origin declaration
    std::string sha256;
};

// An overlay verified and checked, waiting for apply(), or live.
struct Staged {
    Entry entry;
    std::string text;
    baker::LayerReport report;  // its check, which the push reuses if it still holds
    std::string origin, tool;   // filled when it goes live
};

class Client {
public:
    Client(baker::Config& cfg, ClientOptions options);
    Client(const Client&) = delete;
    Client& operator=(const Client&) = delete;

    // A refresh in three steps, so that fetching, verifying and checking can leave the
    // Config's thread. Movable; it holds copies of what it needs, and a LayerChecker.
    class Refresh {
    public:
        void run();  // any thread

    private:
        friend class Client;
        const ClientOptions* opt = nullptr;
        baker::LayerChecker checker;
        baker::LoadSession names;  // the stack's session, text only, under max_new_symbols
        bool from_store = false;
        uint64_t floor = 0;  // the sequence to beat
        std::vector<std::string> known_manifests;
        std::map<std::string, std::string, std::less<>> known_texts;  // by sha256
        std::map<std::string, std::string, std::less<>> rolled_back;  // name -> sha256
        std::string manifest;
        uint64_t sequence = 0;
        bool staged = false;  // a new manifest was verified
        std::vector<Staged> overlays;
        std::vector<std::string> log;
    };

    // Stages the overlays applied last time, from the store, verified and checked again.
    // One that no longer passes is dropped: the game starts on the base without it.
    bool restore();
    // Fetches the manifest; if it is new, verifies and stages it. False if nothing was
    // staged (no change, or refused: the log says why), and what was staged stays.
    bool refresh();
    [[nodiscard]] Refresh begin_refresh() const;
    bool finish_refresh(Refresh&& job);

    // True when apply() would change the stack.
    [[nodiscard]] bool pending() const;
    // At the host's boundary: removes the live overlays the staged set drops or replaces,
    // and pushes the new ones under their policies. An overlay Baker refuses is skipped,
    // changing nothing. Saves the result as the last good set. False if any was refused.
    bool apply();
    // Removes a live overlay; it stays off, here and after a restart, until the manifest
    // carries another version of it.
    bool rollback(std::string_view name);

    [[nodiscard]] std::vector<LiveOverlay> live() const;
    // The report of the last check of each staged overlay, by name.
    [[nodiscard]] const baker::LayerReport* report(std::string_view name) const;

private:
    const baker::LayerPolicy* policy(std::string_view name) const;
    std::string layer(const Entry& e) const;
    void note(std::string_view line) const;
    Refresh prepare(bool from_store) const;
    bool push(Staged& o);
    void save() const;

    baker::Config& cfg_;
    ClientOptions opt_;
    uint64_t sequence_ = 0;  // of the newest manifest accepted
    std::string staged_manifest_, live_manifest_;
    std::vector<Staged> staged_, live_;
    std::map<std::string, std::string, std::less<>> rolled_back_;  // name -> sha256
};

}  // namespace baker_liveops
