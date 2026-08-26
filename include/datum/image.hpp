#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <vector>

namespace datum {

/// An 8-bit image with interleaved channels, row-major, no row padding.
struct Image {
    int width = 0;
    int height = 0;
    int channels = 0;             ///< 1 = grey, 2 = grey+alpha, 3 = RGB, 4 = RGBA
    std::vector<uint8_t> pixels;  ///< size() == width * height * channels

    std::size_t pixel_count() const {
        return static_cast<std::size_t>(width) * static_cast<std::size_t>(height);
    }

    /// Number of channel bytes an embedder may write into.
    std::size_t sample_count() const {
        return pixel_count() * static_cast<std::size_t>(channels);
    }

    bool empty() const {
        return pixels.empty();
    }
};

/// Decodes PNG, JPEG, BMP, TGA and GIF. Throws std::runtime_error on failure.
Image load(const std::filesystem::path& file);

/// Writes PNG only. Lossy formats quantise pixel values and would destroy any
/// embedded payload, so a non-`.png` extension is rejected rather than honoured.
/// Throws std::runtime_error on failure.
void save_png(const std::filesystem::path& file, const Image& image);

}  // namespace datum
