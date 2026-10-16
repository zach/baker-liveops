#pragma once

// A stack's scenarios as a gate (baker-dev's stacks: `sim --out DIR` and `compare BEFORE
// AFTER`): the stack runs without the overlay and with it as its agent layer, in a copy of
// its directories, and an objective that passed before and fails with it refuses the
// publish.

#include <filesystem>
#include <string>

namespace baker_liveops {

// A new empty directory under the system's temporary directory.
[[nodiscard]] std::filesystem::path temp_dir(const std::string& name);
// Runs a shell command; its output into `out` if given, else discarded. The exit code.
int run(const std::string& command, std::string* out = nullptr);
// Copies the stack around `data_dir` (it, and sim/, metrics/, packs/ beside it) into `into`,
// without its local and agent layers. Returns the copy's data directory, or empty.
[[nodiscard]] std::filesystem::path copy_stack(const std::filesystem::path& data_dir,
                                               const std::filesystem::path& into);

struct ScenarioResult {
    int code = 2;         // 0: nothing broke; 1: an objective broke; 2: could not run
    std::string report;   // compare's JSON, or why it could not run
};

[[nodiscard]] ScenarioResult run_scenarios(const std::string& binary,
                                           const std::filesystem::path& data_dir,
                                           const std::string& overlay_text);

}  // namespace baker_liveops
