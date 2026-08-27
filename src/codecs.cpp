#include <algorithm>
#include <memory>
#include <optional>
#include <random>
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

/// Pulls up to `k` bits into the low end of a byte, MSB-first.
///
/// A final partial group — the source ran out mid-group, which happens whenever
/// 8·bytes is not a multiple of k — is left-aligned, so extraction reads its real
/// bits before the padding rather than after it.
uint8_t take_bits(BitReader& source, int k) {
    int taken = 0;
    uint8_t value = 0;
    for (; taken < k && !source.exhausted(); ++taken) {
        value = static_cast<uint8_t>(value << 1 | (source.read() ? 1u : 0u));
    }
    return static_cast<uint8_t>(value << (k - taken));
}

/// Pushes the low `k` bits of `value`, MSB-first. Returns false once `sink` holds
/// `bits` bits in total, which is the caller's signal to stop scanning the image.
bool put_bits(BitWriter& sink, uint8_t value, int k, std::size_t bits) {
    for (int bit = k - 1; bit >= 0; --bit) {
        if (sink.size() >= bits) {
            return false;
        }
        sink.write(((value >> bit) & 1u) != 0);
    }
    return true;
}

/// Moves `sample` to the nearest value whose low k bits are `low`, instead of
/// overwriting them: LSB *matching* rather than LSB replacement.
///
/// Replacement drives the counts of each value pair (2i, 2i+1) toward equality,
/// which is exactly what chi-square and RS analysis look for and is a pattern no
/// natural image has. Adding or subtracting leaves no such pair to equalise:
/// measured on a real photograph, it takes chi-square from p = 1.000 to p = 0.000.
/// It is also quieter — at most 2^(k-1) off instead of 2^k - 1, worth 2.7 dB at
/// k = 4 — and extraction is unchanged, because the low bits end up the same
/// either way. See `datum analyze` for what this does and does not buy.
///
/// At k = 1 the two candidates are always equidistant, so `direction` is the whole
/// decision; it breaks ties only, and nothing secret rides on it.
uint8_t nudge(uint8_t sample, uint8_t low, int step, std::minstd_rand& direction) {
    const int up_by = (low - (sample & (step - 1)) + step) % step;
    if (up_by == 0) {
        return sample;  // the low bits already say what we want
    }
    const int down_by = step - up_by;
    const int up = sample + up_by;
    // Only one of the two can fall outside 0..255: reaching past 255 needs a large
    // sample, reaching below 0 needs a small one.
    const bool go_up =
        sample < down_by ||
        (up <= 255 && (up_by < down_by || (up_by == down_by && (direction() & 1u) != 0u)));
    return static_cast<uint8_t>(go_up ? up : sample - down_by);
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

/// L2: the low k bits of every colour channel carry k payload bits. At
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
        const int step = 1 << k_;
        // Fixed seed: the ± choice must be unpredictable from the pixel, not secret,
        // and a constant one keeps embedding reproducible for the tests.
        std::minstd_rand direction(0x5EEDu);
        for (std::size_t pixel = 0; pixel < image.pixel_count(); ++pixel) {
            for (int channel = 0; channel < colors; ++channel) {
                if (source.exhausted()) {
                    return;
                }
                const uint8_t low = take_bits(source, k_);
                uint8_t& sample = image.pixels[sample_index(image, pixel, channel)];
                sample = nudge(sample, low, step, direction);
            }
        }
    }

    void extract(const Image& image, BitWriter& sink, std::size_t bits) const override {
        const int colors = image.color_channels();
        const auto mask = static_cast<uint8_t>((1u << k_) - 1u);
        for (std::size_t pixel = 0; pixel < image.pixel_count(); ++pixel) {
            for (int channel = 0; channel < colors; ++channel) {
                const uint8_t low = image.pixels[sample_index(image, pixel, channel)] & mask;
                if (!put_bits(sink, low, k_, bits)) {
                    return;
                }
            }
        }
    }

  private:
    int k_;
};

