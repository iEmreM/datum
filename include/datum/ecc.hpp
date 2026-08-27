#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace datum {

/// Reed-Solomon RS(255, 223) over GF(2^8): 32 parity bytes per 255-byte block,
/// correcting up to 16 wrong bytes wherever they fall in it.
///
/// This is the layer that turns "most of the payload came back" into "the payload
/// came back". Every mode before `dct` is all-or-nothing — a lossless carrier gives
/// a perfect stream and a lossy one gives noise — so none of them had any use for
/// it. `dct` is the first mode whose stream arrives *slightly* wrong, and a CRC
/// needs every bit.
///
/// The correction limit is what it is: 16 of 255 bytes is 6.3% of a block, so an
/// error rate above roughly 0.8% *per bit* (which is where isolated bit errors
/// start landing in more than 16 distinct bytes) is beyond it. That number is the
/// target the dct margin is tuned against, not a detail.
inline constexpr std::size_t kEccParity = 32;
inline constexpr std::size_t kEccData = 223;

/// Bytes `ecc_encode` returns for `data_size` bytes in. A final short block keeps
/// the full 32 parity bytes, so a 16-byte header is carried in 48 and still
/// tolerates 16 errors — the strongest protection in the stream, which is right,
/// because a header that does not parse loses the payload behind it.
std::size_t ecc_encoded_size(std::size_t data_size);

std::vector<uint8_t> ecc_encode(std::span<const uint8_t> data);

/// Returns nullopt when a block carries more errors than RS can correct, or when
/// the correction it found does not check out. Never returns a wrong answer
/// silently: the syndromes are recomputed after correcting.
std::optional<std::vector<uint8_t>> ecc_decode(std::span<const uint8_t> code,
                                               std::size_t data_size);

}  // namespace datum
