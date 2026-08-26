#define STB_IMAGE_IMPLEMENTATION
#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STBI_FAILURE_USERMSG

#include "datum/image.hpp"

#include <stb_image.h>
#include <stb_image_write.h>

#include <algorithm>
#include <cctype>
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

}  // namespace datum
