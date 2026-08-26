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

    /// Reads the first `bits` bits back out. Extraction runs twice — once for the
    /// header, once for header plus payload — so this always starts from the top.
    virtual void extract(const Image& image, BitWriter& sink, std::size_t bits) const = 0;
};

/// Throws std::runtime_error for modes that are not implemented yet.
std::unique_ptr<Codec> make_codec(Mode mode, uint8_t param);

/// Modes `datum extract` tries when the user did not name one, cheapest first.
std::span<const Mode> detectable_modes();

/// Prepends a DTM1 header to `payload` and writes the whole stream into `image`.
/// Throws std::runtime_error if the payload does not fit.
void embed_payload(Image& image, Mode mode, uint8_t param, std::span<const uint8_t> payload);

struct Extracted {
    Header header;
    std::vector<uint8_t> payload;
};

/// Recovers a payload and verifies its CRC. Pass a mode to force one, or nullopt
/// to try `detectable_modes()`. Throws std::runtime_error when nothing valid is
/// found or the CRC does not match.
Extracted extract_payload(const Image& image, std::optional<Mode> mode);

}  // namespace datum
