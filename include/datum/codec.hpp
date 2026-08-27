#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <vector>

#include "datum/bitstream.hpp"
#include "datum/container.hpp"
#include "datum/image.hpp"

namespace datum {

/// One embedding mode. Every mode speaks bits, so the CLI, the payload container
/// and (from Phase 3) the video layer are written once and work for all of them.
class Codec {
  public:
    virtual ~Codec() = default;

    /// Total bytes this image holds in this mode, header included.
    virtual std::size_t capacity(const Image& image) const = 0;

    /// Writes bits from `source` until either the image or the source runs out.
    virtual void embed(Image& image, BitReader& source) const = 0;

    /// Reads from the top of the image until `sink` holds `bits` bits in total, or
    /// the image runs out. `bits` is a target for the whole sink, not a count to
    /// append — so calling this on frame after frame with the same sink resumes
    /// where the last frame stopped, which is how a payload spans a video.
    virtual void extract(const Image& image, BitWriter& sink, std::size_t bits) const = 0;
};

/// The quantiser step `qim` accepts, and the default docs/QIM.md settled on. Below
/// the smallest there is no margin worth the name; above the largest the banding
/// costs more than the extra margin buys.
inline constexpr int kLeastDelta = 2;
inline constexpr int kMostDelta = 32;
inline constexpr int kDefaultDelta = 28;

/// Throws std::runtime_error for modes that are not implemented yet.
std::unique_ptr<Codec> make_codec(Mode mode, uint8_t param);

/// Prepends a DTM1 header to `payload` and writes the whole stream into `image`.
/// Throws std::runtime_error if the payload does not fit.
void embed_payload(Image& image, Mode mode, uint8_t param, std::span<const uint8_t> payload);

struct Extracted {
    Header header;
    std::vector<uint8_t> payload;
};

/// The codec that wrote a carrier, together with the header it wrote.
struct Detected {
    std::unique_ptr<Codec> codec;
    Header header;
};

/// Works out which codec wrote `carrier` by reading a header with each candidate
/// and keeping the one whose header agrees with the codec that read it. Pass a
/// mode to restrict the search. Returns nullopt when nothing valid is there.
///
/// A video calls this on its first frame and an image on itself, so both paths
/// share one definition of "is there a payload here, and whose is it".
std::optional<Detected> detect_codec(const Image& carrier, std::optional<Mode> forced);

/// Splits a decoded `header + payload` byte stream and verifies the CRC. Throws
/// std::runtime_error on a mismatch rather than returning a corrupted payload.
Extracted verify_payload(const Header& header, std::span<const uint8_t> stream);

/// Recovers a payload and verifies its CRC. Pass a mode to force one, or nullopt
/// to try each known mode. Throws std::runtime_error when nothing valid is found
/// or the CRC does not match.
Extracted extract_payload(const Image& image, std::optional<Mode> mode);

}  // namespace datum