/// L1: the top k bits of every colour channel carry k payload bits, and the bits
/// below them are set to the middle of the bucket those top bits name. At k = 8
/// that is one payload byte per channel — maximum capacity, and the picture is
/// destroyed, this treats the image purely as a container.
///
/// Below 8 it is a robustness dial. The bucket is 2^(8-k) wide and the sample sits
/// at its centre, so the channel can drift by half a bucket either way and still
/// decode to the same k bits: k = 5 tolerates ±4, k = 4 tolerates ±8. Centring is
/// the whole trick — k = 8's implicit floor placement gives away that margin, since
/// any downward drift at all crosses into the bucket below.
///
/// What this does *not* fix is a lossy codec's chroma subsampling, which averages
/// R, G and B across neighbouring pixels rather than nudging each one; see
/// docs/RAW_BITS.md for what the measurements actually say.
class RawCodec final : public Codec {
  public:
    explicit RawCodec(int k) : k_(k) {
        if (k < 1 || k > 8) {
            throw std::runtime_error("raw bits must be between 1 and 8");
        }
    }

    std::size_t capacity(const Image& image) const override {
        return image.pixel_count() * static_cast<std::size_t>(image.color_channels()) *
               static_cast<std::size_t>(k_) / 8;
    }

    void embed(Image& image, BitReader& source) const override {
        const int colors = image.color_channels();
        const int step = 1 << (8 - k_);
        for (std::size_t pixel = 0; pixel < image.pixel_count(); ++pixel) {
            for (int channel = 0; channel < colors; ++channel) {
                if (source.exhausted()) {
                    return;
                }
                const int bucket = take_bits(source, k_);
                image.pixels[sample_index(image, pixel, channel)] =
                    static_cast<uint8_t>(bucket * step + step / 2);
            }
        }
    }

    void extract(const Image& image, BitWriter& sink, std::size_t bits) const override {
        const int colors = image.color_channels();
        const int step = 1 << (8 - k_);
        for (std::size_t pixel = 0; pixel < image.pixel_count(); ++pixel) {
            for (int channel = 0; channel < colors; ++channel) {
                const int bucket = image.pixels[sample_index(image, pixel, channel)] / step;
                if (!put_bits(sink, static_cast<uint8_t>(bucket), k_, bits)) {
                    return;
                }
            }
        }
    }

  private:
    int k_;
};

/// The bin a value falls in: round(value / delta).
int qim_bin(int value, int delta) {
    return (value + delta / 2) / delta;
}

/// The multiple of `delta` nearest `value` whose bin index has the parity of `bit`,
/// kept inside [low, high].
///
/// Bins of the right parity sit 2*delta apart, so a window at least that wide always
/// holds one and a single step of two is always enough to get back inside — which is
/// exactly the room `carry_bit` makes before it calls here.
int qim_target(int value, bool bit, int delta, int low, int high) {
    int bin = qim_bin(value, delta);
    if ((bin & 1) != static_cast<int>(bit)) {
        bin += value > bin * delta ? 1 : -1;  // both neighbours qualify; take the nearer
    }
    if (bin * delta > high) {
        bin -= 2;
    }
    if (bin * delta < low) {
        bin += 2;
    }
    return bin * delta;
}

/// How far every colour channel of a pixel can move *together* before one of them
/// clips. The width `up - down` is 255 minus the spread between the channels, so a
/// near-grey pixel can move almost anywhere and a fully saturated one cannot move
/// at all.
struct Headroom {
    int down = 0;
    int up = 0;
};

Headroom headroom(const uint8_t* channels, int colors) {
    int least = 255;
    int most = 0;
    for (int i = 0; i < colors; ++i) {
        least = std::min(least, static_cast<int>(channels[i]));
        most = std::max(most, static_cast<int>(channels[i]));
    }
    return {-least, 255 - most};
}

