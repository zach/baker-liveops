#pragma once

#include <string>
#include <string_view>

namespace baker_liveops {

// A unified diff of `before` to `after`, naming the file `name` under a/ and b/ (git apply
// -p1), with three lines of context; empty when they are the same. In process, so retire
// needs no diff program.
[[nodiscard]] std::string unified_diff(std::string_view before, std::string_view after,
                                       std::string_view name);

}  // namespace baker_liveops
