#include "options.h"

#include <cstdio>
#include <cstdlib>
#include <stdexcept>

namespace {

// Accepts "250ms", "1.5s", "2m", "1h" or a bare number of seconds.
bool parse_duration(const std::string& s, std::chrono::milliseconds& out) {
    size_t pos = 0;
    double v;
    try {
        v = std::stod(s, &pos);
    } catch (const std::exception&) {
        return false;
    }
    const std::string unit = s.substr(pos);
    double ms;
    if (unit.empty() || unit == "s") ms = v * 1000.0;
    else if (unit == "ms") ms = v;
    else if (unit == "m") ms = v * 60'000.0;
    else if (unit == "h") ms = v * 3'600'000.0;
    else return false;
    if (ms < 0) return false;
    out = std::chrono::milliseconds(static_cast<long long>(ms));
    return true;
}

bool parse_int(const std::string& s, int& out) {
    char* end = nullptr;
    const long v = std::strtol(s.c_str(), &end, 10);
    if (s.empty() || *end) return false;
    out = static_cast<int>(v);
    return true;
}

bool parse_double(const std::string& s, double& out) {
    char* end = nullptr;
    out = std::strtod(s.c_str(), &end);
    return !s.empty() && !*end;
}

bool parse_size(const std::string& s, int& w, int& h) {
    return std::sscanf(s.c_str(), "%dx%d", &w, &h) == 2 && w > 0 && h > 0;
}

}  // namespace

void print_usage(const char* argv0) {
    std::printf(
        "Usage: %s --model M.gguf --mmproj P.gguf --prompt TEXT [options]\n"
        "\n"
        "Watches a camera, asks a vision-language model whether TEXT is happening, and saves a\n"
        "timestamped JPEG (plus a row in events.csv) whenever it is.\n"
        "\n"
        "Model:\n"
        "  -m, --model PATH          language model GGUF\n"
        "      --mmproj PATH         vision projector GGUF (mmproj-*.gguf)\n"
        "  -p, --prompt TEXT         event to look for, e.g. \"a person waving\"; text ending in\n"
        "                            '?' is sent verbatim as a yes/no question\n"
        "      --ngl N               layers to offload to GPU (default: all, 0 = CPU only)\n"
        "  -t, --threads N           CPU threads (default: auto)\n"
        "      --ctx N               context size (default: 4096)\n"
        "      --vlm-size PX         downscale frames to this longest side for the model (default: 512)\n"
        "\n"
        "Detection:\n"
        "      --interval DUR        time between checks (default: 1s)\n"
        "      --threshold P         min P(yes) to count as a match, 0..1 (default: 0.5)\n"
        "      --consecutive N       matches in a row needed to fire (default: 2)\n"
        "      --cooldown DUR        min time between events (default: 30s)\n"
        "      --motion-threshold D  skip the model when the scene changed less than D (mean abs\n"
        "                            gray diff, 0-255) since the last check; 0 disables (default: 2)\n"
        "      --max-idle DUR        force a check at least this often despite no motion (default: 60s)\n"
        "      --once                exit after the first event\n"
        "\n"
        "Camera:\n"
        "  -d, --device PATH         V4L2 device (default: /dev/video0)\n"
        "      --size WxH            capture resolution (default: 1280x720)\n"
        "      --fps N               capture frame rate (default: 15)\n"
        "      --warmup-frames N     frames to discard while auto-exposure settles (default: 5)\n"
        "\n"
        "Output:\n"
        "  -o, --output DIR          where images and events.csv go (default: ./captures)\n"
        "      --on-match CMD        shell command run on each event; gets FARSIGHT_IMAGE,\n"
        "                            FARSIGHT_TIME, FARSIGHT_SCORE, FARSIGHT_PROMPT in its env\n"
        "      --jpeg-quality N      1-100 (default: 90)\n"
        "      --no-stamp            don't burn the capture time into the saved image\n"
        "\n"
        "Other:\n"
        "      --image PATH          score a still image against --prompt and exit\n"
        "  -v, --verbose             show llama.cpp logs and per-check timings\n"
        "  -h, --help                show this help\n"
        "\n"
        "Durations accept ms, s, m, h suffixes (bare numbers are seconds).\n",
        argv0);
}

