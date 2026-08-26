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

    std::size_t sample_count() const {
        return pixel_count() * static_cast<std::size_t>(channels);
    }

    /// Channels a codec may write into: the alpha channel is excluded, because
    /// altering it changes which pixels are drawn at all, not just their colour.
    int color_channels() const {
        return channels == 2 || channels == 4 ? channels - 1 : channels;
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

/// Peak signal-to-noise ratio in dB from an already-accumulated error total, so a
/// video can sum across frames without holding them all. Returns +infinity when
/// there is no error at all.
double psnr_from_squared_error(double sum_squared_error, std::size_t samples);

/// Peak signal-to-noise ratio in dB between two equally sized images, over every
/// sample. Higher means less distortion; above ~50 dB the change is below the
/// threshold of human vision. Returns +infinity when the images are identical.
/// Throws std::runtime_error if the buffers differ in size.
double psnr(const Image& a, const Image& b);

}  // namespace datum
