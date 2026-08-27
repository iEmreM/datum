#include <algorithm>
#include <cmath>
#include <exception>
#include <filesystem>
#include <initializer_list>
#include <iomanip>
#include <iostream>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "datum/analyze.hpp"
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
                 "  datum capacity -i <carrier> [--mode M] [--bits N] [--repeat N]\n"
                 "  datum embed    -i <cover> -o <stego> -d <payload> [--mode M]\n"
                 "                 [--bits N] [--delta N] [--margin N] [--repeat N]\n"
                 "  datum extract  -i <stego> -o <payload> [--mode M]\n"
                 "  datum analyze  -i <carrier>\n"
                 "\n"
                 "notes:\n"
                 "  M is binary, raw, lsb, qim or dct\n"
                 "  a carrier is an image or a video; the extension picks which\n"
                 "  --bits sets the payload bits per colour channel: lsb uses the\n"
                 "  low 1..4 (default 1), raw the top 1..8 (default 8) and centres\n"
                 "  what is left, which leaves raw a +/-2^(7-N) drift margin\n"
                 "  --delta sets qim's quantiser step, 2..32 (default 28): survives\n"
                 "  a re-encode at the same size, at the cost of banding\n"
                 "  --margin sets dct's coefficient separation, 2..64 (default\n"
                 "  32): the only mode that survives a re-encode *and* a rescale,\n"
                 "  at the cost of a little texture inside each 8x8 block\n"
                 "  --repeat N writes the whole payload into each of the first N\n"
                 "  frames of a video and majority-votes them back, so a damaged or\n"
                 "  dropped frame costs a vote instead of the payload (video only)\n"
                 "  output must be .png for images and .mkv for video — lossy\n"
                 "  formats would erase the payload\n"
                 "  extract auto-detects the mode and its parameter unless --mode\n"
                 "  is given; --repeat is never needed, it is in the header\n"
                 "  analyze runs steganalysis on a carrier — point it at our own\n"
                 "  output to check the hiding actually holds up\n"
                 "  video needs ffmpeg and ffprobe on PATH (or DATUM_FFMPEG /\n"
                 "  DATUM_FFPROBE pointing at them)\n";
}

