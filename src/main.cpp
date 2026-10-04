#include <algorithm>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>

#include "camera.h"
#include "events.h"
#include "image.h"
#include "options.h"
#include "vlm.h"

using Clock = std::chrono::steady_clock;
using std::chrono::duration_cast;
using std::chrono::milliseconds;

namespace {

std::atomic<bool> g_stop{false};

void on_signal(int) { g_stop = true; }

void sleep_interruptible(Clock::time_point until) {
    while (!g_stop && Clock::now() < until)
        std::this_thread::sleep_for(std::min<Clock::duration>(milliseconds(100), until - Clock::now()));
}

std::string now_str() { return format_local_time(std::chrono::system_clock::now()); }

int classify_still(const Options& opts, Vlm& vlm) {
    Image img;
    if (!load_image_file(opts.still_image, img)) {
        std::fprintf(stderr, "error: cannot read image %s\n", opts.still_image.c_str());
        return 1;
    }
    float p = 0;
    std::string err;
    const auto t0 = Clock::now();
    if (!vlm.ask_yes_no(resize_to_fit(img, opts.vlm_size), make_question(opts.prompt), p, err)) {
        std::fprintf(stderr, "error: %s\n", err.c_str());
        return 1;
    }
    const auto ms = duration_cast<milliseconds>(Clock::now() - t0).count();
    std::printf("p_yes=%.3f match=%s (%lld ms)\n", p, p >= opts.threshold ? "yes" : "no", static_cast<long long>(ms));
    return p >= opts.threshold ? 0 : 2;
}

bool open_camera(Camera& cam, const Options& opts) {
    std::string err;
    if (!cam.open(opts.camera, err)) {
        std::fprintf(stderr, "[%s] camera: %s\n", now_str().c_str(), err.c_str());
        return false;
    }
    std::fprintf(stderr, "[%s] camera %s opened at %dx%d %s\n", now_str().c_str(), opts.camera.device.c_str(),
                 cam.width(), cam.height(), cam.format_name().c_str());
    // Auto-exposure needs a few frames to settle; early frames are often dark.
    Image scratch;
    std::chrono::system_clock::time_point ts;
    for (int i = 0; i < opts.warmup_frames && !g_stop; ++i) cam.grab(scratch, ts, 2000, err);
    return true;
}

}  // namespace

int main(int argc, char** argv) {
    Options opts;
    bool show_help = false;
    std::string err;
    if (!parse_options(argc, argv, opts, show_help, err)) {
        std::fprintf(stderr, "error: %s\n\n", err.c_str());
        print_usage(argv[0]);
        return 1;
    }
    if (show_help) {
        print_usage(argv[0]);
        return 0;
    }

    std::signal(SIGINT, on_signal);
    std::signal(SIGTERM, on_signal);
    std::signal(SIGCHLD, SIG_IGN);  // auto-reap --on-match children

    std::fprintf(stderr, "[%s] loading model...\n", now_str().c_str());
    Vlm vlm;
    if (!vlm.load(opts.vlm, err)) {
        std::fprintf(stderr, "error: %s\n", err.c_str());
        return 1;
    }

    if (!opts.still_image.empty()) return classify_still(opts, vlm);

    EventSink sink;
    if (!sink.init(opts.events, err)) {
        std::fprintf(stderr, "error: %s\n", err.c_str());
        return 1;
    }

    Camera cam;
    if (!open_camera(cam, opts)) return 1;

    const std::string question = make_question(opts.prompt);
    std::fprintf(stderr, "[%s] watching for \"%s\" every %lldms (saving to %s)\n", now_str().c_str(),
                 opts.prompt.c_str(), static_cast<long long>(opts.interval.count()), opts.events.output_dir.c_str());

    std::vector<uint8_t> last_thumb;
    Clock::time_point last_check{};
    Clock::time_point cooldown_until{};
    int streak = 0;
    int grab_failures = 0;

    while (!g_stop) {
        const auto tick = Clock::now();

        Image frame;
        std::chrono::system_clock::time_point captured_at;
        if (!cam.grab(frame, captured_at, 3000, err)) {
            if (g_stop) break;
            std::fprintf(stderr, "[%s] grab failed: %s\n", now_str().c_str(), err.c_str());
            if (++grab_failures >= 3) {
                // Device may have been unplugged or reset; reopen it.
                cam.close();
                sleep_interruptible(Clock::now() + std::chrono::seconds(2));
                if (open_camera(cam, opts)) grab_failures = 0;
            }
            sleep_interruptible(tick + opts.interval);
            continue;
        }
        grab_failures = 0;

        // Cheap gate: only wake the model when the scene changed, unless a streak is in progress.
        auto thumb = gray_thumbnail(frame, 64, 48);
        if (opts.motion_threshold > 0 && streak == 0 && !last_thumb.empty() && tick - last_check < opts.max_idle) {
            const double diff = mean_abs_diff(thumb, last_thumb);
            if (diff < opts.motion_threshold) {
                if (opts.verbose) std::fprintf(stderr, "[%s] static scene (diff %.2f), skipped\n", now_str().c_str(), diff);
                sleep_interruptible(tick + opts.interval);
                continue;
            }
        }

        float p = 0;
        const auto t0 = Clock::now();
        if (!vlm.ask_yes_no(resize_to_fit(frame, opts.vlm_size), question, p, err)) {
            std::fprintf(stderr, "[%s] model error: %s\n", now_str().c_str(), err.c_str());
            sleep_interruptible(tick + opts.interval);
            continue;
        }
        const auto infer_ms = duration_cast<milliseconds>(Clock::now() - t0).count();
        last_thumb = std::move(thumb);
        last_check = tick;

        const bool match = p >= opts.threshold;
        streak = match ? streak + 1 : 0;
        if (opts.verbose || match)
            std::fprintf(stderr, "[%s] p_yes=%.3f %s streak=%d (%lld ms)\n", format_local_time(captured_at).c_str(), p,
                         match ? "MATCH" : "no", streak, static_cast<long long>(infer_ms));

        if (streak >= opts.consecutive && Clock::now() >= cooldown_until) {
            const std::string path = sink.fire(frame, captured_at, p, opts.prompt);
            if (path.empty())
                std::fprintf(stderr, "[%s] failed to save event image\n", now_str().c_str());
            else
                std::printf("%s %s %.3f\n", format_local_time(captured_at).c_str(), path.c_str(), p);
            std::fflush(stdout);
            cooldown_until = Clock::now() + opts.cooldown;
            streak = 0;
            if (opts.once) break;
        }

        sleep_interruptible(tick + opts.interval);
    }

    std::fprintf(stderr, "[%s] stopped\n", now_str().c_str());
    return 0;
}
