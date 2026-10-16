#pragma once

// The client: takes published overlays into a running game, safely.
//
//   baker_liveops::Client live(cfg, {.environment = "prod", .keys = keys,
//                                    .policies = policies, .transport = &cdn, .store = &disk});
//   live.attach(cfg.session());            // before the base loads (ApplyMode::Path)
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
// then its bytes (size, SHA-256), and only then does Baker read it, as text, checked with
// Config::check_layer_text under the policy the manifest names. apply() pushes it with the
// same policy. Nothing changes the Config but apply() and rollback(), and a refused overlay
// changes nothing. Every call runs on the thread that owns the Config.

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

enum class ApplyMode {
    // A file layer read through the Config's session, by a reader attach() installs: the
    // overlay's pack (requires, version) and origin are read with it, so source() names
    // its origin. Needs attach() before the base loads, and no ConfigOptions::cache_dir.
    Path,
    // A tree read from the text (text only) and pushed: source() names the layer and its
    // file, without the origin. For a Config that loads through caches.
    Tree,
};

// Whether apply() checks an overlay again as it pushes it.
enum class Recheck {
    // push_layer under the policy, always: the check runs at staging and again at apply.
    Always,
    // Under the policy only if the stack changed since the overlay was staged (another
    // epoch); otherwise the staging check stands, and apply() costs the push alone. Baker
    // has no way yet to push a layer it has already checked (requirements/0002).
    IfStackChanged,
};

struct ClientOptions {
    std::string environment;
    std::span<const PublicKey> keys;
    std::span<const NamedPolicy> policies;
    Transport* transport = nullptr;
    Store* store = nullptr;
    ApplyMode mode = ApplyMode::Path;
    Recheck recheck = Recheck::Always;
    std::string prefix = "liveops/";      // overlays' file names: prefix + name + ".dta"
    uint64_t max_overlay_bytes = 4 << 20;
    std::function<void(std::string_view)> log;  // one line per event; default: none
};

// What is live, as Baker reports it.
struct LiveOverlay {
    std::string layer;    // the overlay's name
    std::string version;  // its pack's version
    std::string file;     // the name it was read under
    std::string origin, tool;  // its (origin ...) declaration, in ApplyMode::Path
    std::string sha256;
};

class Client {
public:
    Client(baker::Config& cfg, ClientOptions options);
    Client(const Client&) = delete;
    Client& operator=(const Client&) = delete;

    // Installs the reader that serves verified overlays by name and everything else as
    // `session`'s reader did. Call on cfg.session() before the first push_layer.
    void attach(baker::LoadSession& session);

    // Stages the overlays applied last time, from the store, verified and checked again.
    // One that no longer passes is dropped: the game starts on the base without it.
    bool restore();
    // Fetches the manifest; if it is new, verifies and stages it. False if nothing was
    // staged (no change, or refused: the log says why), and what was staged stays.
    bool refresh();
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
    struct Overlay {
        Entry entry;
        std::string text;
        baker::LayerReport report;
        uint64_t epoch = 0;        // the Config's when it was checked
        std::string origin, tool;  // filled when it goes live
    };

    const baker::LayerPolicy* policy(std::string_view name) const;
    std::string path(std::string_view name) const;
    bool read(std::string_view path, baker::TextSink out) const;
    void note(std::string_view line) const;
    std::optional<std::vector<Overlay>> stage(std::string_view manifest_text, bool from_store);
    bool push(Overlay& o);
    void save() const;
    bool rolled_back(const Entry& e) const;

    baker::Config& cfg_;
    ClientOptions opt_;
    baker::LoadSession::Reader below_;
    uint64_t sequence_ = 0;  // of the newest manifest accepted
    std::string staged_manifest_, live_manifest_;
    std::vector<Overlay> staged_, live_;
    std::map<std::string, std::string, std::less<>> rolled_back_;  // name -> sha256
};

}  // namespace baker_liveops
