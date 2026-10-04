#include "events.h"

#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <spawn.h>
#include <vector>

extern char** environ;

namespace fs = std::filesystem;

namespace {

struct TimeParts {
    std::tm tm{};
    int millis = 0;
};

TimeParts split(std::chrono::system_clock::time_point tp) {
    TimeParts p;
    const std::time_t t = std::chrono::system_clock::to_time_t(tp);
    localtime_r(&t, &p.tm);
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(tp.time_since_epoch()).count() % 1000;
    p.millis = static_cast<int>(ms < 0 ? ms + 1000 : ms);
    return p;
}

std::string strf(const std::tm& tm, const char* fmt) {
    char buf[64];
    const size_t n = std::strftime(buf, sizeof(buf), fmt, &tm);
    return std::string(buf, n);
}

// ISO 8601 with millis and UTC offset, e.g. 2026-10-04T16:05:03.123+02:00.
std::string iso8601(std::chrono::system_clock::time_point tp) {
    const TimeParts p = split(tp);
    char ms[8];
    std::snprintf(ms, sizeof(ms), ".%03d", p.millis);
    std::string off = strf(p.tm, "%z");
    if (off.size() == 5) off.insert(3, ":");
    return strf(p.tm, "%Y-%m-%dT%H:%M:%S") + ms + off;
}

std::string csv_quote(const std::string& s) {
    std::string out = "\"";
    for (char c : s) out += (c == '"') ? std::string("\"\"") : std::string(1, c);
    return out + "\"";
}

}  // namespace

std::string format_local_time(std::chrono::system_clock::time_point tp) {
    const TimeParts p = split(tp);
    char ms[8];
    std::snprintf(ms, sizeof(ms), ".%03d", p.millis);
    return strf(p.tm, "%Y-%m-%d %H:%M:%S") + ms;
}

EventSink::~EventSink() {
    if (log_) std::fclose(log_);
}

bool EventSink::init(const Options& opts, std::string& err) {
    opts_ = opts;
    std::error_code ec;
    fs::create_directories(opts_.output_dir, ec);
    if (ec) {
        err = "cannot create " + opts_.output_dir + ": " + ec.message();
        return false;
    }
    const fs::path log_path = fs::path(opts_.output_dir) / "events.csv";
    const bool is_new = !fs::exists(log_path);
    log_ = std::fopen(log_path.c_str(), "a");
    if (!log_) {
        err = "cannot open " + log_path.string();
        return false;
    }
    if (is_new) std::fputs("captured_at,image,score,prompt\n", log_);
    std::fflush(log_);
    return true;
}

std::string EventSink::fire(const Image& frame, std::chrono::system_clock::time_point captured_at, float score,
                            const std::string& prompt) {
    const TimeParts p = split(captured_at);
    char ms[8];
    std::snprintf(ms, sizeof(ms), ".%03d", p.millis);
    const std::string base = strf(p.tm, "%Y-%m-%d_%H-%M-%S") + ms;

    // Two events in the same millisecond are unlikely but would otherwise overwrite each other.
    fs::path path = fs::path(opts_.output_dir) / (base + ".jpg");
    for (int i = 1; fs::exists(path); ++i) path = fs::path(opts_.output_dir) / (base + "_" + std::to_string(i) + ".jpg");

    Image out = frame;
    if (opts_.stamp) stamp_text(out, format_local_time(captured_at));
    if (!write_jpeg(path.string(), out, opts_.jpeg_quality)) return {};

    const std::string iso = iso8601(captured_at);
    if (log_) {
        std::fprintf(log_, "%s,%s,%.3f,%s\n", iso.c_str(), csv_quote(path.filename().string()).c_str(), score,
                     csv_quote(prompt).c_str());
        std::fflush(log_);
    }
    if (!opts_.on_match.empty()) run_hook(path.string(), iso, score, prompt);
    return path.string();
}

void EventSink::run_hook(const std::string& image_path, const std::string& iso_time, float score,
                         const std::string& prompt) const {
    char score_buf[16];
    std::snprintf(score_buf, sizeof(score_buf), "%.3f", score);
    std::vector<std::string> env_strings = {
        "FARSIGHT_IMAGE=" + image_path,
        "FARSIGHT_TIME=" + iso_time,
        std::string("FARSIGHT_SCORE=") + score_buf,
        "FARSIGHT_PROMPT=" + prompt,
    };
    std::vector<char*> envp;
    for (char** e = environ; *e; ++e)
        if (std::strncmp(*e, "FARSIGHT_", 9) != 0) envp.push_back(*e);
    for (auto& s : env_strings) envp.push_back(s.data());
    envp.push_back(nullptr);

    // posix_spawn rather than fork: llama.cpp worker threads make post-fork malloc unsafe.
    posix_spawnattr_t attr;
    posix_spawnattr_init(&attr);
    posix_spawnattr_setflags(&attr, POSIX_SPAWN_SETSID);
    const char* argv[] = {"sh", "-c", opts_.on_match.c_str(), nullptr};
    pid_t pid;
    if (posix_spawn(&pid, "/bin/sh", nullptr, &attr, const_cast<char* const*>(argv), envp.data()) != 0)
        std::fprintf(stderr, "far-sight: failed to run --on-match command\n");
    posix_spawnattr_destroy(&attr);
}
