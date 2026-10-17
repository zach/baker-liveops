#pragma once

// The manifest: what an environment should run, signed as a whole. One field a line, a
// name and a value; the notes after ';' here are not part of it.
//
//   baker-liveops manifest 1
//   environment prod
//   sequence 7                       ; rises with every publish: an older one is a replay
//   overlay balance_2026_10          ; one block per overlay, bottom to top
//   version 3
//   requires game_data 1.4           ; as the overlay's pack declares it; any number
//   sha256 9f86d0...                 ; of the overlay's bytes
//   size 10240
//   policy hotfix                    ; the game's name for the LayerPolicy it is checked under
//   key 1a2b3c4d5e6f7081             ; which publisher key signed it
//   signature 4f0c...                ; Ed25519 over every byte above this line
//
// Parsed here, by hand, so that Baker never reads a byte before it is verified.

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace baker_liveops {

struct Entry {
    std::string name{};     // the overlay's pack name, and its layer's name
    std::string version{};  // the pack's (version "3")
    std::vector<std::string> requires_{};  // "game_data 1.4", as declared
    std::string sha256{};   // lowercase hex
    uint64_t size = 0;
    std::string policy{};
};

struct Manifest {
    std::string environment;
    uint64_t sequence = 0;
    std::vector<Entry> overlays;
    std::string key_id;     // 16 hex digits: the first 8 bytes of the public key
    std::string signature;  // 128 hex digits
};

struct PublicKey {
    std::array<unsigned char, 32> bytes{};
    [[nodiscard]] std::string id() const;  // 16 hex digits
};

// Parses text in the form above. On failure, empty and `error` says why.
[[nodiscard]] std::optional<Manifest> parse_manifest(std::string_view text, std::string& error);
// Writes `m` without its signature: the bytes a signature covers.
[[nodiscard]] std::string signed_part(const Manifest& m);
// The bytes a signature covers in received text: everything before the signature line.
[[nodiscard]] std::string_view signed_part(std::string_view text);

// Verifies a received manifest's signature with the key it names, which must be one of
// `keys`. On failure, empty and `error` says why.
[[nodiscard]] std::optional<Manifest> verify_manifest(std::string_view text,
                                                      std::span<const PublicKey> keys,
                                                      std::string& error);
// The overlay's bytes against its entry: size and SHA-256.
[[nodiscard]] bool verify_overlay(const Entry& entry, std::string_view text, std::string& error);

[[nodiscard]] std::string sha256_hex(std::string_view bytes);
[[nodiscard]] std::string to_hex(std::span<const unsigned char> bytes);
[[nodiscard]] bool from_hex(std::string_view hex, std::span<unsigned char> out);
[[nodiscard]] std::optional<PublicKey> parse_public_key(std::string_view hex);

}  // namespace baker_liveops
