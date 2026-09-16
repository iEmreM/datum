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
    uint8_t flags = 0;  ///< bit0 encrypted, bit1 has_name, bit2 ecc, bits 3-7 repeat
    uint8_t param = 0;  ///< mode parameter (lsb: k, qim: delta, dct: margin)
    uint32_t payload_len = 0;
    uint32_t payload_crc = 0;
};

inline constexpr std::size_t kHeaderSize = 16;

/// The stream carries Reed-Solomon parity. Set by the modes that expect to be
/// damaged, and checked at extraction against the mode that claims to have written
/// it — a header that disagrees with itself is not one of ours.
inline constexpr uint8_t kFlagEcc = 0x04;

/// Video temporal repetition lives in the five flag bits nothing else uses: the
/// whole stream is written into each of the first N frames, one complete copy per
/// frame, and the copies are majority-voted back together at extraction.
///
/// It goes here rather than in a new header field because the field would cost a
/// format version bump for five bits, and 32 copies is already far past the point
/// where the capacity it costs is worth having.
inline constexpr int kLeastRepeat = 1;
inline constexpr int kMostRepeat = 32;

int header_repeat(const Header& header);
void set_header_repeat(Header& header, int repeat);

uint32_t crc32(std::span<const uint8_t> bytes);

std::vector<uint8_t> serialize(const Header& header);

/// Returns nullopt when the magic or version does not match — which is also what
/// "there is no payload here" looks like.
std::optional<Header> parse_header(std::span<const uint8_t> bytes);

std::string_view mode_name(Mode mode);
std::optional<Mode> mode_from_name(std::string_view name);

}  // namespace datum