bool parse_options(int argc, char** argv, Options& o, bool& show_help, std::string& err) {
    show_help = false;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto value = [&](std::string& out) {
            if (i + 1 >= argc) {
                err = "missing value for " + a;
                return false;
            }
            out = argv[++i];
            return true;
        };
        std::string v;
        auto bad = [&]() {
            err = "invalid value for " + a + ": " + v;
            return false;
        };

        if (a == "-h" || a == "--help") {
            show_help = true;
            return true;
        } else if (a == "-v" || a == "--verbose") {
            o.verbose = o.vlm.verbose = true;
        } else if (a == "--once") {
            o.once = true;
        } else if (a == "--no-stamp") {
            o.events.stamp = false;
        } else if (!value(v)) {
            return false;
        } else if (a == "-m" || a == "--model") {
            o.vlm.model_path = v;
        } else if (a == "--mmproj") {
            o.vlm.mmproj_path = v;
        } else if (a == "-p" || a == "--prompt") {
            o.prompt = v;
        } else if (a == "--ngl") {
            if (!parse_int(v, o.vlm.n_gpu_layers)) return bad();
        } else if (a == "-t" || a == "--threads") {
            if (!parse_int(v, o.vlm.n_threads) || o.vlm.n_threads < 0) return bad();
        } else if (a == "--ctx") {
            if (!parse_int(v, o.vlm.n_ctx) || o.vlm.n_ctx < 512) return bad();
        } else if (a == "--vlm-size") {
            if (!parse_int(v, o.vlm_size) || o.vlm_size < 64) return bad();
        } else if (a == "--interval") {
            if (!parse_duration(v, o.interval)) return bad();
        } else if (a == "--cooldown") {
            if (!parse_duration(v, o.cooldown)) return bad();
        } else if (a == "--max-idle") {
            if (!parse_duration(v, o.max_idle)) return bad();
        } else if (a == "--threshold") {
            double d;
            if (!parse_double(v, d) || d < 0 || d > 1) return bad();
            o.threshold = static_cast<float>(d);
        } else if (a == "--consecutive") {
            if (!parse_int(v, o.consecutive) || o.consecutive < 1) return bad();
        } else if (a == "--motion-threshold") {
            if (!parse_double(v, o.motion_threshold) || o.motion_threshold < 0) return bad();
        } else if (a == "-d" || a == "--device") {
            o.camera.device = v;
        } else if (a == "--size") {
            if (!parse_size(v, o.camera.width, o.camera.height)) return bad();
        } else if (a == "--fps") {
            if (!parse_int(v, o.camera.fps) || o.camera.fps < 1) return bad();
        } else if (a == "--warmup-frames") {
            if (!parse_int(v, o.warmup_frames) || o.warmup_frames < 0) return bad();
        } else if (a == "-o" || a == "--output") {
            o.events.output_dir = v;
        } else if (a == "--on-match") {
            o.events.on_match = v;
        } else if (a == "--jpeg-quality") {
            if (!parse_int(v, o.events.jpeg_quality) || o.events.jpeg_quality < 1 || o.events.jpeg_quality > 100)
                return bad();
        } else if (a == "--image") {
            o.still_image = v;
        } else {
            err = "unknown option " + a;
            return false;
        }
    }

    if (o.vlm.model_path.empty()) err = "--model is required";
    else if (o.vlm.mmproj_path.empty()) err = "--mmproj is required";
    else if (o.prompt.empty()) err = "--prompt is required";
    return err.empty();
}

std::string make_question(const std::string& prompt) {
    if (!prompt.empty() && prompt.back() == '?') return prompt + " Answer with yes or no.";
    return "Does this image show " + prompt + "? Answer with yes or no.";
}
