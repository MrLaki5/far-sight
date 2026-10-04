#pragma once

#include <chrono>
#include <cstdint>
#include <string>
#include <vector>

#include "image.h"

// Minimal V4L2 capture (MJPEG preferred, YUYV fallback) using mmap'd buffers.
class Camera {
public:
    struct Options {
        std::string device = "/dev/video0";
        int width = 1280;
        int height = 720;
        int fps = 15;
    };

    Camera() = default;
    ~Camera();
    Camera(const Camera&) = delete;
    Camera& operator=(const Camera&) = delete;

    bool open(const Options& opts, std::string& err);
    void close();
    bool is_open() const { return fd_ >= 0; }

    // Returns a frame captured after this call started (stale queued frames are dropped).
    bool grab(Image& out, std::chrono::system_clock::time_point& captured_at, int timeout_ms, std::string& err);

    int width() const { return width_; }
    int height() const { return height_; }
    std::string format_name() const;

private:
    struct Buffer {
        void* start = nullptr;
        size_t length = 0;
    };

    int fd_ = -1;
    int width_ = 0;
    int height_ = 0;
    uint32_t pixfmt_ = 0;
    bool streaming_ = false;
    std::vector<Buffer> buffers_;
};
