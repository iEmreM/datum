# `qim` — measured against real re-encoders

Every mode before this one needs a lossless carrier. `qim` is the first that does not:
it moves a pixel by *more* than a codec's quantiser step and reads the bit back out of
the rounding, so a re-encode that only nudges values still decodes to the same bit.

```
embed:    y  = luma(R, G, B)                      # BT.601, integer weights summing to 256
          y' = nearest multiple of delta whose index has the parity of the bit
          d  = y' - y ;  R += d, G += d, B += d   # the same offset for all three
extract:  bit = round(luma(R, G, B) / delta) & 1
```

`delta` is the robustness dial and the only parameter. A bit survives anything that
moves the luma by less than `delta/2` and nothing that moves it further. Everything
below is a measurement of where that line actually falls.

## Why the whole pixel moves together

The obvious design — quantise R, G and B independently, three bits per pixel — was
built first and measured first. It does not work, for exactly the reason
[`docs/RAW_BITS.md`](RAW_BITS.md) found for `raw`: **JPEG and H.264 store chroma at
quarter resolution.** Independent per-channel data *is* chroma, and 4:2:0 averages it
over each 2×2 block before the quantiser is even reached. No margin defends against a
value being replaced by the mean of its neighbours.

Shifting all three channels by the same amount leaves every channel *difference*
untouched, so the colour is unchanged and 4:2:0 has nothing of ours to average away.
BT.601 luma of a decoded pixel is then the luma the codec kept — that is what the
YCbCr transform is, so chroma damage cancels out of the extraction exactly.

Same cover, same 5% fill, `delta = 32`, BER after each recipe:

| carrier | capacity @640×480 | jpeg 95 | jpeg 85 | h264 crf18 | h264 crf23 |
|---|---:|---:|---:|---:|---:|
| per channel, 3 bits/pixel | 115 KB | 0.0004 | 0.3129 | 0.2655 | 0.3458 |
| whole pixel, 1 bit/pixel | 38 KB | **0.0000** | 0.0059 | 0.0010 | 0.0437 |

The per-channel carrier holds three times as much and never returned a clean CRC —
not at any delta, on any recipe, at any fill. Capacity is therefore **one bit per
pixel**: 259 KB at 1920×1080, the same as `binary`. The roadmap's original 778 KB
belonged to a design that does not survive its own purpose.

## The sweep

`datum_robustness` embeds, re-encodes, extracts and reports the bit error rate.
640×480 `testsrc2` cover, payload filling 25% of capacity, ffmpeg 9.0.1, JPEG written
by stb at the stated quality. Cells are BER; **bold** is a clean CRC, i.e. the file
came back byte for byte. PSNR and SSIM are the cost of embedding, measured against the
cover before any re-encode.

| delta | PSNR | SSIM | jpeg 95 | jpeg 85 | jpeg 75 | h264 crf18 | h264 crf23 | h264 crf28 | h264 4:4:4 crf23 | half scale |
|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| 2 | 47.1 | 0.996 | 0.4184 | 0.4764 | 0.4944 | 0.4962 | 0.4979 | 0.4975 | 0.5018 | 0.4338 |
| 4 | 41.1 | 0.986 | 0.2020 | 0.3589 | 0.4149 | 0.4263 | 0.4968 | 0.5011 | 0.4955 | 0.3664 |
| 6 | 37.5 | 0.970 | 0.1192 | 0.3081 | 0.3574 | 0.2942 | 0.4915 | 0.4967 | 0.4834 | 0.3443 |
| 8 | 35.0 | 0.951 | 0.0474 | 0.2609 | 0.3257 | 0.1972 | 0.4495 | 0.4966 | 0.4553 | 0.3370 |
| 12 | 31.2 | 0.914 | 0.0040 | 0.1754 | 0.2639 | 0.1066 | 0.3261 | 0.4969 | 0.3297 | 0.3272 |
| 16 | 28.6 | 0.881 | 0.0001 | 0.1119 | 0.2165 | 0.0471 | 0.2349 | 0.4843 | 0.2385 | 0.3229 |
| 20 | 26.9 | 0.856 | **0.0000** | 0.0694 | 0.1767 | 0.0163 | 0.1484 | 0.4434 | 0.1702 | 0.3194 |
| 24 | 25.5 | 0.839 | **0.0000** | 0.0328 | 0.1339 | 0.0065 | 0.1049 | 0.3988 | 0.1347 | 0.3145 |
| 26 | 24.9 | 0.830 | **0.0000** | 0.0245 | 0.1214 | 0.0030 | 0.0710 | 0.3391 | 0.1071 | 0.3154 |
| **28** | 23.6 | 0.823 | **0.0000** | 0.0140 | 0.1000 | 0.0023 | 0.0539 | 0.2953 | 0.0820 | 0.3158 |
| 32 | 23.4 | 0.814 | **0.0000** | 0.0061 | 0.0731 | 0.0003 | 0.0359 | 0.2701 | 0.0547 | 0.3138 |

