#pragma once

#include <chrono>
#include <cstdio>
#include <string>

#include "image.h"

// Persists a detection: timestamped JPEG + a line in events.csv, then runs the optional hook.
class EventSink {
public:
    struct Options {
        std::string output_dir = "captures";
        std::string on_match;  // shell command, run detached
        int jpeg_quality = 90;
        bool stamp = true;
    };

    ~EventSink();

    bool init(const Options& opts, std::string& err);

    // Returns the path of the saved image, or empty on failure.
    std::string fire(const Image& frame, std::chrono::system_clock::time_point captured_at, float score,
                     const std::string& prompt);

private:
    void run_hook(const std::string& image_path, const std::string& iso_time, float score,
                  const std::string& prompt) const;

    Options opts_;
    std::FILE* log_ = nullptr;
};

// "2026-10-04 16:05:03.123" in local time.
std::string format_local_time(std::chrono::system_clock::time_point tp);
