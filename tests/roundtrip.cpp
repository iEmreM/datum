// The one check the whole project rests on: a payload embedded into an image and
// read back out is byte-identical, through a real PNG save/load in between.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

#include "datum/analyze.hpp"
#include "datum/bitstream.hpp"
#include "datum/codec.hpp"
#include "datum/container.hpp"
#include "datum/image.hpp"
#include "datum/io.hpp"
#include "datum/video.hpp"

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

/// Raw's bit depth: the top k bits of a channel carry payload and the bits below
/// are centred in the bucket those name. Both halves of that claim are checked —
/// the payload comes back, and it still comes back after every sample has drifted
/// by as much as the bucket allows. The margin is the entire reason k < 8 exists.
bool test_raw_bits(const std::filesystem::path& dir) {
    for (int k = 1; k <= 8; ++k) {
        for (int channels = 1; channels <= 4; ++channels) {
            // 64x64x1 at k = 1 holds 512 bytes; stay under that tightest case.
            const std::vector<uint8_t> payload = make_noise(400, 88u);
            datum::Image image = make_image(64, 64, channels);

            datum::embed_payload(image, datum::Mode::Raw, static_cast<uint8_t>(k), payload);

            const auto file = dir / "raw.png";
            datum::save_png(file, image);
            const datum::Image reloaded = datum::load(file);

            // Auto-detection must recover both the mode and k from the header.
            const datum::Extracted found = datum::extract_payload(reloaded, std::nullopt);
            CHECK(found.header.mode == datum::Mode::Raw);
            CHECK(found.header.param == static_cast<uint8_t>(k));
            CHECK(found.payload == payload);

            // A bucket is 2^(8-k) wide and the sample sits at its centre, so there
            // is room for half a bucket up and half a bucket down. Push every
            // sample — header included — to the very edge of that room, alternating
            // direction so no drift can cancel out, and it must still decode.
            const int room = (1 << (8 - k)) / 2;
            if (room == 0) {
                continue;  // k = 8: bucket of 1, no margin at all — that is the point
            }
            datum::Image drifted = reloaded;
            for (std::size_t i = 0; i < drifted.pixels.size(); ++i) {
                const int push = i % 2 == 0 ? room - 1 : -room;
                drifted.pixels[i] = static_cast<uint8_t>(drifted.pixels[i] + push);
            }
            CHECK(datum::extract_payload(drifted, std::nullopt).payload == payload);

            // One step past the edge has to break, or the margin above proves
            // nothing about where the boundary actually is.
            datum::Image over = reloaded;
            for (auto& sample : over.pixels) {
                sample = static_cast<uint8_t>(sample - room - 1);
            }
            try {
                datum::extract_payload(over, std::nullopt);
                std::cerr << "FAILED: raw k=" << k << " decoded past its bucket\n";
                return false;
            } catch (const std::exception&) {
                // expected
            }
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

/// A photo-like cover: smooth shading with a little grain. RS analysis measures how
/// neighbouring samples correlate, so the flat noise the other tests use would tell
/// it nothing at all — every block is already as noisy as it can get.
datum::Image make_photo(int width, int height, uint32_t seed = 0x9E37u) {
    datum::Image image;
    image.width = width;
    image.height = height;
    image.channels = 3;
    image.pixels.resize(image.sample_count());

    uint32_t state = seed;
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            for (int c = 0; c < 3; ++c) {
                state = state * 1664525u + 1013904223u;
                const double grain = static_cast<double>(state >> 24) / 255.0 * 6.0 - 3.0;
                const double value = 120.0 + 70.0 * std::sin(x / 21.0) + 50.0 * std::cos(y / 17.0) +
                                     18.0 * std::sin((x + y) / 9.0) + 12.0 * c + grain;
                image.pixels[(static_cast<std::size_t>(y) * width + x) * 3 + c] =
                    static_cast<uint8_t>(std::clamp(value, 0.0, 255.0));
            }
        }
    }

    // A camera's tone curve is not a straight line: neighbouring output values
    // collect unequal shares of the input range, and that is what makes a real
    // photograph's histogram jagged. A perfectly smooth synthetic histogram is the
    // one cover where a ±1 nudge is easy to spot, so this is not decoration.
    std::array<uint8_t, 256> curve{};
    double level = 0.0;
    for (int value = 0; value < 256; ++value) {
        curve[value] = static_cast<uint8_t>(std::clamp(level, 0.0, 255.0));
        state = state * 1664525u + 1013904223u;
        level += 0.4 + 1.2 * (static_cast<double>(state >> 24) / 255.0);
    }
    for (auto& sample : image.pixels) {
        sample = curve[sample];
    }
    return image;
}

/// LSB *replacement* over the first `samples` samples — what datum did before
/// Phase 4, kept here as the thing the analysis has to keep catching.
datum::Image replace_low_bits(datum::Image image, std::size_t samples, uint32_t seed) {
    uint32_t state = seed;
    for (std::size_t i = 0; i < samples && i < image.pixels.size(); ++i) {
        state = state * 1664525u + 1013904223u;
        image.pixels[i] = static_cast<uint8_t>((image.pixels[i] & 0xFEu) | ((state >> 24) & 1u));
    }
    return image;
}

/// Phase 4's goal, stated as a test: our own stego must sit inside the same noise
/// band as the clean cover, while LSB replacement at the same fill does not.
///
/// Both halves matter. Without the replacement case the first half only proves the
/// analysis is blind; without the stego case the analysis proves nothing about us.
bool test_steganalysis() {
    const datum::Image cover = make_photo(256, 256);
    const std::size_t tenth = cover.pixels.size() / 10;

    const datum::Analysis clean = datum::analyze(cover);
    CHECK(clean.chi.p_embedded < 0.5);
    CHECK(clean.rs.rate < 0.10);
    CHECK(clean.rs.regular - clean.rs.singular > 0.15);

    // Chi-square has to keep catching the replacement it was written for. At full
    // fill it is unmistakable — and so is RS, though not through its rate estimate:
    // that breaks down at p = 1, where the R and S curves cross and the quadratic
    // has no root left in range. What is unambiguous is that they met at all.
    const datum::Analysis full = datum::analyze(replace_low_bits(cover, cover.pixels.size(), 11u));
    CHECK(full.chi.p_embedded > 0.9);
    CHECK(std::abs(full.rs.regular - full.rs.singular) < 0.05);

    // Half filled is where the rate estimate is at its best.
    const datum::Analysis half =
        datum::analyze(replace_low_bits(cover, cover.pixels.size() / 2, 13u));
    CHECK(half.rs.rate > 0.30);

    // A tenth of the image is averaged into nothing by a whole-image histogram, so
    // catching it is entirely down to the prefix scan — assert that it also landed
    // near where the payload stops.
    const datum::Analysis part = datum::analyze(replace_low_bits(cover, tenth, 12u));
    CHECK(part.chi.p_embedded > 0.9);
    CHECK(part.chi.prefix < 0.30);

    // Phase 4's claim: the same fill through our own codec leaves RS with nothing.
    // Its estimate stays on the clean cover's baseline instead of tracking the
    // payload, and R and S never converge the way replacement makes them.
    //
    // Chi-square is deliberately not asserted against our output. It is defeated on
    // a real photograph — measured at p = 0.000 for every fill, against 1.000 for
    // replacement — but on a generated cover with a smoother histogram the ±1 nudge
    // is enough to even out the value pairs and it still fires. Asserting it would
    // pin down this fixture rather than the codec.
    datum::Image stego = cover;
    datum::embed_payload(stego, datum::Mode::Lsb, 1, make_noise(tenth / 8, 41u));
    CHECK(datum::analyze(stego).rs.rate < clean.rs.rate + 0.05);

    datum::Image filled = cover;
    datum::embed_payload(
        filled, datum::Mode::Lsb, 1, make_noise(cover.pixels.size() / 8 - datum::kHeaderSize, 42u));
    const datum::Analysis hidden = datum::analyze(filled);
    CHECK(hidden.rs.rate < 0.15);
    CHECK(hidden.rs.regular - hidden.rs.singular > 0.10);
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

// ---------------------------------------------------------------------------
// Video. These need ffmpeg, so they report a skip rather than a failure when it
// is missing — a contributor without ffmpeg should still be able to run the suite.
// ---------------------------------------------------------------------------

std::string tool(const char* variable, const char* fallback) {
    const char* value = std::getenv(variable);
    return value != nullptr && value[0] != '\0' ? std::string(value) : std::string(fallback);
}

#ifdef _WIN32
constexpr const char* kNullDevice = "NUL";
#else
constexpr const char* kNullDevice = "/dev/null";
#endif

/// Runs a command with stdout sent to `target` and stderr discarded, returning
/// true on exit status 0. Windows needs the outer quotes for the same reason the
/// video layer does: cmd.exe eats the outermost pair.
bool run(const std::string& command, const std::string& target) {
    const std::string redirected = command + " > " + target + " 2> " + kNullDevice;
#ifdef _WIN32
    return std::system(("\"" + redirected + "\"").c_str()) == 0;
#else
    return std::system(redirected.c_str()) == 0;
#endif
}

bool run_quiet(const std::string& command) {
    return run(command, kNullDevice);
}

bool have_ffmpeg() {
    return run_quiet("\"" + tool("DATUM_FFMPEG", "ffmpeg") + "\" -version") &&
           run_quiet("\"" + tool("DATUM_FFPROBE", "ffprobe") + "\" -version");
}

/// A short clip with an audio track. FFV1 and PCM need no external encoders, so
/// this works on any ffmpeg build; the point is our pipeline, not theirs.
bool make_clip(const std::filesystem::path& file, int width, int height, int frames) {
    const std::string size = std::to_string(width) + "x" + std::to_string(height);
    return run_quiet("\"" + tool("DATUM_FFMPEG", "ffmpeg") +
                     "\" -v error -y -f lavfi -i testsrc2=" + "size=" + size +
                     ":rate=30 -f lavfi -i sine=frequency=440 -frames:v " + std::to_string(frames) +
                     " -c:v ffv1 -pix_fmt bgr0 -c:a pcm_s16le -shortest " + "\"" + file.string() +
                     "\"");
}

bool has_audio_track(const std::filesystem::path& file, const std::filesystem::path& scratch) {
    const auto listing = scratch / "streams.txt";
    if (!run("\"" + tool("DATUM_FFPROBE", "ffprobe") +
                 "\" -v error -show_entries stream=codec_type -of csv=p=0 \"" + file.string() +
                 "\"",
             "\"" + listing.string() + "\"")) {
        return false;
    }
    const std::vector<uint8_t> bytes = datum::read_bytes(listing);
    return std::string(bytes.begin(), bytes.end()).find("audio") != std::string::npos;
}

bool test_video_roundtrip(const std::filesystem::path& dir) {
    if (!have_ffmpeg()) {
        std::cout << "  (skipped video: ffmpeg/ffprobe not found)\n";
        return true;
    }

    const int width = 640;
    const int height = 480;
    const int frames = 30;
    const auto cover = dir / "clip.mkv";
    CHECK(make_clip(cover, width, height, frames));

    const datum::VideoInfo info = datum::probe(cover);
    CHECK(info.width == width);
    CHECK(info.height == height);
    CHECK(info.frames == static_cast<std::size_t>(frames));
    CHECK(info.frame_bytes() == static_cast<std::size_t>(width) * height * 3);

    // 1 MB in lsb k=1 needs 10 of these frames, so the payload genuinely spans
    // frames rather than fitting in the first one.
    const std::size_t per_frame = static_cast<std::size_t>(width) * height * 3 / 8;
    const std::vector<uint8_t> payload = make_noise(1024 * 1024, 4242u);
    CHECK(payload.size() > per_frame);
    CHECK(datum::video_capacity(info, datum::Mode::Lsb, 1) > payload.size());

    const auto stego = dir / "stego.mkv";
    const datum::VideoStats stats = datum::embed_video(cover, stego, datum::Mode::Lsb, 1, payload);
    CHECK(stats.frames == static_cast<std::size_t>(frames));
    CHECK(stats.psnr > 50.0);

    // Auto-detection, then the CRC, then the bytes themselves.
    const datum::Extracted found = datum::extract_video(stego, std::nullopt);
    CHECK(found.header.mode == datum::Mode::Lsb);
    CHECK(found.header.param == 1);
    CHECK(found.payload == payload);

    // Every frame must survive, and so must the audio the user never asked us
    // to touch — dropping it would be silent data loss.
    const datum::VideoInfo after = datum::probe(stego);
    CHECK(after.frames == static_cast<std::size_t>(frames));
    CHECK(after.width == width);
    CHECK(after.height == height);
    CHECK(has_audio_track(stego, dir));

    // A clip with nothing hidden in it must not look like it has a payload.
    try {
        datum::extract_video(cover, std::nullopt);
        std::cerr << "FAILED: extract_video found a payload in an untouched clip\n";
        return false;
    } catch (const std::exception&) {
        // expected
    }

    // Lossy output would quantise the payload away, so it is refused outright.
    try {
        datum::embed_video(cover, dir / "no.mp4", datum::Mode::Lsb, 1, payload);
        std::cerr << "FAILED: embed_video accepted a lossy destination\n";
        return false;
    } catch (const std::exception&) {
        // expected
    }

    // Oversized must fail before any encoding starts, not truncate.
    try {
        datum::embed_video(
            cover, dir / "no.mkv", datum::Mode::Lsb, 1, make_noise(64u * 1024 * 1024, 1u));
        std::cerr << "FAILED: embed_video accepted an oversized payload\n";
        return false;
    } catch (const std::exception&) {
        // expected
    }
    return true;
}

}  // namespace

int main() {
    const auto dir = std::filesystem::temp_directory_path() / "datum_tests";
    std::filesystem::create_directories(dir);

    const bool passed =
        test_image_roundtrip(dir) && test_rejects_lossy_destination(dir) &&
        test_reports_missing_file(dir) && test_crc32() && test_header() && test_bitstream() &&
        test_payload_roundtrip(dir) && test_lsb_roundtrip(dir) && test_raw_bits(dir) &&
        test_lsb_is_quiet() && test_steganalysis() && test_alpha_is_untouched() &&
        test_rejects_oversized_payload() && test_detects_corruption() && test_video_roundtrip(dir);

    std::filesystem::remove_all(dir);
    std::cout << (passed ? "all checks passed\n" : "checks failed\n");
    return passed ? 0 : 1;
}
