// The Phase 5 robustness harness: embed -> re-encode -> extract -> BER, swept over
// the quantiser step and over the encoders a stego image realistically meets.
//
// Deliberately not a CTest test. It needs ffmpeg, takes a couple of minutes, and its
// output is a table for docs/QIM.md rather than a pass or a fail. The two facts worth
// pinning down are asserted in tests/roundtrip.cpp; this is where their numbers came
// from, and Phase 6 extends the same loop with VP9 and frame-rate conversion.
//
//   datum_robustness [-i cover.png] [--fill 1.0]
//
// Without -i the cover is ffmpeg's own `testsrc2` pattern, so the harness needs no
// media in the repository. Point -i at a real photograph for a number that means
// something about photographs.

#include <stb_image_write.h>  // declarations only — datum_core carries the implementation

#include <array>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

#include "datum/bitstream.hpp"
#include "datum/codec.hpp"
#include "datum/container.hpp"
#include "datum/image.hpp"

namespace {

/// One way the world damages a stego image. A non-zero `jpeg_quality` is written in
/// process by stb; otherwise `ffmpeg` holds the encoder flags, applied at the
/// original resolution unless a filter in them says otherwise.
struct Recipe {
    const char* label;
    int jpeg_quality;
    const char* ffmpeg;
};

constexpr std::array<Recipe, 8> kRecipes = {{
    // stb subsamples chroma below quality 90, so 95 is 4:4:4 and 85/75 are 4:2:0 —
    // which is the split that decides whether a spatial mode lives or dies.
    {"jpeg 95", 95, ""},
    {"jpeg 85", 85, ""},
    {"jpeg 75", 75, ""},
    {"h264 crf18", 0, "-c:v libx264 -crf 18 -pix_fmt yuv420p"},
    {"h264 crf23", 0, "-c:v libx264 -crf 23 -pix_fmt yuv420p"},
    {"h264 crf28", 0, "-c:v libx264 -crf 28 -pix_fmt yuv420p"},
    // Same quality with the chroma left alone: the control that says whether a
    // failure above is the quantiser or the subsampling.
    {"h264 444 crf23", 0, "-c:v libx264 -crf 23 -pix_fmt yuv444p"},
    // A lossless codec, so the only thing this row measures is the resampling — the
    // documented limit of every spatial mode.
    {"half scale", 0, "-c:v ffv1 -vf scale=iw/2:ih/2,scale=iw*2:ih*2"},
}};

constexpr std::array<int, 11> kDeltas = {2, 4, 6, 8, 12, 16, 20, 24, 26, 28, 32};

constexpr const char* kGeneratedCover = "testsrc2=size=640x480:rate=1";

std::string tool(const char* variable, const char* fallback) {
    const char* value = std::getenv(variable);
    return value != nullptr && value[0] != '\0' ? std::string(value) : std::string(fallback);
}

std::string quote(const std::filesystem::path& path) {
    return "\"" + path.string() + "\"";
}

/// Runs a command with both streams discarded. Windows needs the outer quotes for
/// the same reason the video layer does: cmd.exe eats the outermost pair.
bool run(const std::string& command) {
#ifdef _WIN32
    return std::system(("\"" + command + " > NUL 2> NUL\"").c_str()) == 0;
#else
    return std::system((command + " > /dev/null 2> /dev/null").c_str()) == 0;
#endif
}

std::string ffmpeg() {
    return "\"" + tool("DATUM_FFMPEG", "ffmpeg") + "\"";
}

/// Deterministic payload bytes, so two runs of the harness are comparable.
std::vector<uint8_t> noise(std::size_t count) {
    std::vector<uint8_t> bytes(count);
    uint32_t state = 0xC0FFEEu;
    for (auto& byte : bytes) {
        state = state * 1664525u + 1013904223u;
        byte = static_cast<uint8_t>(state >> 24);
    }
    return bytes;
}

datum::Image generate_cover(const std::filesystem::path& file) {
    if (!run(ffmpeg() + " -v error -y -f lavfi -i " + kGeneratedCover + " -frames:v 1 " +
             quote(file))) {
        throw std::runtime_error("cannot generate a cover (is ffmpeg on PATH?)");
    }
    return datum::load(file);
}

/// Puts a stego image through one recipe and hands back what came out the other
/// side, at the original resolution and channel count so the two stay comparable
/// sample by sample.
datum::Image damage(const datum::Image& stego,
                    const std::filesystem::path& stego_file,
                    const Recipe& recipe,
                    const std::filesystem::path& dir) {
    if (recipe.jpeg_quality != 0) {
        const auto file = dir / "damaged.jpg";
        if (stbi_write_jpg(file.string().c_str(),
                           stego.width,
                           stego.height,
                           stego.channels,
                           stego.pixels.data(),
                           recipe.jpeg_quality) == 0) {
            throw std::runtime_error("cannot write " + file.string());
        }
        return datum::load(file);
    }

    const auto encoded = dir / "damaged.mkv";
    const auto file = dir / "damaged.png";
    if (!run(ffmpeg() + " -v error -y -i " + quote(stego_file) + " " + recipe.ffmpeg +
             " -frames:v 1 " + quote(encoded)) ||
        !run(ffmpeg() + " -v error -y -i " + quote(encoded) + " -frames:v 1 " + quote(file))) {
        throw std::runtime_error(std::string("ffmpeg failed on recipe ") + recipe.label);
    }
    return datum::load(file);
}

int measure(const std::filesystem::path& cover_file, double fill) {
    const auto dir = std::filesystem::temp_directory_path() / "datum_robustness";
    std::filesystem::create_directories(dir);

    const datum::Image cover =
        cover_file.empty() ? generate_cover(dir / "cover.png") : datum::load(cover_file);
    if (cover.channels != 3) {
        throw std::runtime_error(
            "cover must be 3-channel RGB: the others do not survive JPEG and H.264 unchanged");
    }
    if (cover.width % 2 != 0 || cover.height % 2 != 0) {
        throw std::runtime_error("cover dimensions must be even: yuv420p has no half pixels");
    }

    // Capacity is one bit per pixel whatever the step is, so the payload is sized
    // once and every row below embeds exactly the same bytes.
    const std::size_t capacity = datum::make_codec(datum::Mode::Qim, 2)->capacity(cover);
    const auto payload =
        noise(static_cast<std::size_t>(static_cast<double>(capacity - datum::kHeaderSize) * fill));

    std::cout << "cover " << cover.width << "x" << cover.height << ", qim capacity " << capacity
              << " bytes, payload " << payload.size() << " bytes ("
              << static_cast<int>(fill * 100.0) << "% fill)\n\n"
              << "| delta | PSNR | SSIM |";
    for (const Recipe& recipe : kRecipes) {
        std::cout << " " << recipe.label << " |";
    }
    std::cout << "\n|---:|---:|---:|";
    for (std::size_t i = 0; i < kRecipes.size(); ++i) {
        std::cout << "---:|";
    }
    std::cout << "\n";

    const auto stego_file = dir / "stego.png";
    for (const int delta : kDeltas) {
        const auto codec = datum::make_codec(datum::Mode::Qim, static_cast<uint8_t>(delta));

        datum::Header header;
        header.mode = datum::Mode::Qim;
        header.param = static_cast<uint8_t>(delta);
        header.payload_len = static_cast<uint32_t>(payload.size());
        header.payload_crc = datum::crc32(payload);
        std::vector<uint8_t> stream = datum::serialize(header);
        stream.insert(stream.end(), payload.begin(), payload.end());

        datum::Image stego = cover;
        datum::BitReader source(stream);
        codec->embed(stego, source);
        datum::save_png(stego_file, stego);

        std::cout << "| " << delta << " | " << std::fixed << std::setprecision(1)
                  << datum::psnr(cover, stego) << " | " << std::setprecision(3)
                  << datum::ssim(cover, stego) << " |" << std::flush;

        for (const Recipe& recipe : kRecipes) {
            const datum::Image back = damage(stego, stego_file, recipe, dir);
            datum::BitWriter sink;
            codec->extract(back, sink, stream.size() * 8);
            const double ber = datum::bit_error_rate(sink.bytes(), stream);

            // The CRC is the only signal that matters in the end: a payload either
            // came back byte for byte or it did not.
            bool intact = true;
            try {
                datum::extract_payload(back, datum::Mode::Qim);
            } catch (const std::exception&) {
                intact = false;
            }
            const char* mark = intact ? "**" : "";
            std::cout << " " << mark << std::setprecision(4) << ber << mark << " |" << std::flush;
        }
        std::cout << "\n";
    }

    std::filesystem::remove_all(dir);
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    try {
        std::filesystem::path cover;
        double fill = 1.0;
        for (int i = 1; i < argc; i += 2) {
            const std::string flag = argv[i];
            if (i + 1 >= argc) {
                throw std::runtime_error("missing value for " + flag);
            }
            if (flag == "-i") {
                cover = argv[i + 1];
            } else if (flag == "--fill") {
                fill = std::stod(argv[i + 1]);
            } else {
                throw std::runtime_error("unknown flag " + flag + " (accepts: -i --fill)");
            }
        }
        return measure(cover, fill);
    } catch (const std::exception& error) {
        std::cerr << "error: " << error.what() << "\n";
        return 1;
    }
}
