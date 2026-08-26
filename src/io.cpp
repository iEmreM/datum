#include "datum/io.hpp"

#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>

namespace datum {

std::vector<uint8_t> read_bytes(const std::filesystem::path& file) {
    std::ifstream in(file, std::ios::binary);
    if (!in) {
        throw std::runtime_error("cannot open " + file.string());
    }
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

void write_bytes(const std::filesystem::path& file, std::span<const uint8_t> bytes) {
    std::ofstream out(file, std::ios::binary);
    if (!out) {
        throw std::runtime_error("cannot create " + file.string());
    }
    out.write(reinterpret_cast<const char*>(bytes.data()),
              static_cast<std::streamsize>(bytes.size()));
    if (!out) {
        throw std::runtime_error("cannot write " + file.string());
    }
}

}  // namespace datum