/// Parses `-name value` pairs, rejecting any flag this command does not accept.
///
/// Silently ignoring an unknown flag is the worst thing this parser could do:
/// `--bit 4` instead of `--bits 4` would embed at the default depth and report
/// success, so you would only find out when the numbers made no sense.
Flags parse_flags(int argc,
                  char** argv,
                  int first,
                  const std::string& command,
                  std::initializer_list<std::string_view> accepted) {
    Flags flags;
    for (int i = first; i < argc; ++i) {
        const std::string token = argv[i];
        if (token.rfind('-', 0) != 0) {
            throw std::runtime_error("unexpected argument: " + token);
        }
        const std::string name = token.substr(token.rfind("--", 0) == 0 ? 2 : 1);
        if (std::find(accepted.begin(), accepted.end(), name) == accepted.end()) {
            std::string known;
            for (const std::string_view flag : accepted) {
                known += (known.empty() ? "" : " ") + std::string(flag.size() == 1 ? "-" : "--") +
                         std::string(flag);
            }
            throw std::runtime_error("unknown flag " + token + " for '" + command +
                                     "' (accepts: " + known + ")");
        }
        if (i + 1 >= argc) {
            throw std::runtime_error("missing value for " + token);
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

/// The flag that sets a mode's header parameter, its range and its default.
///
/// `lsb` and `raw` take --bits, the payload bits per colour channel: lsb's are the
/// *low* bits of each channel, raw's the *top* ones. `qim` takes --delta, the
/// quantiser step whose bin parity carries the bit. `dct` takes --margin, how far
/// apart it pushes the two coefficients whose order carries the bit. A mode with no
/// rule takes none of them, so a flag aimed at the wrong mode is refused rather
/// than ignored.
///
/// Each default is that mode's quietest useful setting: lsb 1, raw 8 (one payload
/// byte per channel), qim the step docs/QIM.md measured, dct the separation
/// docs/DCT.md measured.
struct ParamRule {
    std::string_view flag;
    int least = 0;
    int most = 0;
    int fallback = 0;
};

ParamRule param_rule(datum::Mode mode) {
    switch (mode) {
        case datum::Mode::Lsb:
            return {"bits", 1, 4, 1};
        case datum::Mode::Raw:
            return {"bits", 1, 8, 8};
        case datum::Mode::Qim:
            return {"delta", datum::kLeastDelta, datum::kMostDelta, datum::kDefaultDelta};
        case datum::Mode::Dct:
            return {"margin", datum::kLeastMargin, datum::kMostMargin, datum::kDefaultMargin};
        case datum::Mode::Binary:
            break;
    }
    return {};
}

uint8_t require_param(const Flags& flags, datum::Mode mode) {
    const ParamRule rule = param_rule(mode);
    for (const std::string_view name : {"bits", "delta", "margin"}) {
        if (name != rule.flag && find_flag(flags, std::string(name)) != nullptr) {
            throw std::runtime_error("--" + std::string(name) + " does not apply to --mode " +
                                     std::string(datum::mode_name(mode)));
        }
    }
    if (rule.flag.empty()) {
        return 0;
    }

    const std::string* given = find_flag(flags, std::string(rule.flag));
    const int value = given == nullptr ? rule.fallback : std::stoi(*given);
    if (value < rule.least || value > rule.most) {
        throw std::runtime_error("--" + std::string(rule.flag) + " must be between " +
                                 std::to_string(rule.least) + " and " + std::to_string(rule.most) +
                                 " for " + std::string(datum::mode_name(mode)) + " mode");
    }
    return static_cast<uint8_t>(value);
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

void print_capacity(datum::Mode mode, std::size_t payload) {
    std::cout << datum::mode_name(mode) << ": " << payload << " payload bytes (" << payload / 1024
              << " KiB), " << datum::header_block_size(mode) << " byte header"
              << (datum::uses_ecc(mode) ? ", Reed-Solomon parity on top" : "") << "\n";
}

/// Video-only, and refused rather than ignored on a still: one image is one frame,
/// so there is nothing to repeat across.
int require_repeat(const Flags& flags, bool video) {
    const std::string* given = find_flag(flags, "repeat");
    if (given == nullptr) {
        return 1;
    }
    if (!video) {
        throw std::runtime_error("--repeat applies to video only: an image is a single frame");
    }
    const int value = std::stoi(*given);
    if (value < datum::kLeastRepeat || value > datum::kMostRepeat) {
        throw std::runtime_error("--repeat must be between " + std::to_string(datum::kLeastRepeat) +
                                 " and " + std::to_string(datum::kMostRepeat));
    }
    return value;
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
        const int repeat = require_repeat(flags, true);
        print_capacity(mode, datum::video_capacity(info, mode, param, repeat));
        return 0;
    }

    const datum::Image image = datum::load(input);
    require_repeat(flags, false);
    print_capacity(mode,
                   datum::payload_capacity(mode, datum::make_codec(mode, param)->capacity(image)));
    return 0;
}

int run_embed(const Flags& flags) {
    const std::filesystem::path input = require(flags, "i");
    const std::filesystem::path output = require(flags, "o");
    const std::vector<uint8_t> payload = datum::read_bytes(require(flags, "d"));
    const datum::Mode mode = require_mode(flags, datum::Mode::Raw);
    const uint8_t param = require_param(flags, mode);

    if (datum::is_video(input)) {
        const int repeat = require_repeat(flags, true);
        const datum::VideoStats stats =
            datum::embed_video(input, output, mode, param, payload, repeat);
        std::cout << "embedded " << payload.size() << " bytes in " << datum::mode_name(mode)
                  << " mode, " << (repeat > 1 ? "copied into " : "filling ") << stats.frames_used
                  << " of " << stats.frames << " frames";
        print_distortion(stats.psnr);
        std::cout << "\n";
        return 0;
    }

    require_repeat(flags, false);
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

/// Steganalysis of a carrier, aimed at our own output. Both tests hunt the LSB
/// *replacement* signature, which is what Phase 4's switch to LSB matching removed:
/// a high reading here on a `datum`-made stego is a regression, not a curiosity.
int run_analyze(const Flags& flags) {
    const std::filesystem::path input = require(flags, "i");
    const datum::Image carrier =
        datum::is_video(input) ? datum::first_frame(input) : datum::load(input);
    const datum::Analysis report = datum::analyze(carrier);

    std::cout << std::fixed << std::setprecision(3)
              << "chi-square:  p(replacement) = " << report.chi.p_embedded << " over "
              << report.chi.pairs << " value pairs, worst in the first " << std::setprecision(0)
              << report.chi.prefix * 100.0 << "%" << std::setprecision(3) << "\n"
              << "RS analysis: " << std::setprecision(1) << report.rs.rate * 100.0
              << "% of the low bits look like payload" << std::setprecision(3) << " (R "
              << report.rs.regular << " / S " << report.rs.singular << ")" << "\n";

    // The two tests fail in different places — chi-square is blind to LSB matching,
    // RS over-reads it — so the verdict names which one spoke rather than merging
    // them into one number.
    //
    // ponytail: flat thresholds, no calibration set behind them. Wide enough that a
    // clean cover and a filled one land on opposite sides; tighten against real
    // covers if this ever has to mean more than a smoke test.
    const bool chi_flags = report.chi.p_embedded > 0.5;
    const bool rs_flags = report.rs.rate > 0.10;
    std::cout << "verdict:     "
              << (chi_flags && rs_flags ? "both tests see a payload"
                  : chi_flags           ? "chi-square sees the LSB replacement signature"
                  : rs_flags            ? "RS analysis sees a payload"
                                        : "clean — neither test sees a payload")
              << "\n";
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
        if (command == "info") {
            return run_info(parse_flags(argc, argv, 2, command, {"i"}));
        }
        if (command == "capacity") {
            return run_capacity(parse_flags(
                argc, argv, 2, command, {"i", "mode", "bits", "delta", "margin", "repeat"}));
        }
        if (command == "embed") {
            return run_embed(
                parse_flags(argc,
                            argv,
                            2,
                            command,
                            {"i", "o", "d", "mode", "bits", "delta", "margin", "repeat"}));
        }
        if (command == "extract") {
            return run_extract(parse_flags(argc, argv, 2, command, {"i", "o", "mode"}));
        }
        if (command == "analyze") {
            return run_analyze(parse_flags(argc, argv, 2, command, {"i"}));
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
