#include "image.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

// Static so these copies don't clash with the stb_image that libmtmd already links in.
#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_JPEG
#define STBI_ONLY_PNG
#include "stb/stb_image.h"

#define STB_IMAGE_WRITE_STATIC
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb/stb_image_write.h"

namespace {

bool from_stbi(uint8_t* pixels, int w, int h, Image& out) {
    if (!pixels) return false;
    out.width = w;
    out.height = h;
    out.rgb.assign(pixels, pixels + static_cast<size_t>(w) * h * 3);
    stbi_image_free(pixels);
    return true;
}

uint8_t clamp_u8(int v) { return static_cast<uint8_t>(std::clamp(v, 0, 255)); }

// Box-filter downscale; dst dimensions must not exceed src.
Image box_resize(const Image& src, int dw, int dh) {
    Image dst;
    dst.width = dw;
    dst.height = dh;
    dst.rgb.resize(static_cast<size_t>(dw) * dh * 3);
    for (int y = 0; y < dh; ++y) {
        const int y0 = static_cast<int>(static_cast<int64_t>(y) * src.height / dh);
        const int y1 = std::max(y0 + 1, static_cast<int>(static_cast<int64_t>(y + 1) * src.height / dh));
        for (int x = 0; x < dw; ++x) {
            const int x0 = static_cast<int>(static_cast<int64_t>(x) * src.width / dw);
            const int x1 = std::max(x0 + 1, static_cast<int>(static_cast<int64_t>(x + 1) * src.width / dw));
            uint32_t sum[3] = {0, 0, 0};
            for (int sy = y0; sy < y1; ++sy) {
                const uint8_t* p = &src.rgb[(static_cast<size_t>(sy) * src.width + x0) * 3];
                for (int sx = x0; sx < x1; ++sx, p += 3) {
                    sum[0] += p[0];
                    sum[1] += p[1];
                    sum[2] += p[2];
                }
            }
            const uint32_t n = static_cast<uint32_t>((y1 - y0) * (x1 - x0));
            uint8_t* d = &dst.rgb[(static_cast<size_t>(y) * dw + x) * 3];
            d[0] = static_cast<uint8_t>(sum[0] / n);
            d[1] = static_cast<uint8_t>(sum[1] / n);
            d[2] = static_cast<uint8_t>(sum[2] / n);
        }
    }
    return dst;
}

// 5x7 bitmap font, one byte per row, low 5 bits used (MSB = leftmost column).
const uint8_t* glyph(char c) {
    static const uint8_t digits[10][7] = {
        {0x0E, 0x11, 0x13, 0x15, 0x19, 0x11, 0x0E}, {0x04, 0x0C, 0x04, 0x04, 0x04, 0x04, 0x0E},
        {0x0E, 0x11, 0x01, 0x02, 0x04, 0x08, 0x1F}, {0x1F, 0x02, 0x04, 0x02, 0x01, 0x11, 0x0E},
        {0x02, 0x06, 0x0A, 0x12, 0x1F, 0x02, 0x02}, {0x1F, 0x10, 0x1E, 0x01, 0x01, 0x11, 0x0E},
        {0x06, 0x08, 0x10, 0x1E, 0x11, 0x11, 0x0E}, {0x1F, 0x01, 0x02, 0x04, 0x08, 0x08, 0x08},
        {0x0E, 0x11, 0x11, 0x0E, 0x11, 0x11, 0x0E}, {0x0E, 0x11, 0x11, 0x0F, 0x01, 0x02, 0x0C},
    };
    static const uint8_t dash[7]  = {0x00, 0x00, 0x00, 0x1F, 0x00, 0x00, 0x00};
    static const uint8_t colon[7] = {0x00, 0x0C, 0x0C, 0x00, 0x0C, 0x0C, 0x00};
    static const uint8_t dot[7]   = {0x00, 0x00, 0x00, 0x00, 0x00, 0x0C, 0x0C};
    static const uint8_t blank[7] = {0, 0, 0, 0, 0, 0, 0};
    if (c >= '0' && c <= '9') return digits[c - '0'];
    if (c == '-') return dash;
    if (c == ':') return colon;
    if (c == '.') return dot;
    return blank;
}

}  // namespace

bool decode_jpeg(const uint8_t* data, size_t size, Image& out) {
    int w = 0, h = 0, n = 0;
    uint8_t* px = stbi_load_from_memory(data, static_cast<int>(size), &w, &h, &n, 3);
    return from_stbi(px, w, h, out);
}

