#include "game.hpp"

#include <baker/parse.hpp>
#include <baker/shape.hpp>

namespace baker_liveops {
namespace {

std::string text_of(const baker::Node& n) {
    if (auto s = n.try_sym()) return std::string(s->str());
    if (auto s = n.try_str()) return std::string(*s);
    return {};
}

// The values after an array's tag, as text.
std::vector<std::string> values(const baker::Array* a) {
    std::vector<std::string> out;
    if (a)
        for (size_t i = 1; i < a->size(); ++i) out.push_back(text_of((*a)[i]));
    return out;
}

}  // namespace

std::vector<NamedPolicy> Game::named() const {
    std::vector<NamedPolicy> out;
    for (const GamePolicy& p : policies) out.push_back({p.name, p.policy});
    return out;
}

const GamePolicy* Game::policy(std::string_view name) const {
    for (const GamePolicy& p : policies)
        if (p.name == name) return &p;
    return nullptr;
}

std::optional<Game> load_game(const std::filesystem::path& file, std::string& error) {
    auto root = baker::read_file(file.string());
    if (!root) return error = std::string(baker::to_string(root.error())), std::nullopt;
    const baker::Array* desc = (*root)->try_find("liveops");
    if (!desc) return error = file.string() + ": no (liveops ...)", std::nullopt;
    Game g;
    g.pack_levels = values(desc->try_find("pack_levels"));
    g.include_dirs = values(desc->try_find("include_dirs"));
    if (const baker::Array* layers = desc->try_find("layers"))
        for (const baker::Array& l : layers->subarrays()) {
            const auto v = values(&l);
            if (v.size() != 1) return error = file.string() + ": a layer is (name file)", std::nullopt;
            g.layers.emplace_back(text_of(l[0]), v[0]);
        }
    g.shapes = desc->find_int("shapes", 0).value_or(0) != 0;
    if (const baker::Array* policies = desc->try_find("policies"))
        for (const baker::Array& p : policies->subarrays()) {
            GamePolicy& gp = g.policies.emplace_back();
            gp.name = text_of(p[0]);
            auto flag = [&](const char* tag) { return p.find_int(tag, 0).value_or(0) != 0; };
            gp.policy.data_only = flag("data_only");
            gp.policy.scalars_only = flag("scalars_only");
            gp.policy.existing_only = flag("existing_only");
            gp.policy.warnings_refuse = flag("warnings_refuse");
            for (const std::string& path : values(p.try_find("allow"))) gp.allow.emplace_back(path);
            gp.policy.allow = gp.allow;
        }
    if (g.layers.empty()) return error = file.string() + ": no layers", std::nullopt;
    return g;
}

bool load_base(baker::Config& cfg, const Game& game, const std::filesystem::path& base,
               std::string& error) {
    std::vector<baker::Symbol> levels;
    for (const std::string& l : game.pack_levels) levels.emplace_back(l);
    if (!levels.empty()) cfg.session().set_pack_levels(levels);
    for (const std::string& d : game.include_dirs) cfg.session().add_include_dir((base / d).string());
    if (game.shapes) cfg.add_validator(baker::shape::validator(), {.any_thread = true});
    for (const auto& [name, file] : game.layers)
        if (auto r = cfg.push_layer(name, (base / file).string()); !r)
            return error = std::string(baker::to_string(r.error())), false;
    return true;
}

}  // namespace baker_liveops
