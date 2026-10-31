#pragma once

// The publisher's parts: keys and signing, backends, and the gate an overlay passes before
// it is uploaded. The command line (main.cpp) puts them together.

#include "game.hpp"

#include <baker_liveops/manifest.hpp>

#include <baker/config.hpp>

#include <array>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace baker_liveops {

// --- Keys. A secret key is kept as its 32-byte seed, in hex, outside the repository.

struct SecretKey {
    std::array<unsigned char, 64> bytes{};
    PublicKey pub;
};

[[nodiscard]] SecretKey generate_key(std::string& seed_hex);
[[nodiscard]] std::optional<SecretKey> key_from_seed(std::string_view seed_hex);
// The manifest as text, signed: `m.key_id` and `m.signature` are set from the key.
[[nodiscard]] std::string sign(Manifest m, const SecretKey& key);

// --- Backends: where the publisher puts overlays and manifests, by key (static.hpp's
// layout). "static:DIR" writes files under DIR; "cmd:PROGRAM ARGS..." runs
// `PROGRAM ARGS get KEY` (the bytes on stdout; exit 3: no such key) and
// `PROGRAM ARGS put KEY` (the bytes on stdin), which is how the Nakama backend plugs in.

class Backend {
public:
    virtual ~Backend() = default;
    // Nothing if the key is absent; `failed` is set if the backend could not answer.
    virtual std::optional<std::string> get(std::string_view key, bool& failed) = 0;
    virtual bool put(std::string_view key, std::string_view bytes) = 0;
};

[[nodiscard]] std::unique_ptr<Backend> open_backend(std::string_view spec, std::string& error);

// A Transport over a backend: what a host uses to read a "cmd:" backend (tests, tools).
class BackendTransport final : public Transport {
public:
    explicit BackendTransport(Backend& backend) : backend_(backend) {}
    std::optional<std::string> fetch_manifest(std::string_view environment) override;
    std::optional<std::string> fetch_overlay(std::string_view environment, const Entry& entry) override;

private:
    Backend& backend_;
};

// --- The gate.

struct Candidate {
    Entry entry;  // name, version and requires from its pack; sha256, size, policy
    std::string text;
    std::string origin;  // its #origin, if it declares one
    baker::LayerReport report;
};

// Checks overlay text against `base` (built as the game builds it) under the game's named
// policy, as the client will. Every reason to refuse it goes into `problems`; a candidate
// comes back only if there are none.
[[nodiscard]] std::optional<Candidate> check_overlay(const baker::Config& base, const Game& game,
                                                     std::string text, std::string_view policy,
                                                     std::vector<std::string>& problems,
                                                     std::string_view prefix = "liveops/");

// The manifest after publishing `entry`: replaces an overlay of the same name, else adds it
// on top, and raises the sequence.
[[nodiscard]] Manifest with_overlay(std::optional<Manifest> current, std::string_view environment,
                                    const Entry& entry);
// The manifest without the named overlay; false if it has none.
[[nodiscard]] bool without_overlay(Manifest& m, std::string_view name);

[[nodiscard]] std::string describe(const baker::LayerReport& report);

}  // namespace baker_liveops
