#include "retire.hpp"

#include "../publish/stack.hpp"

#include <static/static.hpp>

#include <baker/config.hpp>

#include <cstdlib>
#include <map>

namespace baker_liveops {

namespace fs = std::filesystem;

int retire_with_config(const Game& game, const fs::path& base, const fs::path& overlay,
                       const std::string& into, std::string& patch, std::string& message) {
    if constexpr (baker::kShipping) {
        message = "retire needs a development build of Baker: promote writes files";
        return 2;
    }
    baker::Config cfg({.edit_mode = true, .tweaks = nullptr});
    if (!load_base(cfg, game, base, message)) return 2;
    const std::string layer = "retiring";
    if (auto r = cfg.push_layer(layer, fs::absolute(overlay).string()); !r)
        return message = std::string(baker::to_string(r.error())), 2;

    std::map<std::string, std::string> captured;
    cfg.set_writer([&captured](std::string_view path, std::string_view text) {
        captured[std::string(path)] = std::string(text);
        return true;
    });
    auto report = cfg.promote(layer, into);
    if (!report) return message = std::string(baker::to_string(report.error())), 2;
    for (const baker::Error& e : report->refused) message += "refused: " + std::string(baker::to_string(e)) + "\n";
    if (!report->ok()) return 1;

    const fs::path scratch = temp_dir("retire");
    patch.clear();
    for (const auto& [path, text] : captured) {
        const fs::path after = scratch / "after";
        if (!static_backend::write_file(after, text)) return message = "cannot write " + after.string(), 2;
        const std::string rel = fs::relative(fs::path(path), base).generic_string();
        std::string diff;
        const int code = run("diff -u --label 'a/" + rel + "' --label 'b/" + rel + "' '" + path + "' '" +
                                 after.string() + "'",
                             &diff);
        if (code > 1) return message = "diff failed on " + path, 2;
        patch += diff;
        // A generated file is changed at its source: the patch shows the values to carry there.
        std::error_code ec;
        for (const baker::Origin& o : cfg.read_session().origins())
            if (fs::equivalent(fs::path(std::string(o.file.str())), fs::path(path), ec))
                message += "note: " + rel + " is generated from " + std::string(o.source.str()) +
                           (o.tool.str().empty() ? "" : " by " + std::string(o.tool.str())) +
                           ": make the change there and generate it again; the patch shows the values\n";
    }
    fs::remove_all(scratch);
    for (const baker::WrittenLine& w : report->written)
        message += "  " + fs::relative(fs::path(std::string(w.file.str())), base).generic_string() + ":" +
                   std::to_string(w.line) + "\n";
    return 0;
}

int retire_with_stack(const std::string& binary, const fs::path& data_dir, const std::string& overlay_text,
                      const std::string& into, std::string& patch, std::string& message) {
    const fs::path scratch = temp_dir("retire-stack");
    const fs::path data = copy_stack(data_dir, scratch);
    if (data.empty()) return message = "cannot copy the stack from " + data_dir.string(), 2;
    static_backend::write_file(data / "agent.dta", overlay_text);
    const fs::path out = scratch / "retire.patch";
    std::string output;
    const int code = run("'" + binary + "' promote --baker-to " + into + " --data '" + data.string() +
                             "' --baker-patch '" + out.string() + "'",
                         &output);
    if (auto text = static_backend::read_file(out)) patch = *text;
    fs::remove_all(scratch);
    if (code != 0) return message = output, code == 1 ? 1 : 2;
    message = "the patch names files relative to the stack's data directory: apply it from there\n";
    return 0;
}

}  // namespace baker_liveops
