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

}  // namespace datum
