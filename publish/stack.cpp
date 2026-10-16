#include "stack.hpp"

#include <static/static.hpp>

#include <cstdio>
#include <random>

#if defined(_WIN32)
#define popen _popen
#define pclose _pclose
#else
#include <sys/wait.h>
#endif

namespace baker_liveops {

namespace fs = std::filesystem;

fs::path temp_dir(const std::string& name) {
    std::random_device rd;
    fs::path p = fs::temp_directory_path() / ("baker-liveops-" + name + "-" + std::to_string(rd()));
    fs::remove_all(p);
    fs::create_directories(p);
    return p;
}

int run(const std::string& command, std::string* out) {
#if defined(_WIN32)
    const char* quiet = " >NUL 2>&1";
#else
    const char* quiet = " >/dev/null 2>&1";
#endif
    std::FILE* p = popen((command + (out ? " 2>&1" : quiet)).c_str(), "r");
    if (!p) return -1;
    char buf[4096];
    for (size_t n; (n = std::fread(buf, 1, sizeof buf, p)) > 0;)
        if (out) out->append(buf, n);
    const int status = pclose(p);
#if defined(_WIN32)
    return status;
#else
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
#endif
}

fs::path copy_stack(const fs::path& data_dir, const fs::path& into) {
    std::error_code ec;
    fs::path data = fs::weakly_canonical(fs::absolute(data_dir), ec);
    if (data.filename().empty()) data = data.parent_path();  // a trailing separator
    const fs::path parent = data.parent_path(), name = data.filename();
    for (const fs::path dir : {name, fs::path("sim"), fs::path("metrics"), fs::path("packs")})
        if (fs::is_directory(parent / dir))
            fs::copy(parent / dir, into / dir, fs::copy_options::recursive, ec);
    if (ec) return {};
    fs::remove(into / name / "agent.dta", ec);
    fs::remove(into / name / "local.dta", ec);
    return into / name;
}

ScenarioResult run_scenarios(const std::string& binary, const fs::path& data_dir,
                             const std::string& overlay_text) {
    ScenarioResult r;
    const fs::path scratch = temp_dir("scenarios");
    const fs::path data = copy_stack(data_dir, scratch);
    if (data.empty()) return r.report = "cannot copy the stack from " + data_dir.string(), r;
    auto sim = [&](const char* out) {
        return run("'" + binary + "' sim --data '" + data.string() + "' --out '" + (scratch / out).string() + "'");
    };
    if (sim("before") > 1) return r.report = "the stack's scenarios do not run on the base", r;
    static_backend::write_file(data / "agent.dta", overlay_text);
    if (sim("after") > 1) return r.report = "the stack's scenarios do not run with the overlay", r;
    const int code = run("'" + binary + "' compare '" + (scratch / "before").string() + "' '" +
                             (scratch / "after").string() + "'",
                         &r.report);
    r.code = code == 0 ? 0 : code == 1 ? 1 : 2;
    fs::remove_all(scratch);
    return r;
}

}  // namespace baker_liveops