bool load_image_file(const std::string& path, Image& out) {
    int w = 0, h = 0, n = 0;
    uint8_t* px = stbi_load(path.c_str(), &w, &h, &n, 3);
    return from_stbi(px, w, h, out);
}

void yuyv_to_rgb(const uint8_t* data, int width, int height, Image& out) {
    out.width = width;
    out.height = height;
    out.rgb.resize(static_cast<size_t>(width) * height * 3);
    const size_t pairs = static_cast<size_t>(width) * height / 2;
    uint8_t* d = out.rgb.data();
    for (size_t i = 0; i < pairs; ++i, data += 4) {
        const int u = data[1] - 128;
        const int v = data[3] - 128;
        for (int k = 0; k < 2; ++k) {
            const int c = 298 * (data[k * 2] - 16);
            *d++ = clamp_u8((c + 409 * v + 128) >> 8);
            *d++ = clamp_u8((c - 100 * u - 208 * v + 128) >> 8);
            *d++ = clamp_u8((c + 516 * u + 128) >> 8);
        }
    }
}

Image resize_to_fit(const Image& src, int max_side) {
    const int longest = std::max(src.width, src.height);
    if (max_side <= 0 || longest <= max_side) return src;
    const double s = static_cast<double>(max_side) / longest;
    const int dw = std::max(1, static_cast<int>(std::lround(src.width * s)));
    const int dh = std::max(1, static_cast<int>(std::lround(src.height * s)));
    return box_resize(src, dw, dh);
}

std::vector<uint8_t> gray_thumbnail(const Image& src, int tw, int th) {
    const Image small = box_resize(src, std::min(tw, src.width), std::min(th, src.height));
    std::vector<uint8_t> gray(static_cast<size_t>(small.width) * small.height);
    for (size_t i = 0; i < gray.size(); ++i) {
        const uint8_t* p = &small.rgb[i * 3];
        gray[i] = static_cast<uint8_t>((77 * p[0] + 150 * p[1] + 29 * p[2]) >> 8);
    }
    return gray;
}

double mean_abs_diff(const std::vector<uint8_t>& a, const std::vector<uint8_t>& b) {
    if (a.size() != b.size() || a.empty()) return 255.0;
    uint64_t sum = 0;
    for (size_t i = 0; i < a.size(); ++i) sum += static_cast<uint64_t>(std::abs(a[i] - b[i]));
    return static_cast<double>(sum) / a.size();
}

void stamp_text(Image& img, const std::string& text) {
    if (img.empty() || text.empty()) return;
    const int scale = std::max(2, img.height / 240);
    const int advance = 6 * scale;
    const int pad = 2 * scale;
    const int box_w = std::min(img.width, static_cast<int>(text.size()) * advance + 2 * pad - scale);
    const int box_h = std::min(img.height, 7 * scale + 2 * pad);
    const int bx = 0, by = img.height - box_h;

    auto px = [&](int x, int y) { return &img.rgb[(static_cast<size_t>(y) * img.width + x) * 3]; };

    for (int y = by; y < by + box_h; ++y)
        for (int x = bx; x < bx + box_w; ++x) {
            uint8_t* p = px(x, y);
            for (int c = 0; c < 3; ++c) p[c] = static_cast<uint8_t>(p[c] * 3 / 10);
        }

    for (size_t i = 0; i < text.size(); ++i) {
        const uint8_t* g = glyph(text[i]);
        const int gx = bx + pad + static_cast<int>(i) * advance;
        for (int row = 0; row < 7; ++row)
            for (int col = 0; col < 5; ++col) {
                if (!(g[row] & (0x10 >> col))) continue;
                for (int dy = 0; dy < scale; ++dy)
                    for (int dx = 0; dx < scale; ++dx) {
                        const int x = gx + col * scale + dx, y = by + pad + row * scale + dy;
                        if (x < img.width && y < img.height) std::memset(px(x, y), 255, 3);
                    }
            }
    }
}

bool write_jpeg(const std::string& path, const Image& img, int quality) {
    // Write then rename so --on-match consumers never see a half-written file.
    const std::string tmp = path + ".part";
    if (!stbi_write_jpg(tmp.c_str(), img.width, img.height, 3, img.rgb.data(), std::clamp(quality, 1, 100))) {
        std::remove(tmp.c_str());
        return false;
    }
    return std::rename(tmp.c_str(), path.c_str()) == 0;
}
