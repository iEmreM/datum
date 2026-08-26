// The one check the whole project rests on: a payload embedded into an image and
// read back out is byte-identical, through a real PNG save/load in between.

#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

#include "datum/bitstream.hpp"
#include "datum/codec.hpp"
#include "datum/container.hpp"
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
std::vector<uint8_t> make_noise(std::size_t count, uint32_t seed) {
    std::vector<uint8_t> bytes(count);
    uint32_t state = seed;
    for (auto& byte : bytes) {
        state = state * 1664525u + 1013904223u;
        byte = static_cast<uint8_t>(state >> 24);
    }
    return bytes;
}

datum::Image make_image(int width, int height, int channels, uint32_t seed = 0x1234567u) {
    datum::Image image;
    image.width = width;
    image.height = height;
    image.channels = channels;
    image.pixels = make_noise(image.sample_count(), seed);
    return image;
}

bool test_image_roundtrip(const std::filesystem::path& dir) {
    for (int channels = 1; channels <= 4; ++channels) {
        const datum::Image original = make_image(37, 23, channels);
        const auto file = dir / ("image_" + std::to_string(channels) + ".png");

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

bool test_crc32() {
    // The check value every CRC-32/ISO-HDLC implementation agrees on.
    const std::string check = "123456789";
    const auto* bytes = reinterpret_cast<const uint8_t*>(check.data());
    CHECK(datum::crc32({bytes, check.size()}) == 0xCBF43926u);
    CHECK(datum::crc32({}) == 0u);
    return true;
}

bool test_header() {
    datum::Header header;
    header.mode = datum::Mode::Raw;
    header.param = 3;
    header.payload_len = 123456;
    header.payload_crc = 0xDEADBEEFu;

    const std::vector<uint8_t> bytes = datum::serialize(header);
    CHECK(bytes.size() == datum::kHeaderSize);

    const auto parsed = datum::parse_header(bytes);
    CHECK(parsed.has_value());
    CHECK(parsed->mode == header.mode);
    CHECK(parsed->param == header.param);
    CHECK(parsed->payload_len == header.payload_len);
    CHECK(parsed->payload_crc == header.payload_crc);

    // Anything that is not a header must read as "no payload", not as garbage.
    CHECK(!datum::parse_header(make_noise(datum::kHeaderSize, 99u)).has_value());
    CHECK(!datum::parse_header({bytes.data(), datum::kHeaderSize - 1}).has_value());

    std::vector<uint8_t> wrong_magic = bytes;
    wrong_magic[0] = 'X';
    CHECK(!datum::parse_header(wrong_magic).has_value());
    return true;
}

bool test_bitstream() {
    const std::vector<uint8_t> source = make_noise(257, 7u);
    datum::BitReader reader(source);
    datum::BitWriter writer;

    CHECK(reader.remaining() == source.size() * 8);
    while (!reader.exhausted()) {
        writer.write(reader.read());
    }
    CHECK(writer.size() == source.size() * 8);
    CHECK(writer.bytes() == source);

    // Reading past the end must be loud, not silently zero.
    try {
        reader.read();
        std::cerr << "FAILED: BitReader read past the end\n";
        return false;
    } catch (const std::exception&) {
        return true;
    }
}

bool test_payload_roundtrip(const std::filesystem::path& dir) {
    struct Case {
        datum::Mode mode;
        std::size_t payload_size;
    };

    const Case cases[] = {
        {datum::Mode::Raw, 4000},
        {datum::Mode::Raw, 1},
        {datum::Mode::Raw, 0},
        {datum::Mode::Binary, 400},
        {datum::Mode::Binary, 1},
    };

    for (const auto& test : cases) {
        for (int channels = 1; channels <= 4; ++channels) {
            const std::vector<uint8_t> payload = make_noise(test.payload_size, 42u);
            datum::Image image = make_image(120, 90, channels);

            datum::embed_payload(image, test.mode, 0, payload);

            // Through a real PNG on disk, not just in memory.
            const auto file = dir / "stego.png";
            datum::save_png(file, image);
            const datum::Image reloaded = datum::load(file);

            // Auto-detection must land on the mode that was used.
            const datum::Extracted found = datum::extract_payload(reloaded, std::nullopt);
            CHECK(found.header.mode == test.mode);
            CHECK(found.payload == payload);

            // Forcing the correct mode must agree with auto-detection.
            const datum::Extracted forced = datum::extract_payload(reloaded, test.mode);
            CHECK(forced.payload == payload);
        }
    }
    return true;
}

bool test_lsb_roundtrip(const std::filesystem::path& dir) {
    // k = 3 is the interesting one: 8·bytes is not a multiple of 3, so the final
    // bit group is partial and its alignment is exercised.
    for (int k = 1; k <= 4; ++k) {
        for (int channels = 1; channels <= 4; ++channels) {
            // 100×100×1 at k = 1 holds 1250 bytes; stay under that tightest case.
            const std::vector<uint8_t> payload = make_noise(1000, 77u);
            datum::Image image = make_image(100, 100, channels);

            datum::embed_payload(image, datum::Mode::Lsb, static_cast<uint8_t>(k), payload);

            const auto file = dir / "lsb.png";
            datum::save_png(file, image);
            const datum::Image reloaded = datum::load(file);

            // Auto-detection must recover both the mode and k from the header.
            const datum::Extracted found = datum::extract_payload(reloaded, std::nullopt);
            CHECK(found.header.mode == datum::Mode::Lsb);
            CHECK(found.header.param == static_cast<uint8_t>(k));
            CHECK(found.payload == payload);

            // Forcing lsb (without knowing k) must still work — k comes from the header.
            const datum::Extracted forced = datum::extract_payload(reloaded, datum::Mode::Lsb);
            CHECK(forced.payload == payload);
        }
    }
    return true;
}

bool test_lsb_is_quiet() {
    // k = 1 must be near-invisible: each touched channel moves by at most 1, so a
    // well-filled image still sits above 50 dB — and below 60, proving it changed.
    const datum::Image cover = make_image(256, 256, 3);
    datum::Image stego = cover;
    datum::embed_payload(stego, datum::Mode::Lsb, 1, make_noise(20000, 3u));

    const double db = datum::psnr(cover, stego);
    CHECK(db > 50.0);
    CHECK(db < 60.0);

    // An image compared with itself is infinitely close.
    CHECK(std::isinf(datum::psnr(cover, cover)));
    return true;
}

bool test_alpha_is_untouched() {
    const int channels = 4;
    const datum::Image cover = make_image(64, 64, channels);
    datum::Image image = cover;
    datum::embed_payload(image, datum::Mode::Raw, 0, make_noise(2000, 5u));

    for (std::size_t pixel = 0; pixel < image.pixel_count(); ++pixel) {
        const std::size_t alpha = pixel * static_cast<std::size_t>(channels) + 3;
        CHECK(image.pixels[alpha] == cover.pixels[alpha]);
    }
    return true;
}

bool test_rejects_oversized_payload() {
    datum::Image image = make_image(16, 16, 3);
    try {
        // Must fail loudly rather than truncate: a silently short payload is worse
        // than no payload at all.
        datum::embed_payload(image, datum::Mode::Raw, 0, make_noise(100000, 1u));
        std::cerr << "FAILED: embed accepted an oversized payload\n";
        return false;
    } catch (const std::exception&) {
        return true;
    }
}

bool test_detects_corruption() {
    const std::vector<uint8_t> payload = make_noise(2000, 11u);
    datum::Image image = make_image(120, 90, 3);
    datum::embed_payload(image, datum::Mode::Raw, 0, payload);

    // Flip one bit inside the payload — past the 16-byte header, before the end of
    // the 2000 bytes. The CRC is the only thing standing between a corrupted
    // extraction and a silently wrong file.
    image.pixels[1000] ^= 0x01;
    try {
        datum::extract_payload(image, std::nullopt);
        std::cerr << "FAILED: extract accepted a corrupted payload\n";
        return false;
    } catch (const std::exception&) {
        // expected
    }

    const datum::Image clean = make_image(120, 90, 3, 0xABCDEFu);
    try {
        datum::extract_payload(clean, std::nullopt);
        std::cerr << "FAILED: extract found a payload in an untouched image\n";
        return false;
    } catch (const std::exception&) {
        return true;
    }
}

}  // namespace

int main() {
    const auto dir = std::filesystem::temp_directory_path() / "datum_tests";
    std::filesystem::create_directories(dir);

    const bool passed =
        test_image_roundtrip(dir) && test_rejects_lossy_destination(dir) &&
        test_reports_missing_file(dir) && test_crc32() && test_header() && test_bitstream() &&
        test_payload_roundtrip(dir) && test_lsb_roundtrip(dir) && test_lsb_is_quiet() &&
        test_alpha_is_untouched() && test_rejects_oversized_payload() && test_detects_corruption();

    std::filesystem::remove_all(dir);
    std::cout << (passed ? "all checks passed\n" : "checks failed\n");
    return passed ? 0 : 1;
}
