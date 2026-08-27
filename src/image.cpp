#define STB_IMAGE_IMPLEMENTATION
#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STBI_FAILURE_USERMSG

#include "datum/image.hpp"

#include <stb_image.h>
#include <stb_image_write.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <string>

#include "datum/io.hpp"

namespace datum {
namespace {

void append_to_vector(void* context, void* data, int size) {
    auto* out = static_cast<std::vector<uint8_t>*>(context);
    const auto* bytes = static_cast<const uint8_t*>(data);
    out->insert(out->end(), bytes, bytes + size);
}

std::string lowercase(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return text;
}

}  // namespace

Image load(const std::filesystem::path& file) {
    const std::vector<uint8_t> encoded = read_bytes(file);
    if (encoded.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
        throw std::runtime_error(file.string() + " is too large to decode");
    }

    int width = 0;
    int height = 0;
    int channels = 0;
    uint8_t* decoded = stbi_load_from_memory(
        encoded.data(), static_cast<int>(encoded.size()), &width, &height, &channels, 0);
    if (decoded == nullptr) {
        throw std::runtime_error("cannot decode " + file.string() + ": " + stbi_failure_reason());
    }

    Image image;
    image.width = width;
    image.height = height;
    image.channels = channels;
    image.pixels.assign(decoded, decoded + image.sample_count());
    stbi_image_free(decoded);
    return image;
}

void save_png(const std::filesystem::path& file, const Image& image) {
    if (lowercase(file.extension().string()) != ".png") {
        throw std::runtime_error("refusing to write " + file.string() +
                                 ": only .png preserves exact pixel values");
    }
    if (image.width <= 0 || image.height <= 0 || image.channels < 1 || image.channels > 4) {
        throw std::runtime_error("image has invalid dimensions or channel count");
    }
    if (image.pixels.size() != image.sample_count()) {
        throw std::runtime_error("image buffer size does not match its dimensions");
    }

    std::vector<uint8_t> encoded;
    const int stride = image.width * image.channels;
    if (stbi_write_png_to_func(append_to_vector,
                               &encoded,
                               image.width,
                               image.height,
                               image.channels,
                               image.pixels.data(),
                               stride) == 0) {
        throw std::runtime_error("cannot encode " + file.string() + " as PNG");
    }
    write_bytes(file, encoded);
}

int luma(const Image& image, std::size_t pixel) {
    const std::size_t at = pixel * static_cast<std::size_t>(image.channels);
    if (image.color_channels() < 3) {
        return image.pixels[at];
    }
    return (77 * image.pixels[at] + 150 * image.pixels[at + 1] + 29 * image.pixels[at + 2] + 128) >>
           8;
}

double psnr_from_squared_error(double sum_squared_error, std::size_t samples) {
    if (samples == 0) {
        throw std::runtime_error("psnr: nothing to compare");
    }
    if (sum_squared_error == 0.0) {
        return std::numeric_limits<double>::infinity();
    }
    const double mse = sum_squared_error / static_cast<double>(samples);
    return 10.0 * std::log10(255.0 * 255.0 / mse);
}

double psnr(const Image& a, const Image& b) {
    if (a.pixels.size() != b.pixels.size()) {
        throw std::runtime_error("psnr: images differ in size");
    }

    double sum_squared_error = 0.0;
    for (std::size_t i = 0; i < a.pixels.size(); ++i) {
        const double diff = static_cast<double>(a.pixels[i]) - static_cast<double>(b.pixels[i]);
        sum_squared_error += diff * diff;
    }
    return psnr_from_squared_error(sum_squared_error, a.pixels.size());
}

double ssim(const Image& a, const Image& b) {
    if (a.width != b.width || a.height != b.height || a.channels != b.channels ||
        a.pixels.size() != b.pixels.size()) {
        throw std::runtime_error("ssim: images differ in shape");
    }

    // ponytail: 8x8 uniform windows instead of the published 11x11 Gaussian. On the
    // same pair of images the two agree to a few thousandths, and what this is used
    // for is ranking one delta against another, not the third decimal.
    constexpr int kWindow = 8;
    constexpr double kCount = kWindow * kWindow;
    constexpr double kC1 = 6.5025;   // (0.01 * 255)^2
    constexpr double kC2 = 58.5225;  // (0.03 * 255)^2

    double total = 0.0;
    std::size_t windows = 0;
    for (int y = 0; y + kWindow <= a.height; y += kWindow) {
        for (int x = 0; x + kWindow <= a.width; x += kWindow) {
            double sum_a = 0.0;
            double sum_b = 0.0;
            double sum_aa = 0.0;
            double sum_bb = 0.0;
            double sum_ab = 0.0;
            for (int j = 0; j < kWindow; ++j) {
                const auto row =
                    static_cast<std::size_t>(y + j) * static_cast<std::size_t>(a.width);
                for (int i = 0; i < kWindow; ++i) {
                    const std::size_t pixel = row + static_cast<std::size_t>(x + i);
                    const double va = luma(a, pixel);
                    const double vb = luma(b, pixel);
                    sum_a += va;
                    sum_b += vb;
                    sum_aa += va * va;
                    sum_bb += vb * vb;
                    sum_ab += va * vb;
                }
            }
            const double mean_a = sum_a / kCount;
            const double mean_b = sum_b / kCount;
            const double bessel = kCount / (kCount - 1.0);
            const double var_a = (sum_aa / kCount - mean_a * mean_a) * bessel;
            const double var_b = (sum_bb / kCount - mean_b * mean_b) * bessel;
            const double covariance = (sum_ab / kCount - mean_a * mean_b) * bessel;
            total += ((2.0 * mean_a * mean_b + kC1) * (2.0 * covariance + kC2)) /
                     ((mean_a * mean_a + mean_b * mean_b + kC1) * (var_a + var_b + kC2));
            ++windows;
        }
    }
    // Smaller than one window: nothing structural to compare, so nothing to report.
    return windows == 0 ? 1.0 : total / static_cast<double>(windows);
}

}  // namespace datum
