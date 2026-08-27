#pragma once

#include <algorithm>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <span>
#include <stdexcept>
#include <vector>

namespace datum {

/// Share of bits that differ between two byte sequences, over the shorter of the
/// two. This is the pass/fail number of every robustness measurement: a mode that
/// survives a re-encode reads 0 here, one that does not reads about 0.5, which is
/// what guessing scores.
inline double bit_error_rate(std::span<const uint8_t> a, std::span<const uint8_t> b) {
    const std::size_t bytes = std::min(a.size(), b.size());
    if (bytes == 0) {
        return 0.0;
    }
    std::size_t wrong = 0;
    for (std::size_t i = 0; i < bytes; ++i) {
        wrong += static_cast<std::size_t>(std::popcount(static_cast<uint8_t>(a[i] ^ b[i])));
    }
    return static_cast<double>(wrong) / static_cast<double>(bytes * 8);
}

/// Reads bits MSB-first out of a byte sequence.
///
/// This is the seam that lets a payload span several images or video frames: a
/// codec pulls bits until the carrier is full and the next carrier resumes from
/// the same reader, without either side knowing the other exists.
class BitReader {
  public:
    explicit BitReader(std::span<const uint8_t> bytes) : bytes_(bytes) {}

    std::size_t remaining() const {
        return bytes_.size() * 8 - position_;
    }

    bool exhausted() const {
        return remaining() == 0;
    }

    bool read() {
        if (exhausted()) {
            throw std::runtime_error("bit stream exhausted");
        }
        const uint8_t byte = bytes_[position_ / 8];
        const unsigned shift = 7 - static_cast<unsigned>(position_ % 8);
        ++position_;
        return ((byte >> shift) & 1u) != 0;
    }

  private:
    std::span<const uint8_t> bytes_;
    std::size_t position_ = 0;
};

/// Collects bits MSB-first. A trailing partial byte is zero-padded.
class BitWriter {
  public:
    void write(bool bit) {
        if (count_ % 8 == 0) {
            bytes_.push_back(0);
        }
        if (bit) {
            bytes_.back() |= static_cast<uint8_t>(1u << (7 - count_ % 8));
        }
        ++count_;
    }

    std::size_t size() const {
        return count_;
    }

    const std::vector<uint8_t>& bytes() const {
        return bytes_;
    }

  private:
    std::vector<uint8_t> bytes_;
    std::size_t count_ = 0;
};

}  // namespace datum
