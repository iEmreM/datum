#include <cmath>
#include <exception>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "datum/codec.hpp"
#include "datum/container.hpp"
#include "datum/image.hpp"
#include "datum/io.hpp"
#include "datum/video.hpp"

namespace {

/// Flag name (leading dashes stripped) -> value.
using Flags = std::map<std::string, std::string, std::less<>>;

void print_usage() {
    std::cerr << "datum — embed data inside pixel values\n"
                 "\n"
                 "usage:\n"
                 "  datum info     -i <carrier>\n"
                 "  datum capacity -i <carrier> [--mode binary|raw|lsb] [--bits 1..4]\n"
                 "  datum embed    -i <cover> -o <stego> -d <payload>"
                 " [--mode binary|raw|lsb] [--bits 1..4]\n"
                 "  datum extract  -i <stego> -o <payload> [--mode binary|raw|lsb]\n"
                 "\n"
                 "notes:\n"
                 "  a carrier is an image or a video; the extension picks which\n"
                 "  --bits sets the low bits per channel used by lsb (default 1)\n"
                 "  output must be .png for images and .mkv for video — lossy\n"
                 "  formats would erase the payload\n"
                 "  extract auto-detects the mode and bit depth unless --mode is given\n"
                 "  video needs ffmpeg and ffprobe on PATH (or DATUM_FFMPEG /\n"
                 "  DATUM_FFPROBE pointing at them)\n";
}

Flags parse_flags(int argc, char** argv, int first) {
    Flags flags;
    for (int i = first; i < argc; ++i) {
        const std::string token = argv[i];
        if (token.rfind('-', 0) != 0) {
            throw std::runtime_error("unexpected argument: " + token);
        }
        const std::string name = token.substr(token.rfind("--", 0) == 0 ? 2 : 1);
        if (i + 1 >= argc) {
            throw std::runtime_error("missing value for -" + name);
        }
        flags[name] = argv[++i];
    }
    return flags;
}

const std::string& require(const Flags& flags, const std::string& name) {
    const auto found = flags.find(name);
    if (found == flags.end()) {
        throw std::runtime_error("missing required flag -" + name);
    }
    return found->second;
}

const std::string* find_flag(const Flags& flags, const std::string& name) {
    const auto found = flags.find(name);
    return found == flags.end() ? nullptr : &found->second;
}

datum::Mode require_mode(const Flags& flags, datum::Mode fallback) {
    const std::string* name = find_flag(flags, "mode");
    if (name == nullptr) {
        return fallback;
    }
    const auto mode = datum::mode_from_name(*name);
    if (!mode) {
        throw std::runtime_error("unknown mode: " + *name);
    }
    return *mode;
}

/// The per-mode parameter carried in the header. Only lsb uses it (k, the number
/// of low bits per channel); other modes reject --bits so a typo is not ignored.
uint8_t require_param(const Flags& flags, datum::Mode mode) {
    const std::string* bits = find_flag(flags, "bits");
    if (mode != datum::Mode::Lsb) {
        if (bits != nullptr) {
            throw std::runtime_error("--bits only applies to --mode lsb");
        }
        return 0;
    }
    const int k = bits == nullptr ? 1 : std::stoi(*bits);
    if (k < 1 || k > 4) {
        throw std::runtime_error("--bits must be between 1 and 4");
    }
    return static_cast<uint8_t>(k);
}

// ponytail: argv is ANSI on Windows, so non-ASCII paths mangle here even though the
// image and io layers are wide-path clean. Switch to wmain/GetCommandLineW if it bites.

void print_distortion(double db) {
    if (std::isinf(db)) {
        std::cout << " (unchanged)";
    } else {
        std::cout << ", PSNR " << std::fixed << std::setprecision(2) << db << " dB";
    }
}

void print_capacity(std::string_view label, std::size_t payload) {
    std::cout << label << ": " << payload << " payload bytes (" << payload / 1024 << " KiB), "
              << datum::kHeaderSize << " byte header\n";
}

int run_info(const Flags& flags) {
    const std::filesystem::path input = require(flags, "i");
    if (datum::is_video(input)) {
        const datum::VideoInfo info = datum::probe(input);
        std::cout << info.width << "x" << info.height << ", " << info.frames << " frames @ "
                  << info.frame_rate << " fps\n"
                  << info.frame_bytes() << " bytes per frame (rgb24)\n";
        return 0;
    }

    const datum::Image image = datum::load(input);
    std::cout << image.width << "x" << image.height << ", " << image.channels << " channel(s), "
              << image.color_channels() << " usable\n"
              << image.pixel_count() << " pixels, " << image.sample_count() << " channel bytes\n";
    return 0;
}

int run_capacity(const Flags& flags) {
    const std::filesystem::path input = require(flags, "i");
    const datum::Mode mode = require_mode(flags, datum::Mode::Raw);
    const uint8_t param = require_param(flags, mode);

    if (datum::is_video(input)) {
        const datum::VideoInfo info = datum::probe(input);
        print_capacity(datum::mode_name(mode), datum::video_capacity(info, mode, param));
        return 0;
    }

    const datum::Image image = datum::load(input);
    const std::size_t total = datum::make_codec(mode, param)->capacity(image);
    print_capacity(datum::mode_name(mode),
                   total > datum::kHeaderSize ? total - datum::kHeaderSize : 0);
    return 0;
}

int run_embed(const Flags& flags) {
    const std::filesystem::path input = require(flags, "i");
    const std::filesystem::path output = require(flags, "o");
    const std::vector<uint8_t> payload = datum::read_bytes(require(flags, "d"));
    const datum::Mode mode = require_mode(flags, datum::Mode::Raw);
    const uint8_t param = require_param(flags, mode);

    if (datum::is_video(input)) {
        const datum::VideoStats stats = datum::embed_video(input, output, mode, param, payload);
        std::cout << "embedded " << payload.size() << " bytes in " << datum::mode_name(mode)
                  << " mode across " << stats.frames << " frames";
        print_distortion(stats.psnr);
        std::cout << "\n";
        return 0;
    }

    datum::Image image = datum::load(input);
    const datum::Image cover = image;  // kept only to measure the distortion below
    datum::embed_payload(image, mode, param, payload);
    datum::save_png(output, image);

    std::cout << "embedded " << payload.size() << " bytes in " << datum::mode_name(mode) << " mode";
    print_distortion(datum::psnr(cover, image));
    std::cout << "\n";
    return 0;
}

int run_extract(const Flags& flags) {
    const std::filesystem::path input = require(flags, "i");
    std::optional<datum::Mode> mode;
    if (find_flag(flags, "mode") != nullptr) {
        mode = require_mode(flags, datum::Mode::Raw);
    }

    const datum::Extracted result = datum::is_video(input)
                                        ? datum::extract_video(input, mode)
                                        : datum::extract_payload(datum::load(input), mode);
    datum::write_bytes(require(flags, "o"), result.payload);

    std::cout << "extracted " << result.payload.size() << " bytes from "
              << datum::mode_name(result.header.mode) << " mode, CRC ok\n";
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        print_usage();
        return 2;
    }

    const std::string command = argv[1];
    if (command == "-h" || command == "--help" || command == "help") {
        print_usage();
        return 0;
    }

    try {
        const Flags flags = parse_flags(argc, argv, 2);
        if (command == "info") {
            return run_info(flags);
        }
        if (command == "capacity") {
            return run_capacity(flags);
        }
        if (command == "embed") {
            return run_embed(flags);
        }
        if (command == "extract") {
            return run_extract(flags);
        }
        std::cerr << "error: unknown command: " << command << "\n\n";
        print_usage();
        return 2;
    } catch (const std::exception& error) {
        // No usage dump here: by this point the command parsed fine and the
        // failure is operational, so the message is the useful part.
        std::cerr << "error: " << error.what() << "\n";
        return 1;
    }
}
