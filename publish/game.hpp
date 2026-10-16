#pragma once

// A game's live-ops description: how its Config is built, and the policies its overlays are
// checked under. It ships with the game's data, so the publisher and the game check an
// overlay the same way.
//
//   (liveops
//      (pack_levels game live)                    ; hardest first
//      (include_dirs data)                        ; relative to the base checkout
//      (layers (default data/default.dta))        ; bottom to top
//      (shapes 1)                                 ; check every merge with baker::shape
//      (policies
//         (hotfix (data_only 1) (scalars_only 1) (existing_only 1) (warnings_refuse 1)
//                 (allow economy events))))

#include <baker_liveops/client.hpp>

#include <baker/config.hpp>
#include <baker/path.hpp>

#include <deque>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace baker_liveops {

struct GamePolicy {
    std::string name;
    std::vector<baker::Path> allow;
    baker::LayerPolicy policy;  // its allow points into `allow`
};

struct Game {
    Game() = default;
    Game(Game&&) = default;
    Game& operator=(Game&&) = default;
    Game(const Game&) = delete;  // the policies' spans point into it

    std::vector<std::string> pack_levels;
    std::vector<std::string> include_dirs;
    std::vector<std::pair<std::string, std::string>> layers;  // name, file
    bool shapes = false;
    std::deque<GamePolicy> policies;  // a deque: `allow` spans stay put

    [[nodiscard]] std::vector<NamedPolicy> named() const;
    [[nodiscard]] const GamePolicy* policy(std::string_view name) const;
};

[[nodiscard]] std::optional<Game> load_game(const std::filesystem::path& file, std::string& error);

// Sets up `cfg` as the game does and pushes its layers from `base`. A client's attach()
// comes before this.
[[nodiscard]] bool load_base(baker::Config& cfg, const Game& game, const std::filesystem::path& base,
                             std::string& error);

}  // namespace baker_liveops
