#include "Fetcher.h"

#include <fcntl.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cstdlib>
#include <filesystem>

namespace fs = std::filesystem;

extern char** environ;

namespace {

// Run a command with an explicit argv — no shell anywhere, so artifact names
// and zip patterns from the config can never be reinterpreted as shell syntax
// (and unzip receives its member glob unexpanded, which is what it wants).
// Returns the exit status, or -1 if the binary could not be started.
int run_cmd(const std::vector<std::string>& args, bool silence_stderr = false) {
    std::vector<char*> argv;
    argv.reserve(args.size() + 1);
    for (const auto& a : args) argv.push_back(const_cast<char*>(a.c_str()));
    argv.push_back(nullptr);

    // Downloads are chatty; keep the app's own log readable.
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_addopen(&fa, STDOUT_FILENO, "/dev/null", O_WRONLY, 0);
    if (silence_stderr)
        posix_spawn_file_actions_addopen(&fa, STDERR_FILENO, "/dev/null", O_WRONLY, 0);

    pid_t pid = 0;
    const int rc = posix_spawnp(&pid, argv[0], &fa, nullptr, argv.data(), environ);
    posix_spawn_file_actions_destroy(&fa);
    if (rc != 0) return -1;

    int status = 0;
    if (waitpid(pid, &status, 0) < 0) return -1;
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

// Present = the binary could be *started*. Testing the exit status instead
// would be wrong: `unzip --version` is not a valid invocation, so unzip prints
// its usage and exits non-zero — which once made this report "unzip not
// installed" and silently skip every Axelera archive.
bool have_tool(const char* name) {
    return run_cmd({name, "--version"}, /*silence_stderr=*/true) != -1;
}

// Expand the zip `strip` pattern: %m -> artifact name.
std::string expand(const std::string& pattern, const std::string& artifact) {
    std::string out;
    for (size_t i = 0; i < pattern.size(); ++i) {
        if (pattern[i] == '%' && i + 1 < pattern.size() && pattern[i + 1] == 'm') {
            out += artifact;
            ++i;
        } else {
            out += pattern[i];
        }
    }
    return out;
}

}  // namespace

Fetcher::~Fetcher() { stop(); }

void Fetcher::start(const Catalog& catalog) {
    if (running_.load()) return;
    if (std::getenv("MB_BENCH_NO_DOWNLOAD")) return;

    auto todo = catalog.missing_downloadable();
    if (todo.empty()) return;

    if (th_.joinable()) th_.join();
    stop_flag_.store(false);

    // Copy the vendor table into the thread: the catalog gets re-scanned as
    // files land, rewriting its storage while this download is still going.
    std::vector<VendorSource> vendors;
    vendors.reserve(kAccelCount);
    for (int i = 0; i < kAccelCount; ++i) vendors.push_back(catalog.vendor(accel_at(i)));

    {
        std::lock_guard<std::mutex> lk(mu_);
        status_ = Status{};
        status_.running = true;
        status_.total = static_cast<int>(todo.size());
    }
    running_.store(true);

    th_ = std::thread([this, todo = std::move(todo), vendors = std::move(vendors)]() mutable {
        run(std::move(todo), std::move(vendors));
    });
}

void Fetcher::run(std::vector<Artifact> todo, std::vector<VendorSource> vendors) {
    const bool wget_ok = have_tool("wget");
    const bool unzip_ok = have_tool("unzip");

    for (const auto& a : todo) {
        if (stop_flag_.load()) break;
        const VendorSource& v = vendors[accel_index(a.accel)];

        {
            std::lock_guard<std::mutex> lk(mu_);
            status_.current = std::string(accel_name(a.accel)) + " / " + a.name;
        }

        std::string err;
        if (!wget_ok) {
            err = "wget not installed";
        } else if ((v.fetch == Fetch::Zip || v.fetch == Fetch::ZipFile) && !unzip_ok) {
            err = "unzip not installed";
        } else {
            err = fetch_one(a, v);
        }

        std::lock_guard<std::mutex> lk(mu_);
        ++status_.done;
        status_.finished_since_last = true;
        if (!err.empty()) {
            status_.errors.push_back(std::string(accel_name(a.accel)) + " / " +
                                     a.name + ": " + err);
        }
    }

    std::lock_guard<std::mutex> lk(mu_);
    status_.running = false;
    status_.current.clear();
    running_.store(false);
}

std::string Fetcher::fetch_one(const Artifact& a, const VendorSource& v) {
    std::error_code ec;
    const fs::path dest(a.path);
    fs::create_directories(dest.parent_path(), ec);

    // Everything lands in a `.part` sibling and is renamed on success, so an
    // interrupted fetch never leaves a truncated file that looks like a model.
    if (v.fetch == Fetch::File) {
        const fs::path part = dest.string() + ".part";
        fs::remove(part, ec);
        const std::string url = v.url + "/" + a.name;
        if (run_cmd({"wget", "-q", "--tries=3", "--timeout=30", "-O", part.string(), url}) != 0) {
            fs::remove(part, ec);
            return "download failed (" + url + ")";
        }
        if (fs::file_size(part, ec) == 0) {
            fs::remove(part, ec);
            return "downloaded an empty file (" + url + ")";
        }
        fs::rename(part, dest, ec);
        return ec ? ("could not place " + dest.string()) : std::string();
    }

    // MemryX's model explorer serves each model as a zip holding exactly one
    // .dfp at the archive root, so the zip is named after the artifact with its
    // extension swapped. Extract that one member and drop the archive.
    if (v.fetch == Fetch::ZipFile) {
        fs::path stem = dest;
        stem.replace_extension();
        const std::string url = v.url + "/" + stem.filename().string() + ".zip";
        const fs::path zip = dest.string() + ".zip.part";
        const fs::path staging = dest.string() + ".d.part";
        fs::remove(zip, ec);
        fs::remove_all(staging, ec);

        if (run_cmd({"wget", "-q", "--tries=3", "--timeout=60", "-O", zip.string(), url}) != 0) {
            fs::remove(zip, ec);
            return "download failed (" + url + ")";
        }
        fs::create_directories(staging, ec);
        const int rc = run_cmd({"unzip", "-q", "-o", "-j", zip.string(),
                                dest.filename().string(), "-d", staging.string()});
        fs::remove(zip, ec);
        const fs::path got = staging / dest.filename();
        if (rc != 0 || !fs::is_regular_file(got, ec)) {
            fs::remove_all(staging, ec);
            return "archive did not contain " + dest.filename().string();
        }
        fs::remove(dest, ec);
        fs::rename(got, dest, ec);
        fs::remove_all(staging, ec);
        return ec ? ("could not place " + dest.string()) : std::string();
    }

    if (v.fetch == Fetch::Zip) {
        const fs::path zip = dest.string() + ".zip.part";
        const fs::path staging = dest.string() + ".part";
        fs::remove(zip, ec);
        fs::remove_all(staging, ec);

        const std::string url = v.url + "/" + a.name + ".zip";
        if (run_cmd({"wget", "-q", "--tries=3", "--timeout=30", "-O", zip.string(), url}) != 0) {
            fs::remove(zip, ec);
            return "download failed (" + url + ")";
        }
        fs::create_directories(staging, ec);
        const std::string pattern = expand(v.strip, a.name) + "/*";
        const int rc = run_cmd({"unzip", "-q", "-o", "-j", zip.string(), pattern,
                                "-d", staging.string()});
        fs::remove(zip, ec);
        if (rc != 0) {
            fs::remove_all(staging, ec);
            return "unzip failed (pattern " + pattern + ")";
        }
        if (fs::is_empty(staging, ec)) {
            fs::remove_all(staging, ec);
            return "archive had nothing under " + pattern;
        }
        fs::remove_all(dest, ec);
        fs::rename(staging, dest, ec);
        return ec ? ("could not place " + dest.string()) : std::string();
    }

    return "no source configured";
}

void Fetcher::stop() {
    stop_flag_.store(true);
    if (th_.joinable()) th_.join();
    running_.store(false);
}

Fetcher::Status Fetcher::snapshot() {
    std::lock_guard<std::mutex> lk(mu_);
    Status s = status_;
    status_.finished_since_last = false;  // consume the edge
    return s;
}
