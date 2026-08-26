#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace datum {

enum class Mode : uint8_t {
    Binary = 0,
    Raw = 1,
    Lsb = 2,
    Qim = 3,
    Dct = 4,
};

/// The fixed prologue every embedded stream starts with, so extraction knows the
/// mode, the length and whether it read the payload correctly. Layout and the
/// reasoning behind it: docs/ROADMAP.md, section 3.
struct Header {
    Mode mode = Mode::Binary;
    uint8_t flags = 0;  ///< reserved: bit0 encrypted, bit1 has_name, bit2 ecc
    uint8_t param = 0;  ///< mode parameter (lsb: k, qim: delta)
    uint32_t payload_len = 0;
    uint32_t payload_crc = 0;
};

inline constexpr std::size_t kHeaderSize = 16;

uint32_t crc32(std::span<const uint8_t> bytes);

std::vector<uint8_t> serialize(const Header& header);

/// Returns nullopt when the magic or version does not match — which is also what
/// "there is no payload here" looks like.
std::optional<Header> parse_header(std::span<const uint8_t> bytes);

std::string_view mode_name(Mode mode);
std::optional<Mode> mode_from_name(std::string_view name);

}  // namespace datum
