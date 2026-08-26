#include <exception>
#include <iostream>
#include <map>
#include <string>

#include "datum/image.hpp"

namespace {

/// Flag name (leading dashes stripped) -> value.
using Flags = std::map<std::string, std::string, std::less<>>;

void print_usage() {
    std::cerr << "datum — embed data inside pixel values\n\n"
                 "usage:\n"
                 "  datum info -i <image>    describe an image and its raw capacity\n";
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

int run_info(const Flags& flags) {
    // ponytail: argv is ANSI on Windows, so non-ASCII paths mangle here even though
    // the image layer is wide-path clean. Switch to wmain/GetCommandLineW if it bites.
    const datum::Image image = datum::load(require(flags, "i"));
    std::cout << image.width << "x" << image.height << ", " << image.channels << " channel(s)\n"
              << image.pixel_count() << " pixels, " << image.sample_count() << " channel bytes\n";
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        print_usage();
        return 2;
    }

    const std::string command = argv[1];
    try {
        const Flags flags = parse_flags(argc, argv, 2);
        if (command == "info") {
            return run_info(flags);
        }
        throw std::runtime_error("unknown command: " + command);
    } catch (const std::exception& error) {
        std::cerr << "error: " << error.what() << "\n\n";
        print_usage();
        return 1;
    }
}
