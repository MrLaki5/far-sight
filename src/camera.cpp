#include "camera.h"

#include <cerrno>
#include <cstring>
#include <ctime>
#include <fcntl.h>
#include <linux/videodev2.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

namespace {

int xioctl(int fd, unsigned long req, void* arg) {
    int r;
    do {
        r = ioctl(fd, req, arg);
    } while (r == -1 && errno == EINTR);
    return r;
}

std::string errno_str(const char* what) { return std::string(what) + ": " + std::strerror(errno); }

// Map a V4L2 monotonic buffer timestamp onto the wall clock.
std::chrono::system_clock::time_point to_wall_clock(const timeval& tv) {
    timespec now_mono{};
    clock_gettime(CLOCK_MONOTONIC, &now_mono);
    const auto now_wall = std::chrono::system_clock::now();
    const int64_t mono_now_us = static_cast<int64_t>(now_mono.tv_sec) * 1000000 + now_mono.tv_nsec / 1000;
    const int64_t buf_us = static_cast<int64_t>(tv.tv_sec) * 1000000 + tv.tv_usec;
    const int64_t age_us = mono_now_us - buf_us;
    if (buf_us == 0 || age_us < 0 || age_us > 10'000'000) return now_wall;
    return now_wall - std::chrono::microseconds(age_us);
}

}  // namespace

Camera::~Camera() { close(); }

std::string Camera::format_name() const {
    if (pixfmt_ == V4L2_PIX_FMT_MJPEG) return "MJPEG";
    if (pixfmt_ == V4L2_PIX_FMT_YUYV) return "YUYV";
    return "unknown";
}

bool Camera::open(const Options& opts, std::string& err) {
    close();

    fd_ = ::open(opts.device.c_str(), O_RDWR | O_NONBLOCK | O_CLOEXEC);
    if (fd_ < 0) {
        err = errno_str(("open " + opts.device).c_str());
        return false;
    }

    v4l2_capability cap{};
    if (xioctl(fd_, VIDIOC_QUERYCAP, &cap) < 0) {
        err = errno_str("VIDIOC_QUERYCAP");
        close();
        return false;
    }
    const uint32_t caps = (cap.capabilities & V4L2_CAP_DEVICE_CAPS) ? cap.device_caps : cap.capabilities;
    if (!(caps & V4L2_CAP_VIDEO_CAPTURE) || !(caps & V4L2_CAP_STREAMING)) {
        err = opts.device + " is not a streaming capture device (try another /dev/videoN)";
        close();
        return false;
    }

    bool format_ok = false;
    for (uint32_t want : {V4L2_PIX_FMT_MJPEG, V4L2_PIX_FMT_YUYV}) {
        v4l2_format fmt{};
        fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        fmt.fmt.pix.width = static_cast<uint32_t>(opts.width);
        fmt.fmt.pix.height = static_cast<uint32_t>(opts.height);
        fmt.fmt.pix.pixelformat = want;
        fmt.fmt.pix.field = V4L2_FIELD_ANY;
        if (xioctl(fd_, VIDIOC_S_FMT, &fmt) == 0 && fmt.fmt.pix.pixelformat == want) {
            width_ = static_cast<int>(fmt.fmt.pix.width);
            height_ = static_cast<int>(fmt.fmt.pix.height);
            pixfmt_ = want;
            format_ok = true;
            break;
        }
    }
    if (!format_ok) {
        err = "camera supports neither MJPEG nor YUYV";
        close();
        return false;
    }

    v4l2_streamparm parm{};
    parm.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    parm.parm.capture.timeperframe.numerator = 1;
    parm.parm.capture.timeperframe.denominator = static_cast<uint32_t>(opts.fps > 0 ? opts.fps : 15);
    xioctl(fd_, VIDIOC_S_PARM, &parm);  // best effort

    v4l2_requestbuffers req{};
    req.count = 4;
    req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    req.memory = V4L2_MEMORY_MMAP;
    if (xioctl(fd_, VIDIOC_REQBUFS, &req) < 0 || req.count < 2) {
        err = errno_str("VIDIOC_REQBUFS");
        close();
        return false;
    }

    buffers_.resize(req.count);
    for (uint32_t i = 0; i < req.count; ++i) {
        v4l2_buffer buf{};
        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory = V4L2_MEMORY_MMAP;
        buf.index = i;
        if (xioctl(fd_, VIDIOC_QUERYBUF, &buf) < 0) {
            err = errno_str("VIDIOC_QUERYBUF");
            close();
            return false;
        }
        void* p = mmap(nullptr, buf.length, PROT_READ | PROT_WRITE, MAP_SHARED, fd_, buf.m.offset);
        if (p == MAP_FAILED) {
            err = errno_str("mmap");
            close();
            return false;
        }
        buffers_[i] = {p, buf.length};
        if (xioctl(fd_, VIDIOC_QBUF, &buf) < 0) {
            err = errno_str("VIDIOC_QBUF");
            close();
            return false;
        }
    }

    v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (xioctl(fd_, VIDIOC_STREAMON, &type) < 0) {
        err = errno_str("VIDIOC_STREAMON");
        close();
        return false;
    }
    streaming_ = true;
    return true;
}

