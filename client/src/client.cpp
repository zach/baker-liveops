#include <baker_liveops/client.hpp>

#include <baker/parse.hpp>

#include <algorithm>
#include <charconv>
#include <fstream>
#include <sstream>

namespace baker_liveops {
namespace {

bool same(const Entry& a, const Entry& b) { return a.name == b.name && a.sha256 == b.sha256; }

std::string first_error(const baker::LayerReport& r) {
    std::string s = std::string(baker::to_string(r.refusals[0]));
    if (r.refusals.size() > 1) s += " (and " + std::to_string(r.refusals.size() - 1) + " more)";
    return s;
}

}  // namespace

Client::Client(baker::Config& cfg, ClientOptions options) : cfg_(cfg), opt_(std::move(options)) {}

void Client::note(std::string_view line) const {
    if (opt_.log) opt_.log(line);
}

const baker::LayerPolicy* Client::policy(std::string_view name) const {
    for (const NamedPolicy& p : opt_.policies)
        if (p.name == name) return &p.policy;
    return nullptr;
}

std::string Client::path(std::string_view name) const {
    return opt_.prefix + std::string(name) + ".dta";
}

void Client::attach(baker::LoadSession& session) {
    below_ = session.reader();
    session.set_reader([this](std::string_view p, baker::TextSink out) { return read(p, out); });
}

// Overlays by name, from memory; anything else as before. A name under the prefix that is
// not a verified overlay is not read from anywhere.
bool Client::read(std::string_view p, baker::TextSink out) const {
    if (p.starts_with(opt_.prefix)) {
        for (const auto* set : {&staged_, &live_})
            for (const Overlay& o : *set)
                if (path(o.entry.name) == p) return out(o.text), true;
        return false;
    }
    if (below_) return below_(p, out);
    std::ifstream in{std::string(p), std::ios::binary};
    if (!in) return false;
    std::ostringstream text;
    text << in.rdbuf();
    out(text.view());
    return true;
}

bool Client::rolled_back(const Entry& e) const {
    const auto it = rolled_back_.find(e.name);
    return it != rolled_back_.end() && it->second == e.sha256;
}

// Verifies a manifest and each overlay it names, then checks each with Baker. An overlay
// that fails is dropped with a log line; a manifest that fails stages nothing.
std::optional<std::vector<Client::Overlay>> Client::stage(std::string_view text, bool from_store) {
    std::string error;
    auto m = verify_manifest(text, opt_.keys, error);
    if (m && m->environment != opt_.environment)
        error = "manifest is for environment " + m->environment, m.reset();
    if (m && (from_store ? m->sequence < sequence_ : m->sequence <= sequence_))
        error = "manifest sequence " + std::to_string(m->sequence) + " is not newer than " +
                std::to_string(sequence_) + ": a replay",
        m.reset();
    if (!m) return note("refused: " + error), std::nullopt;
    sequence_ = m->sequence;

    std::vector<Overlay> out;
    for (const Entry& e : m->overlays) {
        if (rolled_back(e)) {
            note(e.name + ": rolled back here; skipped");
            continue;
        }
        const baker::LayerPolicy* p = policy(e.policy);
        if (!p) {
            note("refused " + e.name + ": the game has no policy named " + e.policy);
            continue;
        }
        std::optional<std::string> body;
        for (const auto* set : {&staged_, &live_})
            for (const Overlay& o : *set)
                if (!body && same(o.entry, e)) body = o.text;
        if (!body && e.size <= opt_.max_overlay_bytes)
            body = from_store ? (opt_.store ? opt_.store->load("overlay." + e.name) : std::nullopt)
                              : opt_.transport->fetch_overlay(opt_.environment, e);
        if (!body) {
            note("refused " + e.name + ": could not fetch it, or it is too large");
            continue;
        }
        if (!verify_overlay(e, *body, error)) {
            note("refused " + error);
            continue;
        }
        // Verified: now Baker may read it, as text, under the policy.
        auto report = cfg_.check_layer_text(path(e.name), *body, *p);
        if (!report) {
            note("refused " + e.name + ": " + std::string(baker::to_string(report.error())));
            continue;
        }
        if (!report->ok()) {
            note("refused " + e.name + ": " + first_error(*report));
            continue;
        }
        note("staged " + e.name + " " + e.version);
        out.push_back(Overlay{e, std::move(*body), std::move(*report), cfg_.epoch(), {}, {}});
    }
    return out;
}

bool Client::restore() {
    if (!opt_.store) return true;
    if (auto s = opt_.store->load("sequence")) (void)std::from_chars(s->data(), s->data() + s->size(), sequence_);
    if (auto rb = opt_.store->load("rolled_back")) {
        std::istringstream in(*rb);
        for (std::string name, sha; in >> name >> sha;) rolled_back_[name] = sha;
    }
    auto text = opt_.store->load("manifest");
    if (!text) return true;
    auto set = stage(*text, true);
    if (!set) return false;
    staged_ = std::move(*set);
    staged_manifest_ = std::move(*text);
    return true;
}

bool Client::refresh() {
    auto text = opt_.transport ? opt_.transport->fetch_manifest(opt_.environment) : std::nullopt;
    if (!text) return note("no manifest: the store is unreachable or empty"), false;
    if (*text == staged_manifest_ || *text == live_manifest_) return false;
    auto set = stage(*text, false);
    if (!set) return false;
    staged_ = std::move(*set);
    staged_manifest_ = std::move(*text);
    if (opt_.store) opt_.store->save("sequence", std::to_string(sequence_));
    return true;
}

bool Client::pending() const {
    if (staged_manifest_.empty()) return false;
    if (staged_.size() != live_.size()) return true;
    for (size_t i = 0; i < staged_.size(); ++i)
        if (!same(staged_[i].entry, live_[i].entry)) return true;
    return false;
}

bool Client::push(Overlay& o) {
    const bool checked = opt_.recheck == Recheck::IfStackChanged && o.epoch == cfg_.epoch();
    const baker::LayerPolicy* p = checked ? nullptr : policy(o.entry.policy);
    const std::string file = path(o.entry.name);
    const baker::LoadSession* read = &cfg_.read_session();
    baker::LoadSession session = cfg_.read_session();
    baker::Expected<void> r;
    if (opt_.mode == ApplyMode::Path) {
        r = cfg_.push_layer(o.entry.name, file, {.policy = p});
    } else {
        session.text_only = true;
        session.clear_autorun();
        session.clear_warnings();
        auto tree = baker::read_string(o.text, file, session);
        if (!tree) r = baker::Unexpected(std::move(tree).error());
        else if (p && p->warnings_refuse && !session.warnings().empty())
            r = baker::Unexpected(session.warnings()[0]);
        else r = cfg_.push_layer(o.entry.name, *tree, {.policy = p});
        read = &session;
    }
    if (!r) return note("refused " + o.entry.name + ": " + std::string(baker::to_string(r.error()))), false;
    // What Baker read, for live(): the pack's version and the origin.
    for (const baker::Origin& origin : read->origins())
        if (std::string_view(origin.file.str()).ends_with(file)) {
            o.origin = origin.source.str();
            o.tool = origin.tool.str();
        }
    for (const baker::Pack& pack : read->packs())
        if (std::string_view(pack.file.str()).ends_with(file) && pack.version.str() != o.entry.version)
            note(o.entry.name + ": its pack says version " + std::string(pack.version.str()) +
                 ", the manifest " + o.entry.version);
    note("applied " + o.entry.name + " " + o.entry.version);
    return true;
}

bool Client::apply() {
    if (!pending()) return true;
    bool ok = true;
    std::vector<Overlay> kept;
    for (size_t i = live_.size(); i-- > 0;) {
        const bool stays = std::any_of(staged_.begin(), staged_.end(),
                                       [&](const Overlay& s) { return same(s.entry, live_[i].entry); });
        if (stays) {
            kept.insert(kept.begin(), std::move(live_[i]));
        } else if (auto r = cfg_.remove_layer(live_[i].entry.name); !r) {
            note("could not remove " + live_[i].entry.name + ": " + std::string(baker::to_string(r.error())));
            kept.insert(kept.begin(), std::move(live_[i]));
            ok = false;
        } else {
            note("removed " + live_[i].entry.name + " " + live_[i].entry.version);
        }
    }
    live_ = std::move(kept);
    for (Overlay& s : staged_) {
        if (std::any_of(live_.begin(), live_.end(), [&](const Overlay& l) { return same(l.entry, s.entry); }))
            continue;
        if (push(s)) live_.push_back(s);
        else ok = false;
    }
    // What did not apply stays out of the staged set, so pending() reports the truth.
    staged_ = live_;
    live_manifest_ = staged_manifest_;
    save();
    return ok;
}

bool Client::rollback(std::string_view name) {
    const auto it = std::find_if(live_.begin(), live_.end(), [&](const Overlay& o) { return o.entry.name == name; });
    if (it == live_.end()) return note("no live overlay " + std::string(name)), false;
    if (auto r = cfg_.remove_layer(name); !r)
        return note("could not roll back " + std::string(name) + ": " + std::string(baker::to_string(r.error()))), false;
    note("rolled back " + it->entry.name + " " + it->entry.version);
    rolled_back_[it->entry.name] = it->entry.sha256;
    std::erase_if(staged_, [&](const Overlay& o) { return o.entry.name == name; });
    live_.erase(it);
    save();
    return true;
}

void Client::save() const {
    if (!opt_.store) return;
    for (const Overlay& o : live_) opt_.store->save("overlay." + o.entry.name, o.text);
    std::string rb;
    for (const auto& [name, sha] : rolled_back_) rb += name + " " + sha + "\n";
    opt_.store->save("rolled_back", rb);
    opt_.store->save("sequence", std::to_string(sequence_));
    opt_.store->save("manifest", live_manifest_);
}

std::vector<LiveOverlay> Client::live() const {
    std::vector<LiveOverlay> out;
    for (const Overlay& o : live_)
        out.push_back({o.entry.name, o.entry.version, path(o.entry.name), o.origin, o.tool, o.entry.sha256});
    return out;
}

const baker::LayerReport* Client::report(std::string_view name) const {
    for (const auto* set : {&staged_, &live_})
        for (const Overlay& o : *set)
            if (o.entry.name == name) return &o.report;
    return nullptr;
}

}  // namespace baker_liveops
