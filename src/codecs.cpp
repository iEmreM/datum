#include <algorithm>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#include "datum/codec.hpp"

namespace datum {
namespace {

std::size_t sample_index(const Image& image, std::size_t pixel, int channel) {
    return pixel * static_cast<std::size_t>(image.channels) + static_cast<std::size_t>(channel);
}

/// L0: one bit per pixel, painted as full black or full white. The image becomes
/// the data — visible by design. Because a bit survives as long as the pixel stays
/// on its side of mid-grey, this is also the mode with the widest error margin.
class BinaryCodec final : public Codec {
  public:
    std::size_t capacity(const Image& image) const override {
        return image.pixel_count() / 8;
    }

    void embed(Image& image, BitReader& source) const override {
        const int colors = image.color_channels();
        for (std::size_t pixel = 0; pixel < image.pixel_count() && !source.exhausted(); ++pixel) {
            const uint8_t value = source.read() ? 0xFF : 0x00;
            for (int channel = 0; channel < colors; ++channel) {
                image.pixels[sample_index(image, pixel, channel)] = value;
            }
        }
    }

    void extract(const Image& image, BitWriter& sink, std::size_t bits) const override {
        const std::size_t count = std::min(bits, image.pixel_count());
        for (std::size_t pixel = 0; pixel < count; ++pixel) {
            sink.write(image.pixels[sample_index(image, pixel, 0)] >= 128);
        }
    }
};

/// L1: every colour channel byte is one payload byte. Maximum capacity, and the
/// picture is destroyed — this treats the image purely as a container.
class RawCodec final : public Codec {
  public:
    std::size_t capacity(const Image& image) const override {
        return image.pixel_count() * static_cast<std::size_t>(image.color_channels());
    }

    void embed(Image& image, BitReader& source) const override {
        const int colors = image.color_channels();
        for (std::size_t pixel = 0; pixel < image.pixel_count(); ++pixel) {
            for (int channel = 0; channel < colors; ++channel) {
                if (source.remaining() < 8) {
                    return;
                }
                uint8_t byte = 0;
                for (int bit = 0; bit < 8; ++bit) {
                    byte = static_cast<uint8_t>(byte << 1 | (source.read() ? 1u : 0u));
                }
                image.pixels[sample_index(image, pixel, channel)] = byte;
            }
        }
    }

    void extract(const Image& image, BitWriter& sink, std::size_t bits) const override {
        const int colors = image.color_channels();
        for (std::size_t pixel = 0; pixel < image.pixel_count(); ++pixel) {
            for (int channel = 0; channel < colors; ++channel) {
                if (sink.size() >= bits) {
                    return;
                }
                const uint8_t byte = image.pixels[sample_index(image, pixel, channel)];
                for (int bit = 7; bit >= 0; --bit) {
                    sink.write(((byte >> bit) & 1u) != 0);
                }
            }
        }
    }
};

}  // namespace

std::unique_ptr<Codec> make_codec(Mode mode, uint8_t param) {
    (void)param;  // used from Phase 2 onward (lsb: k, qim: delta)
    switch (mode) {
        case Mode::Binary:
            return std::make_unique<BinaryCodec>();
        case Mode::Raw:
            return std::make_unique<RawCodec>();
        case Mode::Lsb:
        case Mode::Qim:
        case Mode::Dct:
            break;
    }
    throw std::runtime_error("mode " + std::string(mode_name(mode)) + " is not implemented yet");
}

std::span<const Mode> detectable_modes() {
    static constexpr Mode kModes[] = {Mode::Binary, Mode::Raw};
    return kModes;
}

namespace {

/// Reads back a header with the given codec. Returns nullopt when there is none,
/// which is also what a wrong mode guess looks like.
std::optional<Header> peek_header(const Image& image, const Codec& codec) {
    if (codec.capacity(image) < kHeaderSize) {
        return std::nullopt;
    }
    BitWriter sink;
    codec.extract(image, sink, kHeaderSize * 8);
    return parse_header(sink.bytes());
}

}  // namespace

void embed_payload(Image& image, Mode mode, uint8_t param, std::span<const uint8_t> payload) {
    const auto codec = make_codec(mode, param);
    const std::size_t needed = kHeaderSize + payload.size();
    const std::size_t available = codec->capacity(image);
    if (needed > available) {
        throw std::runtime_error("payload needs " + std::to_string(needed) + " bytes but " +
                                 std::string(mode_name(mode)) + " holds only " +
                                 std::to_string(available));
    }

    Header header;
    header.mode = mode;
    header.param = param;
    header.payload_len = static_cast<uint32_t>(payload.size());
    header.payload_crc = crc32(payload);

    std::vector<uint8_t> stream = serialize(header);
    stream.insert(stream.end(), payload.begin(), payload.end());

    BitReader source(stream);
    codec->embed(image, source);
}

Extracted extract_payload(const Image& image, std::optional<Mode> mode) {
    std::unique_ptr<Codec> codec;
    std::optional<Header> header;

    if (mode) {
        codec = make_codec(*mode, 0);
        header = peek_header(image, *codec);
    } else {
        for (const Mode candidate : detectable_modes()) {
            auto attempt = make_codec(candidate, 0);
            // The mode byte must agree with the codec that read it, otherwise a
            // chance match on the magic would be accepted as a payload.
            if (auto found = peek_header(image, *attempt); found && found->mode == candidate) {
                codec = std::move(attempt);
                header = found;
                break;
            }
        }
    }

    if (!header) {
        throw std::runtime_error("no datum payload found (or the wrong mode was given)");
    }

    const std::size_t needed = kHeaderSize + header->payload_len;
    if (needed > codec->capacity(image)) {
        throw std::runtime_error("header claims " + std::to_string(header->payload_len) +
                                 " payload bytes, more than this image can hold");
    }

    BitWriter sink;
    codec->extract(image, sink, needed * 8);
    Extracted result;
    result.header = *header;
    result.payload.assign(sink.bytes().begin() + static_cast<std::ptrdiff_t>(kHeaderSize),
                          sink.bytes().begin() + static_cast<std::ptrdiff_t>(needed));

    if (crc32(result.payload) != header->payload_crc) {
        throw std::runtime_error(
            "CRC mismatch — the payload is corrupted (the image was probably "
            "resized or re-encoded after embedding)");
    }
    return result;
}

}  // namespace datum
