#include <algorithm>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
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
        for (std::size_t pixel = 0; pixel < image.pixel_count(); ++pixel) {
            if (sink.size() >= bits) {
                return;
            }
            sink.write(image.pixels[sample_index(image, pixel, 0)] >= 128);
        }
    }
};

/// L2: replace the low k bits of every colour channel with k payload bits. At
/// k = 1 a channel moves by at most 1 (PSNR ≈ 51 dB), which is below the threshold
/// of human vision; higher k buys capacity at the cost of visibility. k is stored
/// in the header, so extraction recovers it without being told.
class LsbCodec final : public Codec {
  public:
    explicit LsbCodec(int k) : k_(k) {
        if (k < 1 || k > 8) {
            throw std::runtime_error("lsb bits must be between 1 and 8");
        }
    }

    std::size_t capacity(const Image& image) const override {
        return image.pixel_count() * static_cast<std::size_t>(image.color_channels()) *
               static_cast<std::size_t>(k_) / 8;
    }

    void embed(Image& image, BitReader& source) const override {
        const int colors = image.color_channels();
        const auto keep = static_cast<uint8_t>(~((1u << k_) - 1u));  // high bits kept as-is
        for (std::size_t pixel = 0; pixel < image.pixel_count(); ++pixel) {
            for (int channel = 0; channel < colors; ++channel) {
                if (source.exhausted()) {
                    return;
                }
                int taken = 0;
                uint8_t low = 0;
                for (; taken < k_ && !source.exhausted(); ++taken) {
                    low = static_cast<uint8_t>(low << 1 | (source.read() ? 1u : 0u));
                }
                // A final partial group (only with k = 3, when 8·bytes ∤ k) is
                // left-aligned so extraction reads its real bits before the padding.
                low = static_cast<uint8_t>(low << (k_ - taken));
                uint8_t& sample = image.pixels[sample_index(image, pixel, channel)];
                sample = static_cast<uint8_t>((sample & keep) | low);
            }
        }
    }

    void extract(const Image& image, BitWriter& sink, std::size_t bits) const override {
        const int colors = image.color_channels();
        const auto mask = static_cast<uint8_t>((1u << k_) - 1u);
        for (std::size_t pixel = 0; pixel < image.pixel_count(); ++pixel) {
            for (int channel = 0; channel < colors; ++channel) {
                const uint8_t low = image.pixels[sample_index(image, pixel, channel)] & mask;
                for (int bit = k_ - 1; bit >= 0; --bit) {
                    if (sink.size() >= bits) {
                        return;
                    }
                    sink.write(((low >> bit) & 1u) != 0);
                }
            }
        }
    }

  private:
    int k_;
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
    switch (mode) {
        case Mode::Binary:
            return std::make_unique<BinaryCodec>();
        case Mode::Raw:
            return std::make_unique<RawCodec>();
        case Mode::Lsb:
            return std::make_unique<LsbCodec>(param);
        case Mode::Qim:
        case Mode::Dct:
            break;
    }
    throw std::runtime_error("mode " + std::string(mode_name(mode)) + " is not implemented yet");
}

namespace {

/// (mode, param) pairs to try when no mode is forced, cheapest first. lsb records
/// its k in the header, so every k is a distinct guess: a header parsed at the
/// wrong k fails the `param == k` self-consistency check in detect_codec.
std::vector<std::pair<Mode, uint8_t>> detection_candidates() {
    std::vector<std::pair<Mode, uint8_t>> candidates = {{Mode::Binary, 0}, {Mode::Raw, 0}};
    for (uint8_t k = 1; k <= 4; ++k) {
        candidates.emplace_back(Mode::Lsb, k);
    }
    return candidates;
}

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

std::optional<Detected> detect_codec(const Image& carrier, std::optional<Mode> forced) {
    for (const auto& [candidate, param] : detection_candidates()) {
        if (forced && *forced != candidate) {
            continue;
        }
        auto attempt = make_codec(candidate, param);
        // The header must agree with the codec that read it — same mode, same
        // parameter — or a chance match on the magic would be taken as a payload.
        if (auto found = peek_header(carrier, *attempt);
            found && found->mode == candidate && found->param == param) {
            return Detected{std::move(attempt), *found};
        }
    }
    return std::nullopt;
}

Extracted verify_payload(const Header& header, std::span<const uint8_t> stream) {
    if (stream.size() < kHeaderSize + header.payload_len) {
        throw std::runtime_error("payload is shorter than its header claims");
    }

    Extracted result;
    result.header = header;
    result.payload.assign(stream.begin() + static_cast<std::ptrdiff_t>(kHeaderSize),
                          stream.begin() + static_cast<std::ptrdiff_t>(kHeaderSize) +
                              static_cast<std::ptrdiff_t>(header.payload_len));

    if (crc32(result.payload) != header.payload_crc) {
        throw std::runtime_error(
            "CRC mismatch — the payload is corrupted (the carrier was probably "
            "resized or re-encoded after embedding)");
    }
    return result;
}

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
    const std::optional<Detected> found = detect_codec(image, mode);
    if (!found) {
        throw std::runtime_error("no datum payload found (or the wrong mode was given)");
    }

    const std::size_t needed = kHeaderSize + found->header.payload_len;
    if (needed > found->codec->capacity(image)) {
        throw std::runtime_error("header claims " + std::to_string(found->header.payload_len) +
                                 " payload bytes, more than this image can hold");
    }

    BitWriter sink;
    found->codec->extract(image, sink, needed * 8);
    return verify_payload(found->header, sink.bytes());
}

}  // namespace datum
