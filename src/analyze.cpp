#include "datum/analyze.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>

namespace datum {
namespace {

std::size_t sample_index(const Image& image, std::size_t pixel, int channel) {
    return pixel * static_cast<std::size_t>(image.channels) + static_cast<std::size_t>(channel);
}

/// P(X² >= x) with `degrees` degrees of freedom, through the Wilson–Hilferty
/// cube-root transform to a standard normal.
///
/// ponytail: the exact answer is the regularised incomplete gamma function — a
/// series plus a continued fraction, about 40 lines. Wilson–Hilferty is within
/// ~1e-3 for df >= 5 and this test always has dozens of value pairs. Swap it in if
/// a borderline p-value ever has to be defended to three decimals.
double chi_square_tail(double x, int degrees) {
    const double df = degrees;
    const double z = (std::cbrt(x / df) - (1.0 - 2.0 / (9.0 * df))) / std::sqrt(2.0 / (9.0 * df));
    return 0.5 * std::erfc(z / std::sqrt(2.0));
}

/// Scores one already-accumulated histogram.
ChiSquare score(const std::array<double, 256>& histogram) {
    ChiSquare result;
    for (int pair = 0; pair < 128; ++pair) {
        const double expected = (histogram[2 * pair] + histogram[2 * pair + 1]) / 2.0;
        // Westfeld's threshold: under it the chi-square approximation breaks down,
        // and an empty pair would otherwise be a division by zero.
        if (expected < 4.0) {
            continue;
        }
        const double delta = histogram[2 * pair] - expected;
        result.statistic += delta * delta / expected;
        ++result.pairs;
    }
    // A fully embedded plane lands near half the degrees of freedom rather than at
    // them: the denominator uses the pair total where the true variance is half it.
    // That is the published behaviour, and it is why p reads ~1 and not ~0.5.
    result.p_embedded =
        result.pairs > 1 ? chi_square_tail(result.statistic, result.pairs - 1) : 0.0;
    return result;
}

/// Scans growing prefixes of the image and keeps the strongest reading.
///
/// This is the attack as Westfeld wrote it, and the reason matters here: our own
/// embedding still fills the image in raster order, so a small payload leaves a
/// signature in the first few per cent and nothing after it. Averaged over the
/// whole image that signature vanishes — a 10%-filled stego reads as clean. The
/// prefix scan is what sees it, so `analyze` can see the weakness we know we left in.
ChiSquare chi_square(const Image& image) {
    constexpr int kSteps = 32;
    const int colors = image.color_channels();

    std::array<double, 256> histogram{};
    ChiSquare best;
    ChiSquare whole;
    std::size_t pixel = 0;
    for (int step = 1; step <= kSteps; ++step) {
        const std::size_t upto = image.pixel_count() * static_cast<std::size_t>(step) / kSteps;
        for (; pixel < upto; ++pixel) {
            for (int channel = 0; channel < colors; ++channel) {
                histogram[image.pixels[sample_index(image, pixel, channel)]] += 1.0;
            }
        }
        ChiSquare here = score(histogram);
        here.prefix = static_cast<double>(step) / kSteps;
        whole = here;  // the last step is the whole image
        if (here.p_embedded > best.p_embedded) {
            best = here;
        }
    }
    // Strictly greater above, so the *earliest* peak wins — that is where the
    // payload ends. When nothing ever rose above zero there is no peak to report,
    // and the whole-image reading is the honest answer.
    return best.pairs > 0 ? best : whole;
}

/// The mask that picks which samples of a block get flipped.
constexpr std::array<int, 4> kMask = {0, 1, 1, 0};

int noisiness(const std::array<int, 4>& block) {
    return std::abs(block[1] - block[0]) + std::abs(block[2] - block[1]) +
           std::abs(block[3] - block[2]);
}

struct Groups {
    double regular = 0.0;
    double singular = 0.0;
    double total = 0.0;

    double difference() const {
        return total == 0.0 ? 0.0 : (regular - singular) / total;
    }
};

/// Counts regular and singular groups over blocks of four horizontally adjacent
/// samples of one channel.
///
/// `shift` picks the flipping function: 0 is F1 (0<->1, 2<->3, ...), 1 is F-1, its
/// dual. F-1 sends 0 to -1 and 255 to 256; both stay inside `int` and are only ever
/// fed to a difference, so letting them stray beats special-casing the two ends.
///
/// `invert` flips every low bit first, which is the p = 1 end of the curve the
/// estimate is fitted through.
Groups count_groups(const Image& image, int shift, bool invert) {
    const int colors = image.color_channels();
    const auto stride = static_cast<std::size_t>(image.channels);
    Groups groups;
    std::array<int, 4> block{};
    std::array<int, 4> flipped{};

    for (int channel = 0; channel < colors; ++channel) {
        for (int y = 0; y < image.height; ++y) {
            const auto row = static_cast<std::size_t>(y) * static_cast<std::size_t>(image.width);
            for (int x = 0; x + 4 <= image.width; x += 4) {
                for (int j = 0; j < 4; ++j) {
                    int value = image.pixels[(row + static_cast<std::size_t>(x + j)) * stride +
                                             static_cast<std::size_t>(channel)];
                    if (invert) {
                        value ^= 1;
                    }
                    block[j] = value;
                    flipped[j] = kMask[j] != 0 ? ((value + shift) ^ 1) - shift : value;
                }
                const int before = noisiness(block);
                const int after = noisiness(flipped);
                if (after > before) {
                    groups.regular += 1.0;
                } else if (after < before) {
                    groups.singular += 1.0;
                }
                groups.total += 1.0;
            }
        }
    }
    return groups;
}

RsAnalysis rs_analysis(const Image& image) {
    RsAnalysis result;
    const Groups m0 = count_groups(image, 0, false);
    if (m0.total == 0.0) {
        return result;  // fewer than four samples in a row: nothing to group
    }
    const Groups n0 = count_groups(image, 1, false);
    const Groups m1 = count_groups(image, 0, true);
    const Groups n1 = count_groups(image, 1, true);

    result.regular = m0.regular / m0.total;
    result.singular = m0.singular / m0.total;
    result.regular_flipped = n0.regular / n0.total;
    result.singular_flipped = n0.singular / n0.total;

    // R(p) and S(p) are parabolas; the four measurements above pin them at p = 0 and
    // p = 1, and the payload rate is where the R and S curves would meet.
    const double d0 = m0.difference();
    const double e0 = n0.difference();
    const double d1 = m1.difference();
    const double e1 = n1.difference();

    const double a = 2.0 * (d1 + d0);
    const double b = e0 - e1 - d1 - 3.0 * d0;
    const double c = d0 - e0;

    double x = 0.0;
    if (std::abs(a) < 1e-12) {
        if (std::abs(b) < 1e-12) {
            return result;  // both curves flat — a noise image, where RS says nothing
        }
        x = -c / b;
    } else {
        const double discriminant = b * b - 4.0 * a * c;
        if (discriminant < 0.0) {
            return result;
        }
        const double root = std::sqrt(discriminant);
        const double first = (-b + root) / (2.0 * a);
        const double second = (-b - root) / (2.0 * a);
        x = std::abs(first) <= std::abs(second) ? first : second;
    }

    if (std::abs(x - 0.5) < 1e-12) {
        return result;
    }
    result.rate = std::clamp(x / (x - 0.5), 0.0, 1.0);
    return result;
}

}  // namespace

Analysis analyze(const Image& image) {
    return Analysis{chi_square(image), rs_analysis(image)};
}

}  // namespace datum
