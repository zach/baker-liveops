#include "diff.hpp"

#include <algorithm>
#include <cstdint>
#include <vector>

namespace baker_liveops {
namespace {

// Each line with its newline; the last may have none.
std::vector<std::string_view> lines(std::string_view text) {
    std::vector<std::string_view> out;
    while (!text.empty()) {
        const size_t eol = text.find('\n');
        const size_t n = eol == std::string_view::npos ? text.size() : eol + 1;
        out.push_back(text.substr(0, n));
        text.remove_prefix(n);
    }
    return out;
}

struct Op {
    char kind;      // ' ', '-' or '+'
    size_t a, b;    // the line's index in before and in after
};

}  // namespace

std::string unified_diff(std::string_view before, std::string_view after, std::string_view name) {
    if (before == after) return {};
    const auto a = lines(before), b = lines(after);
    // The common head and tail first, so the table covers only what changed.
    size_t head = 0;
    while (head < a.size() && head < b.size() && a[head] == b[head]) ++head;
    size_t tail = 0;
    while (tail < a.size() - head && tail < b.size() - head &&
           a[a.size() - 1 - tail] == b[b.size() - 1 - tail])
        ++tail;
    const size_t n = a.size() - head - tail, m = b.size() - head - tail;
    // The longest common subsequence of the middles.
    std::vector<uint32_t> lcs((n + 1) * (m + 1), 0);
    auto at = [&](size_t i, size_t j) -> uint32_t& { return lcs[i * (m + 1) + j]; };
    for (size_t i = n; i-- > 0;)
        for (size_t j = m; j-- > 0;)
            at(i, j) = a[head + i] == b[head + j] ? at(i + 1, j + 1) + 1 : std::max(at(i + 1, j), at(i, j + 1));

    std::vector<Op> ops;
    for (size_t i = 0; i < head; ++i) ops.push_back({' ', i, i});
    for (size_t i = 0, j = 0; i < n || j < m;) {
        if (i < n && j < m && a[head + i] == b[head + j])
            ops.push_back({' ', head + i++, head + j++});
        else if (i < n && (j == m || at(i + 1, j) >= at(i, j + 1)))
            ops.push_back({'-', head + i++, head + j});  // removals first, as diff -u
        else
            ops.push_back({'+', head + i, head + j++});
    }
    for (size_t k = 0; k < tail; ++k) ops.push_back({' ', a.size() - tail + k, b.size() - tail + k});

    std::string out = "--- a/" + std::string(name) + "\n+++ b/" + std::string(name) + "\n";
    constexpr size_t context = 3;
    std::vector<size_t> changes;
    for (size_t k = 0; k < ops.size(); ++k)
        if (ops[k].kind != ' ') changes.push_back(k);
    // Hunks: changes no more than twice the context apart, with the context around them.
    for (size_t c = 0; c < changes.size();) {
        size_t last = c;
        while (last + 1 < changes.size() && changes[last + 1] - changes[last] <= 2 * context + 1) ++last;
        const size_t start = changes[c] > context ? changes[c] - context : 0;
        const size_t end = std::min(ops.size(), changes[last] + context + 1);
        size_t na = 0, nb = 0;
        for (size_t x = start; x < end; ++x) {
            na += ops[x].kind != '+';
            nb += ops[x].kind != '-';
        }
        // A side with no lines names the line before the hunk, as diff -u does.
        out += "@@ -" + std::to_string(ops[start].a + (na ? 1 : 0)) + "," + std::to_string(na) + " +" +
               std::to_string(ops[start].b + (nb ? 1 : 0)) + "," + std::to_string(nb) + " @@\n";
        for (size_t x = start; x < end; ++x) {
            const std::string_view line = ops[x].kind == '+' ? b[ops[x].b] : a[ops[x].a];
            out += ops[x].kind;
            out += line;
            if (line.empty() || line.back() != '\n') out += "\n\\ No newline at end of file\n";
        }
        c = last + 1;
    }
    return out;
}

}  // namespace baker_liveops
