#pragma once

// Retiring an overlay that stuck: its effect written into the base data as a reviewable
// patch, for a person to apply, review, and merge. Nothing is written to the checkout, and
// nothing is pushed anywhere.

#include "../publish/game.hpp"

#include <filesystem>
#include <string>

namespace baker_liveops {

// Through a Baker Config built as the game builds it: the overlay is pushed as a layer over
// `base`, and Config::promote writes its effect into the layer `into`, with the Config's
// writer capturing each file. The patch names files relative to `base` (git apply -p1).
// Returns 0, 1 if Baker refused part of it (`message` lists why; no patch), or 2.
int retire_with_config(const Game& game, const std::filesystem::path& base,
                       const std::filesystem::path& overlay, const std::string& into,
                       std::string& patch, std::string& message);

// Through a stack's own `promote --baker-patch` (baker-dev's stacks): the overlay as its agent
// layer, in a copy of the stack's directories.
int retire_with_stack(const std::string& binary, const std::filesystem::path& data_dir,
                      const std::string& overlay_text, const std::string& into, std::string& patch,
                      std::string& message);

}  // namespace baker_liveops