Four things fall out of it.

**How full the carrier is barely matters.** The same sweep at 5% and at 100% fill
moves every BER cell by less than 0.005. A codec damages the region it is given, and
whether the rest of the frame was left alone does not change what happens there. Only
PSNR and SSIM move, because they average over the whole image: at 5% fill `delta = 28`
reads 30.7 dB and 0.966 instead of 23.6 dB and 0.823.

**Chroma stopped mattering, which is the design working.** The `h264 4:4:4 crf23`
column is the control — leaving chroma at full resolution changes the 4:2:0 result by
a few hundredths. For the per-channel carrier it changed everything.

**A rescale is the wall.** The `half scale` column uses a *lossless* codec, so the only
thing it measures is the resampling — and it sits at 0.31–0.43 for every delta, which
is what guessing scores. Averaging neighbouring pixels replaces a value instead of
nudging it, and a margin is no defence against a replacement. This is the documented
limit of every spatial mode, and the reason Phase 6 moves into the DCT domain.

**BER alone does not decide anything.** A CRC needs every bit, so 0.0001 and 0.5 are
the same verdict. The bold cells are the only rows that mean "the file came back".

## Picking the default

Two JPEG encoders disagree about what a given quality number costs, so the default was
chosen against both. `delta` at which a 1.9 KB payload came back byte for byte through
ffmpeg's `mjpeg` at `-q:v 1` and `-q:v 2`, over four covers:

| delta | testsrc2 | mandelbrot | gradients | smptehdbars |
|---:|:--:|:--:|:--:|:--:|
| 24 | — | ✅ | — | — |
| 26 | ✅ | ✅ | ✅ | ✅ |
| 28 | ✅ | ✅ | ✅ | ✅ |

and through H.264 at the same resolution:

| delta | crf 8 | crf 12 | crf 15 | crf 18 | crf 20 | crf 23 |
|---:|:--:|:--:|:--:|:--:|:--:|:--:|
| 16 | ✅ | — | — | — | — | — |
| 20 | ✅ | — | — | — | — | — |
| 24 | ✅ | ✅ | — | — | — | — |
| 26 | ✅ | ✅ | — | — | — | — |
| 28 | ✅ | ✅ | ✅ | — | — | — |
| 32 | ✅ | ✅ | ✅ | — | — | — |

**The default is `delta = 28`.** 26 is where every cover first came back clean, and 24
already fails on three covers out of four — the boundary is plainly cover-dependent, so
the default sits one step above it rather than on it. 28 is also the smallest step that
reaches H.264 CRF 15, and 32 buys nothing beyond it but banding.

## Where H.264 actually stops

CRF 23 does not survive at any delta in range, so it is worth saying why rather than
only that. The problem is not the margin. Neighbouring pixels get independent offsets,
so the carrier *is* high-frequency noise, and that is the first thing a
rate-distortion encoder spends its budget removing — which is why CRF 18 is two orders
of magnitude better than CRF 23 at the same delta while the margin is identical.

It is not an encoder-tuning problem either. Turning off psychovisual optimisation
(`-x264-params psy-rd=0:aq-mode=0`) and forcing full-range conversion move the CRF 23
failure from "no header found" to "CRC mismatch" and no further. Closing the gap needs
redundancy rather than a bigger step, and that is Phase 6: Reed–Solomon over the
payload, one bit per block instead of per pixel, and the same bit repeated across
frames.

The roadmap expected `delta = 16` to clear "moderate JPEG and H.264". Half of that is
right — high-quality JPEG is comfortable and near-lossless H.264 is reachable at a
large step; delivery-grade H.264 is not.

## The visible cost

`delta = 28` on a filled carrier is 23.6 dB and SSIM 0.823. That is not an invisible
mode and it is not meant to be: flat regions band, because every pixel in them is
pulled to the nearer of two grey levels 56 apart. PSNR understates it — the samples do
not move very far — and SSIM is the number that notices, which is why the harness
reports both.

The practical rule is the same one that applies everywhere else here: **do not fill the
carrier.** The same payload at a fifth of that fill reads 30.7 dB and 0.966, and the
banding stays inside the region it occupies.

`datum analyze` reads a `qim` stego as clean, and that means nothing at all. Both
attacks it runs hunt the LSB *replacement* signature, which `qim` does not produce.
`qim` is a **visible** mode; concealment is `lsb`'s job.

## Reproducing

```sh
cmake --build build --target datum_robustness
build/datum_robustness --fill 0.25              # generated cover, no media needed
build/datum_robustness -i photo.png --fill 0.1
```

By hand — note that the stego is written as PNG, because `datum` still refuses to
write a lossy file itself. The re-encode is something you do to it afterwards, which
is the whole scenario this mode is for:

```sh
datum embed -i cover.png -o stego.png -d secret.zip --mode qim
ffmpeg -i stego.png -q:v 2 stego.jpg
datum extract -i stego.jpg -o back.zip          # CRC ok
```
