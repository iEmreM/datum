#pragma once

#include "datum/image.hpp"

namespace datum {

/// The chi-square attack (Westfeld & Pfitzmann). LSB *replacement* pushes the
/// counts of each value pair (2i, 2i+1) toward each other; no natural image does
/// that, so an equalised histogram is the signature of a payload.
struct ChiSquare {
    double statistic = 0.0;   ///< sum over the pairs; near half `pairs` means equalised
    int pairs = 0;            ///< value pairs with enough samples to test
    double p_embedded = 0.0;  ///< 1 = the pairs are equalised, 0 = they are not
    double prefix = 0.0;      ///< share of the image, from the top, that scored highest
};

/// RS analysis (Fridrich, Goljan & Du). Estimates what share of the low bits
/// carry a payload from how blocks of neighbouring samples react to a flipping
/// function. More sensitive than chi-square at low fill, but it assumes a natural
/// image: on pure noise there is no neighbour correlation to measure and the
/// estimate means nothing.
struct RsAnalysis {
    double regular = 0.0;           ///< R_M,  share of blocks made noisier by the flip
    double singular = 0.0;          ///< S_M,  share made smoother
    double regular_flipped = 0.0;   ///< R_-M, the same under the dual flip
    double singular_flipped = 0.0;  ///< S_-M
    double rate = 0.0;              ///< estimated share of low bits carrying payload, 0..1
};

struct Analysis {
    ChiSquare chi;
    RsAnalysis rs;
};

/// Runs both tests over the image's colour channels. The alpha channel is skipped
/// for the same reason no codec writes there.
///
/// This exists to be aimed at our own output: "undetectable" is a claim, and this
/// is the measurement that either backs it or does not.
Analysis analyze(const Image& image);

}  // namespace datum