void Camera::close() {
    if (fd_ < 0) return;
    if (streaming_) {
        v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        xioctl(fd_, VIDIOC_STREAMOFF, &type);
        streaming_ = false;
    }
    for (auto& b : buffers_)
        if (b.start) munmap(b.start, b.length);
    buffers_.clear();
    ::close(fd_);
    fd_ = -1;
}

bool Camera::grab(Image& out, std::chrono::system_clock::time_point& captured_at, int timeout_ms, std::string& err) {
    if (fd_ < 0) {
        err = "camera not open";
        return false;
    }

    // Requeue everything already waiting: those frames were captured while we were busy.
    for (;;) {
        v4l2_buffer buf{};
        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory = V4L2_MEMORY_MMAP;
        if (xioctl(fd_, VIDIOC_DQBUF, &buf) < 0) {
            if (errno == EAGAIN) break;
            err = errno_str("VIDIOC_DQBUF");
            return false;
        }
        xioctl(fd_, VIDIOC_QBUF, &buf);
    }

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    for (;;) {
        const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now());
        pollfd pfd{fd_, POLLIN, 0};
        const int pr = left.count() > 0 ? poll(&pfd, 1, static_cast<int>(left.count())) : 0;
        if (pr < 0 && errno == EINTR) {
            err = "interrupted";
            return false;
        }
        if (pr <= 0) {
            err = pr == 0 ? "timed out waiting for a frame" : errno_str("poll");
            return false;
        }

        v4l2_buffer buf{};
        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory = V4L2_MEMORY_MMAP;
        if (xioctl(fd_, VIDIOC_DQBUF, &buf) < 0) {
            if (errno == EAGAIN) continue;
            err = errno_str("VIDIOC_DQBUF");
            return false;
        }

        bool ok = false;
        if (!(buf.flags & V4L2_BUF_FLAG_ERROR) && buf.bytesused > 0) {
            const auto* data = static_cast<const uint8_t*>(buffers_[buf.index].start);
            if (pixfmt_ == V4L2_PIX_FMT_MJPEG) {
                ok = decode_jpeg(data, buf.bytesused, out);
            } else if (buf.bytesused >= static_cast<uint32_t>(width_ * height_ * 2)) {
                yuyv_to_rgb(data, width_, height_, out);
                ok = true;
            }
            captured_at = to_wall_clock(buf.timestamp);
        }
        xioctl(fd_, VIDIOC_QBUF, &buf);
        if (ok) return true;
        // Corrupt or short frame (common right after stream start): wait for the next one.
    }
}
