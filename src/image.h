#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

// Packed 8-bit RGB, row-major, no padding.
struct Image {
    int width = 0;
    int height = 0;
    std::vector<uint8_t> rgb;

    bool empty() const { return width <= 0 || height <= 0 || rgb.empty(); }
};

bool decode_jpeg(const uint8_t* data, size_t size, Image& out);
bool load_image_file(const std::string& path, Image& out);
void yuyv_to_rgb(const uint8_t* data, int width, int height, Image& out);

// Downscale so the longer side is at most max_side; returns a copy if already small enough.
Image resize_to_fit(const Image& src, int max_side);

// Tiny grayscale thumbnail used for cheap change detection.
std::vector<uint8_t> gray_thumbnail(const Image& src, int tw, int th);
double mean_abs_diff(const std::vector<uint8_t>& a, const std::vector<uint8_t>& b);

// Burn a text label (digits, '-', ':', '.', ' ') into the bottom-left corner.
void stamp_text(Image& img, const std::string& text);

bool write_jpeg(const std::string& path, const Image& img, int quality);
