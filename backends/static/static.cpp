#include "static.hpp"

#include <fstream>
#include <sstream>

namespace baker_liveops::static_backend {

std::string manifest_key(std::string_view environment) {
    return std::string(environment) + "/manifest";
}

std::string overlay_key(std::string_view environment, const Entry& entry) {
    return std::string(environment) + "/overlays/" + entry.name + "-" + entry.sha256.substr(0, 16) +
           ".dta";
}

std::optional<std::string> read_file(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return std::nullopt;
    std::ostringstream text;
    text << in.rdbuf();
    return std::move(text).str();
}

bool write_file(const std::filesystem::path& path, std::string_view bytes) {
    std::error_code ec;
    if (path.has_parent_path()) std::filesystem::create_directories(path.parent_path(), ec);
    std::filesystem::path tmp = path;
    tmp += ".tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()))) return false;
    }
    std::filesystem::rename(tmp, path, ec);
    return !ec;
}

std::optional<std::string> DirTransport::fetch_manifest(std::string_view environment) {
    return read_file(root_ / manifest_key(environment));
}

std::optional<std::string> DirTransport::fetch_overlay(std::string_view environment,
                                                       const Entry& entry) {
    return read_file(root_ / overlay_key(environment, entry));
}

std::optional<std::string> DirStore::load(std::string_view key) { return read_file(root_ / key); }

bool DirStore::save(std::string_view key, std::string_view bytes) {
    return write_file(root_ / key, bytes);
}

}  // namespace baker_liveops::static_backend
