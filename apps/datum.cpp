#include <cmath>
#include <exception>
#include <iomanip>
#include <iostream>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "datum/codec.hpp"
#include "datum/container.hpp"
#include "datum/image.hpp"
#include "datum/io.hpp"

namespace {

/// Flag name (leading dashes stripped) -> value.
using Flags = std::map<std::string, std::string, std::less<>>;

void print_usage() {
    std::cerr << "datum — embed data inside pixel values\n"
                 "\n"
                 "usage:\n"
                 "  datum info     -i <image>\n"
                 "  datum capacity -i <image> [--mode binary|raw|lsb] [--bits 1..4]\n"
                 "  datum embed    -i <cover> -o <stego.png> -d <payload>"
                 " [--mode binary|raw|lsb] [--bits 1..4]\n"
                 "  datum extract  -i <stego.png> -o <payload> [--mode binary|raw|lsb]\n"
                 "\n"
                 "notes:\n"
                 "  --bits sets the low bits per channel used by lsb (default 1)\n"
                 "  output must be .png — lossy formats would erase the payload\n"
                 "  extract auto-detects the mode and bit depth unless --mode is given\n";
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

int run_info(const Flags& flags) {
    const datum::Image image = datum::load(require(flags, "i"));
    std::cout << image.width << "x" << image.height << ", " << image.channels << " channel(s), "
              << image.color_channels() << " usable\n"
              << image.pixel_count() << " pixels, " << image.sample_count() << " channel bytes\n";
    return 0;
}

int run_capacity(const Flags& flags) {
    const datum::Image image = datum::load(require(flags, "i"));
    const datum::Mode mode = require_mode(flags, datum::Mode::Raw);
    const uint8_t param = require_param(flags, mode);
    const auto codec = datum::make_codec(mode, param);

    const std::size_t total = codec->capacity(image);
    const std::size_t payload = total > datum::kHeaderSize ? total - datum::kHeaderSize : 0;
    std::cout << datum::mode_name(mode) << ": " << payload << " payload bytes (" << payload / 1024
              << " KiB), " << datum::kHeaderSize << " byte header\n";
    return 0;
}

int run_embed(const Flags& flags) {
    datum::Image image = datum::load(require(flags, "i"));
    const datum::Image cover = image;  // kept only to measure the distortion below
    const std::vector<uint8_t> payload = datum::read_bytes(require(flags, "d"));
    const datum::Mode mode = require_mode(flags, datum::Mode::Raw);
    const uint8_t param = require_param(flags, mode);

    datum::embed_payload(image, mode, param, payload);
    datum::save_png(require(flags, "o"), image);

    std::cout << "embedded " << payload.size() << " bytes in " << datum::mode_name(mode) << " mode";
    const double db = datum::psnr(cover, image);
    if (std::isinf(db)) {
        std::cout << " (image unchanged)";
    } else {
        std::cout << ", PSNR " << std::fixed << std::setprecision(2) << db << " dB";
    }
    std::cout << "\n";
    return 0;
}

int run_extract(const Flags& flags) {
    const datum::Image image = datum::load(require(flags, "i"));
    std::optional<datum::Mode> mode;
    if (find_flag(flags, "mode") != nullptr) {
        mode = require_mode(flags, datum::Mode::Raw);
    }

    const datum::Extracted result = datum::extract_payload(image, mode);
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
