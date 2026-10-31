#include <baker_liveops/client.hpp>

#include <baker/parse.hpp>

#include <algorithm>
#include <charconv>
#include <sstream>

namespace baker_liveops {
namespace {

bool same(const Entry& a, const Entry& b) { return a.name == b.name && a.sha256 == b.sha256; }

std::string first_error(const baker::LayerReport& r) {
    std::string s(baker::to_string(r.refusals[0]));
    if (r.refusals.size() > 1) s += " (and " + std::to_string(r.refusals.size() - 1) + " more)";
    return s;
}

const baker::LayerPolicy* find_policy(const ClientOptions& opt, std::string_view name) {
    for (const NamedPolicy& p : opt.policies)
        if (p.name == name) return &p.policy;
    return nullptr;
}

std::string layer_of(const ClientOptions& opt, const Entry& e) {
    return opt.prefix + e.name + "@" + e.version + ".dta";
}

}  // namespace

Client::Client(baker::Config& cfg, ClientOptions options) : cfg_(cfg), opt_(std::move(options)) {}

void Client::note(std::string_view line) const {
    if (opt_.log) opt_.log(line);
}

const baker::LayerPolicy* Client::policy(std::string_view name) const { return find_policy(opt_, name); }

std::string Client::layer(const Entry& e) const { return layer_of(opt_, e); }

// What a refresh needs from the client, copied, and a checker of the stack as it stands.
Client::Refresh Client::prepare(bool from_store) const {
    Refresh job;
    job.opt = &opt_;
    job.checker = cfg_.checker();
    job.names = cfg_.read_session();
    job.names.text_only = true;
    job.names.refuse_includes = true;
    job.names.limits.max_new_symbols = opt_.max_new_symbols;
    job.from_store = from_store;
    job.floor = sequence_;
    job.known_manifests = {staged_manifest_, live_manifest_};
    for (const auto* set : {&staged_, &live_})
        for (const Staged& o : *set) job.known_texts[o.entry.sha256] = o.text;
    job.rolled_back = rolled_back_;
    return job;
}

Client::Refresh Client::begin_refresh() const { return prepare(false); }

// Verifies a manifest and each overlay it names, then checks each against the checker. An
// overlay that fails is dropped with a log line; a manifest that fails stages nothing.
void Client::Refresh::run() {
    auto note = [this](std::string line) { log.push_back(std::move(line)); };
    std::optional<std::string> text =
        from_store ? (opt->store ? opt->store->load("manifest") : std::nullopt)
                   : (opt->transport ? opt->transport->fetch_manifest(opt->environment) : std::nullopt);
    if (!text) {
        if (!from_store) note("no manifest: the store is unreachable or empty");
        return;
    }
    if (!from_store && std::find(known_manifests.begin(), known_manifests.end(), *text) != known_manifests.end())
        return;  // nothing new
    std::string error;
    auto m = verify_manifest(*text, opt->keys, error);
    if (m && m->environment != opt->environment)
        error = "manifest is for environment " + m->environment, m.reset();
    if (m && (from_store ? m->sequence < floor : m->sequence <= floor))
        error = "manifest sequence " + std::to_string(m->sequence) + " is not newer than " +
                std::to_string(floor) + ": a replay",
        m.reset();
    if (!m) return note("refused: " + error);

    for (const Entry& e : m->overlays) {
        if (auto it = rolled_back.find(e.name); it != rolled_back.end() && it->second == e.sha256) {
            note(e.name + ": rolled back here; skipped");
            continue;
        }
        const baker::LayerPolicy* p = find_policy(*opt, e.policy);
        if (!p) {
            note("refused " + e.name + ": the game has no policy named " + e.policy);
            continue;
        }
        std::optional<std::string> body;
        if (auto it = known_texts.find(e.sha256); it != known_texts.end()) body = it->second;
        else if (e.size <= opt->max_overlay_bytes)
            body = from_store ? (opt->store ? opt->store->load("overlay." + e.name) : std::nullopt)
                              : opt->transport->fetch_overlay(opt->environment, e);
        if (!body) {
            note("refused " + e.name + ": could not fetch it, or it is too large");
            continue;
        }
        if (!verify_overlay(e, *body, error)) {
            note("refused " + error);
            continue;
        }
        // Verified: now Baker may read it. First under the cap on new names, which the check
        // does not apply (requirements/0005); the check then finds them interned.
        {
            baker::LoadSession capped = names;
            capped.clear_autorun();
            if (auto read = baker::read_string(*body, layer_of(*opt, e), capped); !read) {
                note("refused " + e.name + ": " + std::string(baker::to_string(read.error())));
                continue;
            }
        }
        // Then as received text, under the policy.
        auto report = checker.check_text(layer_of(*opt, e), *body, *p);
        if (!report) {
            note("refused " + e.name + ": " + std::string(baker::to_string(report.error())));
            continue;
        }
        if (!report->ok()) {
            note("refused " + e.name + ": " + first_error(*report));
            continue;
        }
        note("staged " + e.name + " " + e.version);
        overlays.push_back(Staged{e, std::move(*body), std::move(*report), {}, {}});
    }
    manifest = std::move(*text);
    sequence = m->sequence;
    staged = true;
}

bool Client::finish_refresh(Refresh&& job) {
    for (const std::string& l : job.log) note(l);
    if (!job.staged) return false;
    // Another refresh may have finished first.
    if (job.from_store ? job.sequence < sequence_ : job.sequence <= sequence_)
        return note("dropped a refresh overtaken by a newer manifest"), false;
    sequence_ = job.sequence;
    staged_ = std::move(job.overlays);
    staged_manifest_ = std::move(job.manifest);
    if (opt_.store && !job.from_store) opt_.store->save("sequence", std::to_string(sequence_));
    return true;
}

bool Client::restore() {
    if (!opt_.store) return true;
    if (auto s = opt_.store->load("sequence")) (void)std::from_chars(s->data(), s->data() + s->size(), sequence_);
    if (auto rb = opt_.store->load("rolled_back")) {
        std::istringstream in(*rb);
        for (std::string name, sha; in >> name >> sha;) rolled_back_[name] = sha;
    }
    if (!opt_.store->load("manifest")) return true;
    Refresh job = prepare(true);
    job.run();
    const bool refused = !job.staged;
    finish_refresh(std::move(job));
    return !refused;
}

bool Client::refresh() {
    Refresh job = begin_refresh();
    job.run();
    return finish_refresh(std::move(job));
}

bool Client::pending() const {
    if (staged_manifest_.empty()) return false;
    if (staged_.size() != live_.size()) return true;
    for (size_t i = 0; i < staged_.size(); ++i)
        if (!same(staged_[i].entry, live_[i].entry)) return true;
    return false;
}

bool Client::push(Staged& o) {
    const std::string name = layer(o.entry);
    // The staged report stands if the Config has not changed since the check; else Baker
    // checks again under the same policy.
    if (auto r = cfg_.push_layer_text(name, o.text, {.policy = policy(o.entry.policy), .checked = &o.report}); !r)
        return note("refused " + o.entry.name + ": " + std::string(baker::to_string(r.error()))), false;
    if (const baker::Origin* origin = cfg_.read_session().origin_of(baker::Symbol(name))) {
        o.origin = origin->source.str();
        o.tool = origin->tool.str();
    }
    if (const baker::Pack* pack = cfg_.read_session().pack_of(baker::Symbol(name)); pack && pack->version.str() != o.entry.version)
        note(o.entry.name + ": its pack says version " + std::string(pack->version.str()) + ", the manifest " +
             o.entry.version);
    note("applied " + o.entry.name + " " + o.entry.version);
    return true;
}

bool Client::apply() {
    if (!pending()) return true;
    bool ok = true;
    std::vector<Staged> kept;
    for (size_t i = live_.size(); i-- > 0;) {
        const bool stays = std::any_of(staged_.begin(), staged_.end(),
                                       [&](const Staged& s) { return same(s.entry, live_[i].entry); });
        if (stays) {
            kept.insert(kept.begin(), std::move(live_[i]));
        } else if (auto r = cfg_.remove_layer(layer(live_[i].entry)); !r) {
            note("could not remove " + live_[i].entry.name + ": " + std::string(baker::to_string(r.error())));
            kept.insert(kept.begin(), std::move(live_[i]));
            ok = false;
        } else {
            note("removed " + live_[i].entry.name + " " + live_[i].entry.version);
        }
    }
    live_ = std::move(kept);
    for (Staged& s : staged_) {
        if (std::any_of(live_.begin(), live_.end(), [&](const Staged& l) { return same(l.entry, s.entry); }))
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
    const auto it = std::find_if(live_.begin(), live_.end(), [&](const Staged& o) { return o.entry.name == name; });
    if (it == live_.end()) return note("no live overlay " + std::string(name)), false;
    if (auto r = cfg_.remove_layer(layer(it->entry)); !r)
        return note("could not roll back " + std::string(name) + ": " + std::string(baker::to_string(r.error()))), false;
    note("rolled back " + it->entry.name + " " + it->entry.version);
    rolled_back_[it->entry.name] = it->entry.sha256;
    std::erase_if(staged_, [&](const Staged& o) { return o.entry.name == name; });
    live_.erase(it);
    save();
    return true;
}

void Client::save() const {
    if (!opt_.store) return;
    for (const Staged& o : live_) opt_.store->save("overlay." + o.entry.name, o.text);
    std::string rb;
    for (const auto& [name, sha] : rolled_back_) rb += name + " " + sha + "\n";
    opt_.store->save("rolled_back", rb);
    opt_.store->save("sequence", std::to_string(sequence_));
    opt_.store->save("manifest", live_manifest_);
}

std::vector<LiveOverlay> Client::live() const {
    std::vector<LiveOverlay> out;
    for (const Staged& o : live_)
        out.push_back({o.entry.name, o.entry.version, layer(o.entry), o.origin, o.tool, o.entry.sha256});
    return out;
}

const baker::LayerReport* Client::report(std::string_view name) const {
    for (const auto* set : {&staged_, &live_})
        for (const Staged& o : *set)
            if (o.entry.name == name) return &o.report;
    return nullptr;
}

}  // namespace baker_liveops
