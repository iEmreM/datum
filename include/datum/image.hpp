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

/// BT.601 luma of one pixel, with integer weights that sum to 256 — so shifting
/// every colour channel by the same amount shifts the luma by exactly that amount.
/// A single-channel image is already its own luma, and alpha is ignored.
///
/// This is also what a lossy codec keeps: luma is stored at full resolution while
/// chroma is subsampled, so luma read back out of a decoded pixel is the value the
/// codec actually preserved. `qim` is built on that.
int luma(const Image& image, std::size_t pixel);

/// Peak signal-to-noise ratio in dB from an already-accumulated error total, so a
/// video can sum across frames without holding them all. Returns +infinity when
/// there is no error at all.
double psnr_from_squared_error(double sum_squared_error, std::size_t samples);

/// Peak signal-to-noise ratio in dB between two equally sized images, over every
/// sample. Higher means less distortion; above ~50 dB the change is below the
/// threshold of human vision. Returns +infinity when the images are identical.
/// Throws std::runtime_error if the buffers differ in size.
double psnr(const Image& a, const Image& b);

/// Structural similarity over the luma plane, in 8x8 windows. 1.0 means identical.
///
/// PSNR alone hides banding, which is the visible cost of a coarse quantiser:
/// flattening a gradient into steps moves each sample only a little, so the mean
/// squared error barely notices, but the structure it destroys is exactly what an
/// eye does notice. SSIM is the number that falls when that happens.
/// Throws std::runtime_error if the images differ in shape.
double ssim(const Image& a, const Image& b);

}  // namespace datum
