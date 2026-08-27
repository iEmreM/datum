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

/// The separation `dct` forces between its two coefficients, in the same units a
/// JPEG quantisation step is measured in, and the default docs/DCT.md settled on.
/// Below the smallest the relation does not survive being rounded back to whole
/// pixels; above the largest the block is visibly textured.
inline constexpr int kLeastMargin = 2;
inline constexpr int kMostMargin = 64;
inline constexpr int kDefaultMargin = 32;

/// The side of a `dct` block, in pixels. One bit rides on each.
inline constexpr int kDctBlock = 8;

/// Throws std::runtime_error for modes that are not implemented yet.
std::unique_ptr<Codec> make_codec(Mode mode, uint8_t param);

/// True when this mode's stream is Reed-Solomon coded. Only `dct` is, and that is
/// the whole difference between the two families of mode here: every other one
/// either reaches the decoder perfectly or not at all, so parity bytes would only
/// cost capacity. `dct` is the first whose stream arrives *slightly* wrong, and a
/// CRC needs every bit of it.
bool uses_ecc(Mode mode);

/// Bytes the header occupies at the front of the stream: 16 plain, 48 with parity.
/// Extraction has to read exactly this much before it knows anything else.
std::size_t header_block_size(Mode mode);

/// Bytes of carrier the stream for a payload of this length occupies: the header
/// block, then the payload block, each with parity when the mode asks for it.
std::size_t stream_size(Mode mode, std::size_t payload_len);

/// The header describing this payload — length, CRC and the flags the mode implies.
Header make_header(Mode mode, uint8_t param, std::span<const uint8_t> payload);

/// The inverse: the largest payload `carrier_bytes` of raw capacity can hold.
std::size_t payload_capacity(Mode mode, std::size_t carrier_bytes);

/// Prepends a DTM1 header to `payload` and writes the whole stream into `image`.
/// Throws std::runtime_error if the payload does not fit.
void embed_payload(Image& image, Mode mode, uint8_t param, std::span<const uint8_t> payload);

/// The exact bytes a codec is fed for this header and payload. Shared so the image
/// and video paths cannot drift apart about what a stream looks like.
std::vector<uint8_t> build_stream(const Header& header, std::span<const uint8_t> payload);

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
