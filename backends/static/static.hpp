#pragma once

// The static backend: files under a prefix, served by any HTTPS host, CDN, or bucket.
//
//   <prefix>/<environment>/manifest
//   <prefix>/<environment>/overlays/<name>-<first 16 digits of its sha256>.dta
//
// Overlays are named by content, so a client that read the previous manifest still finds
// its overlay while a publish is in flight: upload overlays first, the manifest last. A game
// fetches these keys with its own HTTP client; DirTransport reads them from a directory,
// which is what the tests and a local mirror use.

#include <baker_liveops/client.hpp>

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace baker_liveops::static_backend {

[[nodiscard]] std::string manifest_key(std::string_view environment);
[[nodiscard]] std::string overlay_key(std::string_view environment, const Entry& entry);

[[nodiscard]] std::optional<std::string> read_file(const std::filesystem::path& path);
bool write_file(const std::filesystem::path& path, std::string_view bytes);  // via a temporary

// A directory standing in for the HTTPS prefix.
class DirTransport final : public Transport {
public:
    explicit DirTransport(std::filesystem::path root) : root_(std::move(root)) {}
    std::optional<std::string> fetch_manifest(std::string_view environment) override;
    std::optional<std::string> fetch_overlay(std::string_view environment, const Entry& entry) override;

private:
    std::filesystem::path root_;
};

// The client's last good overlays, as files in a directory of the game's user storage.
class DirStore final : public Store {
public:
    explicit DirStore(std::filesystem::path root) : root_(std::move(root)) {}
    std::optional<std::string> load(std::string_view key) override;
    bool save(std::string_view key, std::string_view bytes) override;

private:
    std::filesystem::path root_;
};

}  // namespace baker_liveops::static_backend
