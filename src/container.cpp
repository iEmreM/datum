#include "datum/container.hpp"

#include <array>
#include <cstring>

namespace datum {
namespace {

constexpr std::array<char, 4> kMagic = {'D', 'T', 'M', '1'};
constexpr uint8_t kVersion = 1;

void put_u32_le(std::vector<uint8_t>& out, uint32_t value) {
    out.push_back(static_cast<uint8_t>(value));
    out.push_back(static_cast<uint8_t>(value >> 8));
    out.push_back(static_cast<uint8_t>(value >> 16));
    out.push_back(static_cast<uint8_t>(value >> 24));
}

uint32_t get_u32_le(std::span<const uint8_t> bytes, std::size_t offset) {
    return static_cast<uint32_t>(bytes[offset]) | static_cast<uint32_t>(bytes[offset + 1]) << 8 |
           static_cast<uint32_t>(bytes[offset + 2]) << 16 |
           static_cast<uint32_t>(bytes[offset + 3]) << 24;
}

}  // namespace

uint32_t crc32(std::span<const uint8_t> bytes) {
    static const std::array<uint32_t, 256> table = [] {
        std::array<uint32_t, 256> generated{};
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t value = i;
            for (int bit = 0; bit < 8; ++bit) {
                value = (value & 1u) != 0 ? 0xEDB88320u ^ (value >> 1) : value >> 1;
            }
            generated[i] = value;
        }
        return generated;
    }();

    uint32_t crc = 0xFFFFFFFFu;
    for (const uint8_t byte : bytes) {
        crc = table[(crc ^ byte) & 0xFFu] ^ (crc >> 8);
    }
    return crc ^ 0xFFFFFFFFu;
}

std::vector<uint8_t> serialize(const Header& header) {
    std::vector<uint8_t> bytes;
    bytes.reserve(kHeaderSize);
    for (const char c : kMagic) {
        bytes.push_back(static_cast<uint8_t>(c));
    }
    bytes.push_back(kVersion);
    bytes.push_back(static_cast<uint8_t>(header.mode));
    bytes.push_back(header.flags);
    bytes.push_back(header.param);
    put_u32_le(bytes, header.payload_len);
    put_u32_le(bytes, header.payload_crc);
    return bytes;
}

std::optional<Header> parse_header(std::span<const uint8_t> bytes) {
    if (bytes.size() < kHeaderSize) {
        return std::nullopt;
    }
    for (std::size_t i = 0; i < kMagic.size(); ++i) {
        if (bytes[i] != static_cast<uint8_t>(kMagic[i])) {
            return std::nullopt;
        }
    }
    if (bytes[4] != kVersion || bytes[5] > static_cast<uint8_t>(Mode::Dct)) {
        return std::nullopt;
    }

    Header header;
    header.mode = static_cast<Mode>(bytes[5]);
    header.flags = bytes[6];
    header.param = bytes[7];
    header.payload_len = get_u32_le(bytes, 8);
    header.payload_crc = get_u32_le(bytes, 12);
    return header;
}

std::string_view mode_name(Mode mode) {
    switch (mode) {
        case Mode::Binary:
            return "binary";
        case Mode::Raw:
            return "raw";
        case Mode::Lsb:
            return "lsb";
        case Mode::Qim:
            return "qim";
        case Mode::Dct:
            return "dct";
    }
    return "unknown";
}

std::optional<Mode> mode_from_name(std::string_view name) {
    for (const Mode mode : {Mode::Binary, Mode::Raw, Mode::Lsb, Mode::Qim, Mode::Dct}) {
        if (mode_name(mode) == name) {
            return mode;
        }
    }
    return std::nullopt;
}

}  // namespace datum
