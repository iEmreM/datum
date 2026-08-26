// Phase 0 check: a PNG written by datum reloads with byte-identical pixels, and
// lossy destinations are refused. Everything later in the roadmap assumes both.

#include <cstdint>
#include <filesystem>
#include <iostream>
#include <string>

#include "datum/image.hpp"

// Not <cassert>: Release defines NDEBUG and would compile the checks away.
#define CHECK(condition)                                                                         \
    do {                                                                                         \
        if (!(condition)) {                                                                      \
            std::cerr << "FAILED " << __FILE__ << ":" << __LINE__ << ": " << #condition << "\n"; \
            return false;                                                                        \
        }                                                                                        \
    } while (false)

namespace {

/// Deterministic noise, so every byte value appears — including 0x0A and 0x0D,
/// which are the ones a text-mode file handle would corrupt.
datum::Image make_image(int width, int height, int channels) {
    datum::Image image;
    image.width = width;
    image.height = height;
    image.channels = channels;
    image.pixels.resize(image.sample_count());

    uint32_t state = 0x1234567u;
    for (auto& sample : image.pixels) {
        state = state * 1664525u + 1013904223u;
        sample = static_cast<uint8_t>(state >> 24);
    }
    return image;
}

bool test_roundtrip(const std::filesystem::path& dir) {
    for (int channels = 1; channels <= 4; ++channels) {
        const datum::Image original = make_image(37, 23, channels);
        const auto file = dir / ("roundtrip_" + std::to_string(channels) + ".png");

        datum::save_png(file, original);
        const datum::Image reloaded = datum::load(file);

        CHECK(reloaded.width == original.width);
        CHECK(reloaded.height == original.height);
        CHECK(reloaded.channels == original.channels);
        CHECK(reloaded.pixels == original.pixels);
    }
    return true;
}

bool test_rejects_lossy_destination(const std::filesystem::path& dir) {
    const datum::Image image = make_image(8, 8, 3);
    for (const char* name : {"out.jpg", "out.JPEG", "out"}) {
        try {
            datum::save_png(dir / name, image);
            std::cerr << "FAILED: save_png accepted " << name << "\n";
            return false;
        } catch (const std::exception&) {
            // expected
        }
    }
    return true;
}

bool test_reports_missing_file(const std::filesystem::path& dir) {
    try {
        datum::load(dir / "does_not_exist.png");
        std::cerr << "FAILED: load accepted a missing file\n";
        return false;
    } catch (const std::exception&) {
        return true;
    }
}

}  // namespace

int main() {
    const auto dir = std::filesystem::temp_directory_path() / "datum_tests";
    std::filesystem::create_directories(dir);

    const bool passed = test_roundtrip(dir) && test_rejects_lossy_destination(dir) &&
                        test_reports_missing_file(dir);

    std::filesystem::remove_all(dir);
    std::cout << (passed ? "all checks passed\n" : "checks failed\n");
    return passed ? 0 : 1;
}
