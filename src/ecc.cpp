#include "datum/ecc.hpp"

#include <algorithm>
#include <array>

namespace datum {
namespace {

using Poly = std::vector<uint8_t>;  ///< coefficients, highest degree first

/// GF(2^8) built on x^8 + x^4 + x^3 + x^2 + 1 (0x11D) with alpha = 2 — the field
/// QR codes, CCSDS and every other byte-oriented Reed-Solomon code use.
///
/// Multiplication is a log-add-antilog, and the exponent table is stored twice so
/// the sum of two logs (at most 508) never needs a modulo.
struct Field {
    std::array<uint8_t, 512> exp{};
    std::array<uint8_t, 256> log{};

    Field() {
        int value = 1;
        for (int i = 0; i < 255; ++i) {
            exp[static_cast<std::size_t>(i)] = static_cast<uint8_t>(value);
            exp[static_cast<std::size_t>(i) + 255] = static_cast<uint8_t>(value);
            log[static_cast<std::size_t>(value)] = static_cast<uint8_t>(i);
            value <<= 1;
            if ((value & 0x100) != 0) {
                value ^= 0x11D;
            }
        }
    }
};

const Field& field() {
    static const Field instance;
    return instance;
}

uint8_t mul(uint8_t a, uint8_t b) {
    if (a == 0 || b == 0) {
        return 0;
    }
    const Field& f = field();
    return f.exp[static_cast<std::size_t>(f.log[a]) + f.log[b]];
}

uint8_t inverse(uint8_t a) {
    const Field& f = field();
    return f.exp[255u - f.log[a]];
}

uint8_t divide(uint8_t a, uint8_t b) {
    if (a == 0) {
        return 0;
    }
    const Field& f = field();
    return f.exp[static_cast<std::size_t>(f.log[a]) + 255u - f.log[b]];
}

/// alpha^power, for 0 <= power < 255.
uint8_t alpha(std::size_t power) {
    return field().exp[power];
}

Poly poly_scale(const Poly& p, uint8_t factor) {
    Poly out(p.size());
    for (std::size_t i = 0; i < p.size(); ++i) {
        out[i] = mul(p[i], factor);
    }
    return out;
}

/// Adding is XOR, aligned at the *lowest* degree — the end of the array.
Poly poly_add(const Poly& p, const Poly& q) {
    Poly out(std::max(p.size(), q.size()), 0);
    for (std::size_t i = 0; i < p.size(); ++i) {
        out[i + out.size() - p.size()] ^= p[i];
    }
    for (std::size_t i = 0; i < q.size(); ++i) {
        out[i + out.size() - q.size()] ^= q[i];
    }
    return out;
}

Poly poly_mul(const Poly& p, const Poly& q) {
    Poly out(p.size() + q.size() - 1, 0);
    for (std::size_t i = 0; i < p.size(); ++i) {
        for (std::size_t j = 0; j < q.size(); ++j) {
            out[i + j] ^= mul(p[i], q[j]);
        }
    }
    return out;
}

uint8_t poly_eval(const Poly& p, uint8_t x) {
    uint8_t y = p[0];
    for (std::size_t i = 1; i < p.size(); ++i) {
        y = static_cast<uint8_t>(mul(y, x) ^ p[i]);
    }
    return y;
}

/// g(x) = product of (x - alpha^i) for i in [0, parity). Its roots are what the
/// syndromes test for, so the same convention has to hold on both sides.
const Poly& generator() {
    static const Poly gen = [] {
        Poly g{1};
        for (std::size_t i = 0; i < kEccParity; ++i) {
            g = poly_mul(g, Poly{1, alpha(i)});
        }
        return g;
    }();
    return gen;
}

/// Systematic encoding: the data comes through untouched and the remainder of
/// data * x^parity divided by g(x) is appended.
Poly encode_block(std::span<const uint8_t> data) {
    const Poly& gen = generator();
    Poly out(data.size() + kEccParity, 0);
    std::copy(data.begin(), data.end(), out.begin());
    for (std::size_t i = 0; i < data.size(); ++i) {
        const uint8_t coefficient = out[i];
        if (coefficient == 0) {
            continue;
        }
        for (std::size_t j = 1; j < gen.size(); ++j) {
            out[i + j] ^= mul(gen[j], coefficient);
        }
    }
    // The division above overwrote the data half with its own working state.
    std::copy(data.begin(), data.end(), out.begin());
    return out;
}

/// S_i = r(alpha^i). All zero means the block is already right. The leading zero
/// keeps the result usable as a polynomial in the same highest-degree-first form
/// as everything else here.
Poly syndromes(const Poly& code) {
    Poly out(kEccParity + 1, 0);
    for (std::size_t i = 0; i < kEccParity; ++i) {
        out[i + 1] = poly_eval(code, alpha(i));
    }
    return out;
}

bool is_clean(const Poly& synd) {
    return std::all_of(synd.begin(), synd.end(), [](uint8_t s) { return s == 0; });
}

/// Berlekamp-Massey: the shortest shift register that generates the syndrome
/// sequence is the error locator polynomial, whose roots say *where* the errors are.
std::optional<Poly> error_locator(const Poly& synd) {
    Poly locator{1};
    Poly previous{1};
    for (std::size_t i = 0; i < kEccParity; ++i) {
        const std::size_t k = i + synd.size() - kEccParity;
        uint8_t delta = synd[k];
        for (std::size_t j = 1; j < locator.size(); ++j) {
            delta ^= mul(locator[locator.size() - 1 - j], synd[k - j]);
        }
        previous.push_back(0);
        if (delta == 0) {
            continue;
        }
        if (previous.size() > locator.size()) {
            Poly scaled = poly_scale(previous, delta);
            previous = poly_scale(locator, inverse(delta));
            locator = std::move(scaled);
        }
        locator = poly_add(locator, poly_scale(previous, delta));
    }

    std::size_t lead = 0;
    while (lead < locator.size() && locator[lead] == 0) {
        ++lead;
    }
    locator.erase(locator.begin(), locator.begin() + static_cast<std::ptrdiff_t>(lead));
    // More errors than half the parity bytes is past the correction limit, and
    // guessing past it is how a decoder returns confident nonsense.
    if (locator.empty() || (locator.size() - 1) * 2 > kEccParity) {
        return std::nullopt;
    }
    return locator;
}

/// Chien search: try every position and keep the ones where the locator vanishes.
/// Finding fewer roots than its degree means the locator does not describe a real
/// error pattern, which is the other way a block can be too damaged.
///
/// The locator is reversed first, and that is not cosmetic: Lambda's roots are the
/// *inverses* of the error positions, and reversing a polynomial inverts its roots.
/// Searching the un-reversed one finds the same count of roots at the mirrored
/// places, so the decode fails silently rather than loudly.
std::optional<std::vector<std::size_t>> error_positions(const Poly& locator, std::size_t length) {
    const Poly reversed(locator.rbegin(), locator.rend());
    std::vector<std::size_t> found;
    for (std::size_t i = 0; i < length; ++i) {
        if (poly_eval(reversed, alpha(i)) == 0) {
            found.push_back(i);
        }
    }
    return found.size() == locator.size() - 1 ? std::optional(found) : std::nullopt;
}

/// Forney: the magnitude at each located position, from the error evaluator
/// Omega(x) = S(x) * Lambda(x) mod x^errors over the locator's derivative.
void correct(Poly& code,
             const Poly& synd,
             const Poly& locator,
             const std::vector<std::size_t>& positions) {
    const Poly reversed(synd.rbegin(), synd.rend());
    const Poly product = poly_mul(reversed, locator);
    const Poly evaluator(product.end() - static_cast<std::ptrdiff_t>(positions.size()) - 1,
                         product.end());

    for (std::size_t i = 0; i < positions.size(); ++i) {
        const uint8_t x = alpha(positions[i]);
        const uint8_t x_inverse = inverse(x);

        // Lambda'(X^-1) as a product over the other roots rather than a formal
        // derivative: in a field of characteristic 2 the doubled terms cancel and
        // this is what is left.
        uint8_t derivative = 1;
        for (std::size_t j = 0; j < positions.size(); ++j) {
            if (j != i) {
                const auto term = static_cast<uint8_t>(1u ^ mul(x_inverse, alpha(positions[j])));
                derivative = mul(derivative, term);
            }
        }
        const uint8_t magnitude = divide(mul(x, poly_eval(evaluator, x_inverse)), derivative);
        code[code.size() - 1 - positions[i]] ^= magnitude;
    }
}

/// One block in, one block out, or nullopt when it is past saving.
std::optional<Poly> decode_block(std::span<const uint8_t> block) {
    Poly code(block.begin(), block.end());
    const Poly synd = syndromes(code);
    if (is_clean(synd)) {
        return code;
    }

    const std::optional<Poly> locator = error_locator(synd);
    if (!locator) {
        return std::nullopt;
    }
    const std::optional<std::vector<std::size_t>> positions =
        error_positions(*locator, code.size());
    if (!positions) {
        return std::nullopt;
    }
    correct(code, synd, *locator, *positions);

    // A locator can describe a consistent-looking pattern that is not the one that
    // actually happened. Re-running the syndromes is the only way to tell, and
    // handing back a miscorrected block would be worse than reporting failure.
    if (!is_clean(syndromes(code))) {
        return std::nullopt;
    }
    return code;
}

}  // namespace

std::size_t ecc_encoded_size(std::size_t data_size) {
    const std::size_t blocks = (data_size + kEccData - 1) / kEccData;
    return data_size + blocks * kEccParity;
}

std::vector<uint8_t> ecc_encode(std::span<const uint8_t> data) {
    std::vector<uint8_t> out;
    out.reserve(ecc_encoded_size(data.size()));
    for (std::size_t at = 0; at < data.size(); at += kEccData) {
        const Poly block = encode_block(data.subspan(at, std::min(kEccData, data.size() - at)));
        out.insert(out.end(), block.begin(), block.end());
    }
    return out;
}

std::optional<std::vector<uint8_t>> ecc_decode(std::span<const uint8_t> code,
                                               std::size_t data_size) {
    if (code.size() < ecc_encoded_size(data_size)) {
        return std::nullopt;
    }

    std::vector<uint8_t> out;
    out.reserve(data_size);
    std::size_t at = 0;
    for (std::size_t done = 0; done < data_size; done += kEccData) {
        const std::size_t chunk = std::min(kEccData, data_size - done);
        const std::optional<Poly> block = decode_block(code.subspan(at, chunk + kEccParity));
        if (!block) {
            return std::nullopt;
        }
        out.insert(out.end(), block->begin(), block->begin() + static_cast<std::ptrdiff_t>(chunk));
        at += chunk + kEccParity;
    }
    return out;
}

}  // namespace datum