/// Shifts a whole pixel so that its luma lands on a quantisation bin whose index has
/// the parity of `bit`.
///
/// Every colour channel moves by the *same* offset, which is the entire design. The
/// differences between the channels — the colour — come through untouched, so a
/// codec that stores chroma at quarter resolution has nothing of ours to average
/// away, and BT.601 luma read back from the decoded pixel is the luma the codec
/// kept. Writing the three channels independently, as this mode did first, puts the
/// payload in the chroma as well and 4:2:0 erases it outright: measured in
/// docs/QIM.md, and the same wall `raw` hit in docs/RAW_BITS.md.
void carry_bit(Image& image, std::size_t pixel, bool bit, int delta) {
    const int colors = image.color_channels();
    uint8_t* channels = &image.pixels[sample_index(image, pixel, 0)];

    Headroom room = headroom(channels, colors);
    if (room.up - room.down < 2 * delta) {
        // A pixel this saturated has no room to move as a whole, so pull its channels
        // toward their own luma until it has. Skipping such pixels instead would make
        // the decoder guess which ones were skipped, from values a re-encode has
        // already nudged — and one wrong guess shifts every bit after it. Desaturating
        // a handful of extremes is the cheaper price, and at delta <= 32 it leaves 191
        // of the 255 levels of spread alone, so a photograph barely triggers it.
        const int centre = luma(image, pixel);
        const int want = 255 - 2 * delta;
        const int have = 255 - (room.up - room.down);
        for (int i = 0; i < colors; ++i) {
            channels[i] = static_cast<uint8_t>(centre + (channels[i] - centre) * want / have);
        }
        room = headroom(channels, colors);
    }

    const int value = luma(image, pixel);
    const int offset = qim_target(value, bit, delta, value + room.down, value + room.up) - value;
    for (int i = 0; i < colors; ++i) {
        channels[i] = static_cast<uint8_t>(channels[i] + offset);
    }
}

/// L5: a pixel's luma is snapped to a multiple of delta, and the bit is the parity of
/// that multiple — Quantization Index Modulation.
///
/// Amplitude is the point. `lsb` hides inside a change smaller than a codec's
/// quantiser step, which is exactly why the codec erases it; QIM makes the change
/// *bigger* than the step and reads it back out of the rounding. Anything that moves
/// the luma by less than delta/2 still decodes to the same bin, so delta is a direct
/// robustness dial: small is quiet and fragile, large survives a harder re-encode and
/// pays for it in banding across flat regions. docs/QIM.md has the measured table and
/// where the default came from.
///
/// It is a spatial technique, so it holds only while the pixel grid does. A re-encode
/// at the same resolution nudges values and QIM rounds the nudge away; a rescale
/// *averages* neighbouring pixels, which replaces a value rather than nudging it, and
/// no margin defends against that. That is the line between this mode and `dct`.
class QimCodec final : public Codec {
  public:
    explicit QimCodec(int delta) : delta_(delta) {
        if (delta < kLeastDelta || delta > kMostDelta) {
            throw std::runtime_error("qim delta must be between " + std::to_string(kLeastDelta) +
                                     " and " + std::to_string(kMostDelta));
        }
    }

    /// One bit per pixel, not per channel: the three channels move together and
    /// carry one bit between them, which is what buys the chroma coherence above.
    std::size_t capacity(const Image& image) const override {
        return image.pixel_count() / 8;
    }

    void embed(Image& image, BitReader& source) const override {
        for (std::size_t pixel = 0; pixel < image.pixel_count() && !source.exhausted(); ++pixel) {
            carry_bit(image, pixel, source.read(), delta_);
        }
    }

    void extract(const Image& image, BitWriter& sink, std::size_t bits) const override {
        for (std::size_t pixel = 0; pixel < image.pixel_count(); ++pixel) {
            if (sink.size() >= bits) {
                return;
            }
            sink.write((qim_bin(luma(image, pixel), delta_) & 1) != 0);
        }
    }

  private:
    int delta_;
};

}  // namespace

std::unique_ptr<Codec> make_codec(Mode mode, uint8_t param) {
    switch (mode) {
        case Mode::Binary:
            return std::make_unique<BinaryCodec>();
        case Mode::Raw:
            // param 0 is what raw wrote before it had a bit depth, and it means the
            // full 8 bits — byte-for-byte the same pixels, so old carriers still read.
            return std::make_unique<RawCodec>(param == 0 ? 8 : param);
        case Mode::Lsb:
            return std::make_unique<LsbCodec>(param);
        case Mode::Qim:
            return std::make_unique<QimCodec>(param);
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
    for (uint8_t k = 1; k <= 8; ++k) {
        candidates.emplace_back(Mode::Raw, k);
    }
    for (uint8_t k = 1; k <= 4; ++k) {
        candidates.emplace_back(Mode::Lsb, k);
    }
    for (auto delta = static_cast<uint8_t>(kLeastDelta); delta <= kMostDelta; ++delta) {
        candidates.emplace_back(Mode::Qim, delta);
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
