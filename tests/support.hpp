#pragma once

// What the tests share: a check macro, a scratch directory, and a fake publisher that signs
// whatever it is given, so the client's own refusals can be tested. Keys are made fresh in
// every run and never written to the repository.

#include "../publish/game.hpp"
#include "../publish/publish.hpp"

#include <baker_liveops/client.hpp>
#include <static/static.hpp>

#include <baker/std_interop.hpp>

#include <cstdio>
#include <filesystem>
#include <random>
#include <string>
#include <vector>

namespace fs = std::filesystem;
namespace bl = baker_liveops;

inline int g_failures = 0;
#define CHECK(cond)                                                                 \
    do {                                                                            \
        if (!(cond)) {                                                              \
            std::printf("%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond);    \
            ++g_failures;                                                           \
        }                                                                           \
    } while (0)

inline fs::path source_dir() { return fs::path(LIVEOPS_SOURCE_DIR); }

inline fs::path scratch(const std::string& name) {
    std::random_device rd;
    fs::path p = fs::temp_directory_path() / ("liveops-" + name + "-" + std::to_string(rd()));
    fs::remove_all(p);
    fs::create_directories(p);
    return p;
}

inline std::string overlay_text(const std::string& file) {
    return *bl::static_backend::read_file(source_dir() / "tests/overlays" / file);
}

// Signs and uploads overlays to a static backend directory, as a publisher would, without
// the publisher's gate.
struct FakePublisher {
    fs::path cdn;
    std::string environment = "prod";
    bl::SecretKey key;
    uint64_t sequence = 0;

    explicit FakePublisher(fs::path dir) : cdn(std::move(dir)) {
        std::string seed;
        key = bl::generate_key(seed);
    }

    struct Item {
        std::string name, version, text, policy;
    };

    // Returns the manifest's text.
    std::string publish(const std::vector<Item>& items) {
        bl::Manifest m;
        m.environment = environment;
        m.sequence = ++sequence;
        for (const Item& i : items) {
            bl::Entry e;
            e.name = i.name;
            e.version = i.version;
            e.sha256 = bl::sha256_hex(i.text);
            e.size = i.text.size();
            e.policy = i.policy;
            bl::static_backend::write_file(cdn / bl::static_backend::overlay_key(environment, e), i.text);
            m.overlays.push_back(e);
        }
        std::string text = bl::sign(m, key);
        bl::static_backend::write_file(cdn / bl::static_backend::manifest_key(environment), text);
        return text;
    }
};

inline std::string root_text(const baker::Config& cfg) { return baker::to_string(*cfg.root()); }
