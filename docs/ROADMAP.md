# datum — Technical Roadmap

> Repo language policy: code, comments and docs in English. Issue/PR discussion may be Turkish.

This file is the project's memory: what is built, why it was built that way, and what is
still ahead. The user-facing guide is [`README.md`](../README.md); measurements live in
[`QIM.md`](QIM.md), [`DCT.md`](DCT.md) and [`RAW_BITS.md`](RAW_BITS.md).

## Status at a glance (2026-09-16)

| Phase | Scope | Status |
|---|---|---|
| 0 | Build skeleton, image I/O, CI | ✅ |
| 1 | `binary` + `raw` modes, DTM1 container, CRC | ✅ |
| 2 | `lsb` mode, capacity and PSNR reporting | ✅ |
| 3 | Video via ffmpeg pipes, payload spanning frames | ✅ |
| 4 | LSB matching, `datum analyze` steganalysis | ✅ |
| 4b | Keyed scatter and payload whitening (`--key`) | ☐ next — blocked on a format decision |
| 5 | `qim` mode, robustness harness | ✅ |
| 6 | `dct` mode, Reed–Solomon, temporal repetition | ✅ (real YouTube upload not run) |
| 7 | Parallelism and profiling | ☐ |

**Next up, in order:** settle the Phase 4b header question (§6, Phase 4b), build 4b,
then Phase 7. Everything else that is open — limitations, tech debt, stretch goals — is
collected in [§11](#11-open-work-and-backlog).

## 0. Problem statement

Embed arbitrary byte payloads **inside pixel values** of still images and video
frames (not appended to the file tail), extract them losslessly, and progressively
harden the channel until it survives a YouTube re-encode.

### Terminology

| Term | Meaning |
|---|---|
| **cover** | Original, unmodified image/video |
| **payload** | Bytes to hide |
| **stego** | Cover after embedding |
| **capacity** | Payload bytes a cover can hold in a given mode |
| **BER** | Bit error rate after a round trip |
| **bpp** | Bits embedded per pixel |

### The trade-off triangle

**Capacity ↔ Imperceptibility ↔ Robustness — pick two.**
Every mode below sits at a different corner of this triangle. No mode is
simultaneously high-capacity, invisible, and YouTube-proof; the roadmap builds
each corner separately and lets the CLI pick.

---

## 1. Embedding modes (levels)

| ID | Mode | Domain | Capacity @1920x1080 | Visible? | Survives re-encode? |
|---|---|---|---|---|---|
| L0 | `binary` — pixel is black(0)/white(1), R=G=B | spatial | 259 KB (1 bpp) | Yes, fully | JPEG yes (spot check); H.264 and rescale no |
| L1 | `raw --bits k` — top k bits of each of R,G,B, centred | spatial | 6.22 MB × k/8 | Yes, image becomes noise | k≤2: JPEG yes; H.264 no |
| L2 | `lsb --bits k` — nudge k low bits of each channel (matching) | spatial | k × 777 KB | k≤2: no | No |
| L3 | any mode applied per video frame | spatial | above × frame count | per mode | per mode |
| L4 | `lsb` + keyed scatter + whitening | spatial | ≈ L2 | No | No — **planned, Phase 4b** |
| L5 | `qim` — quantise a pixel's luma to a step Δ, bit = bin parity | spatial | 259 KB (1 bpp) | Yes, it bands | **Partly — high-quality JPEG, near-lossless H.264** |
| L6 | `dct` — DCT coefficient relations on luma + ECC | frequency | 3.4 KB/frame | Only close up | **Yes** — including a rescale |

L0 was originally planned with *block averaging* — one bit painted over a whole block,
which is what would buy robustness against scaling. That variant was never built:
`binary` paints one pixel per bit. A spot check on a generated 1080p cover (2026-09-16,
not part of the harness) found it survives ffmpeg JPEG at `-q:v 2` and `-q:v 10`, and
fails H.264 CRF 23/28 and a `3/4` rescale. L6 is the block idea done properly — the bit
is spread over an 8×8 block, but as a relation between two frequency components rather
than as a flat patch of black or white, so the picture survives it too.

### Capacity math (reference)

1920×1080 = 2,073,600 pixels.

```
L0: 2,073,600 bits                     = 259.2 KB
L1: 2,073,600 × 3 bytes                =   6.22 MB
L2: 2,073,600 × 3 × k bits, k=1        = 777.6 KB   (k=2 -> 1.52 MB)
L5: 2,073,600 bits                     = 259.2 KB   (one bit per pixel, not per channel)
L6: (1920/8) × (1080/8) = 32,400 blocks/frame
    1 bit/block                        = 4050 bytes/frame raw
    minus the header codeword and RS   = 3490 payload bytes/frame
    spanning frames                    = 3490 × frames  (no redundancy)
    --repeat N                         = 3490 bytes total, whatever the length
```

Phase 6 measured the block size rather than assuming it: 8×8 was built first, and it
clears every rescale in the harness, so the 16×16 blocks the original estimate assumed
were never needed. What `--repeat` spends frames on is redundancy, not more payload — which is
the trade the mode exists for.

### Distortion per mode (theoretical PSNR, uniform random payload)

Since Phase 4 the `lsb` codec *matches* rather than replaces: it moves a channel to
the nearest value carrying the right bits, which halves the worst-case error at
k > 1. Both columns are measured on a fully filled random cover.

| Mode | Max per-channel error | MSE | PSNR | (replacement, before Phase 4) |
|---|---|---|---|---|
| L2 k=1 | 1 | 0.50 | 51.1 dB | 51.1 dB |
| L2 k=2 | 2 | 1.51 | 46.3 dB | 44.2 dB |
| L2 k=3 | 4 | 5.65 | 40.6 dB | 37.9 dB |
| L2 k=4 | 8 | 22.81 | 34.6 dB | 31.8 dB |

Rule of thumb: **>45 dB is invisible, <35 dB is obvious.** Ship k=1 as the default
and gate k≥3 behind an explicit flag.

---

## 2. Design decisions to settle before writing code

### 2.1 Two different jobs: the LSB for capacity, QIM for robustness

There are two amplitudes on the table and they are not competing — they answer
different questions.

**For invisibility and capacity, use the binary LSB.** `value & 1`
(`230 -> 230/231`) costs at most **±1** per channel. The decimal variant
("last digit >5 means 1") costs up to **±9** for the same single bit — 19 dB worse
PSNR at identical capacity. Whenever the container is lossless, the bit wins.

**For robustness, amplitude is exactly the point.** A change smaller than the
codec quantiser step is erased by definition; a change larger than it survives. That
is what the decimal-digit idea is really describing, and its proper name is
**Quantization Index Modulation** (QIM):

```
embed:   q = round(v / Δ);  if ((q & 1) != bit) q += 1;  v' = clamp(q * Δ)
extract: bit = round(v / Δ) & 1
```

With Δ = 10 this *is* the "last digit" scheme, generalised: the bit is the parity of
the quantisation bin, and any distortion below Δ/2 decodes to the same bin. Δ is a
direct robustness dial — Δ = 2 degenerates to the LSB, Δ = 28 survives a high-quality
JPEG at the cost of visible banding in flat regions.

Phase 5 changed one thing about this: the quantised value is a pixel's **luma**, not
each channel on its own, and the offset that lands it on a bin is applied to R, G and B
together. Independent per-channel data is chroma, and 4:2:0 averages chroma away before
the quantiser is ever reached — the same wall `raw` hit. Measurements: docs/QIM.md.

QIM is a **spatial** technique, so it holds only while the pixel grid is intact. It
tolerates re-compression; it does **not** tolerate rescaling, because a resample
filter averages neighbouring pixels and destroys the bin parity. That is the dividing
line between L5 and L6: QIM covers "same resolution, re-encoded", DCT covers
"YouTube did whatever it wanted".

### 2.2 Lossless container or nothing (L0–L4)

JPEG / H.264 / VP9 quantise DCT coefficients and destroy every spatial LSB.
Spatial modes therefore **must** write:

- images → **PNG**, nothing else (`save_png` rejects any other extension)
- video → **FFV1 in MKV**, nothing else (`embed_video` rejects any other extension)

Reading a lossy cover is fine (decode → embed → save lossless). Writing a lossy
stego is silently broken, so the CLI refuses it — for every mode, including `qim` and
`dct`, whose robustness is against a re-encode that happens *after* `datum` has written
a lossless file.

Note that a JPEG which mysteriously became a large PNG is itself a signal — real
JPEG-to-JPEG hiding needs DCT-domain embedding (the jsteg / F5 family), tracked as a
stretch goal in §11.

### 2.3 Naive LSB replacement is detectable

"Almost impossible to detect" is only true with care. Sequential LSB **replacement**
is reliably caught by chi-square, RS analysis and Sample Pair Analysis: it drives the
counts of value pairs (2k, 2k+1) toward equality, a signature that does not occur in
natural images. Phase 4 fixes this with three changes:

1. **LSB matching (±1)** instead of replacement — if the bit does not match, add or
   subtract 1 at random. No pair-count signature. **Shipped in Phase 4.**
2. **Keyed pseudorandom scatter** — embedding order is a permutation derived from the
   passphrase, not raster order. Local statistics stay clean, and an attacker without
   the key cannot read the bits in order even if they extract them. *Phase 4b.*
3. **Payload whitening** — XOR the payload with a keystream so the embedded bits are
   indistinguishable from noise. *Phase 4b.*

`datum analyze` (Phase 4) is how each of these is checked rather than claimed.

Beyond this, the state of the art is **content-adaptive embedding** (HUGO, WOW,
S-UNIWARD): spend bits in textured, noisy regions and avoid flat sky or skin.
Stretch goal.

### 2.4 Never embed in chroma when the target is lossy

4:2:0 subsampling stores U and V at quarter resolution. Anything embedded there is
averaged away before the codec even starts. L5 and L6 both embed in **Y (luma) only**
— L5 by moving all three channels together, which changes luma and leaves every
channel difference alone. Phase 5 measured what happens otherwise: docs/QIM.md.

### 2.5 Windows binary-mode pipes

Frame data moves through `ffmpeg` pipes. On Windows, stdio in text mode rewrites
`0x0A` into `0x0D 0x0A` and silently corrupts every frame. `src/video.cpp` opens the
pipes with `_popen(..., "rb")` / `"wb"`; POSIX `popen` rejects the `b` suffix and its
pipes are binary already, so the mode string is chosen per platform. The test payloads
are generated noise that contains every byte value, `0x0A` and `0x0D` included. This is
the single most likely source of "works on Linux, garbage on Windows" bugs in this
project.

Two more Windows traps live next to it: `cmd.exe` strips the outermost pair of quotes
from a command, so every command whose program path is quoted gets wrapped in a second
pair (`finalize()` in `src/video.cpp`); and an old `ffprobe` from an unrelated package
(seen: a Python `Scripts` directory) can shadow the real one on `PATH`, which is what
`DATUM_FFMPEG` / `DATUM_FFPROBE` exist for.

---

## 3. Payload container format (`DTM1`)

Extraction cannot know where the payload ends without a header, so every mode embeds
this 16-byte header first, MSB-first bit order.

```
offset size field
  0      4   magic        "DTM1"
  4      1   version      = 1
  5      1   mode         0=binary 1=raw 2=lsb 3=qim 4=dct
  6      1   flags        bit0 encrypted*, bit1 has_name*, bit2 ecc, bits 3-7 repeat-1
  7      1   param        mode parameter (raw: k, lsb: k, qim: Δ, dct: margin; binary: 0)
  8      4   payload_len  u32 little-endian
 12      4   payload_crc  CRC-32 (IEEE) of the payload
 16    var   name*        present if flags.has_name: u8 len + bytes

* reserved, not implemented: nothing sets these bits and nothing writes a name
```

What the code does today (`src/container.cpp`, `src/codecs.cpp`):

- `parse_header` accepts a stream only if the magic is `DTM1`, the version is 1 and the
  mode byte is 0–4. Anything else reads as "no payload here".
- The header is written **in plaintext**. Header whitening (below) is Phase 4b.
- `raw` with `param == 0` is read as `k = 8`: that is what `raw` wrote before it had a
  bit depth, and it is byte-for-byte the same pixels, so old carriers still decode.
- Extraction auto-detects by trying `(mode, param)` candidates cheapest first —
  `binary`, `raw` k=0..8, `lsb` k=1..4, `qim` Δ=2..32, then one `dct` probe — and keeps
  the first whose header *agrees* with the codec that read it (same mode, same `param`,
  same ECC flag). `dct` needs one probe only: reading a coefficient comparison does not
  depend on the margin, so the margin comes out of the header.

**`flags` bits 3–7 are the video repeat count, minus one** (1–32 copies). Phase 6
needed somewhere to say how many times the stream was written, and a whole new field
would have cost a format version bump for five bits.

**`param` for `dct` is the coefficient margin, not a block size.** The block size is
fixed at 8×8: the measurements in [`DCT.md`](DCT.md) say it already clears every
rescale in the harness, so making it adjustable would be a dial with one useful
setting. The margin is the dial that actually moves the result.

**When `flags.ecc` is set the stream is two Reed-Solomon codewords**, not a plain
header followed by a plain payload:

```
| 48 bytes: header (16) + parity (32) | payload: 223 data + 32 parity, per block |
```

The header is its own codeword because extraction has to read and trust it before it
knows how long the payload behind it is. The flag is checked against the mode that
claims to have written it, so a header disagreeing with its own framing is rejected the
same way a wrong `param` is.

**Header whitening — planned, Phase 4b, not implemented.** When a key is given, the
header is XOR-ed with the same keystream as the payload. Extraction decrypts, then
checks the magic:

- magic matches → correct key, payload present
- magic mismatch → wrong key **or** no payload — indistinguishable

Without this, a plaintext `DTM1` sitting in the LSB plane is a free detector for anyone
— which is the situation today. Whether *keyless* embedding keeps a plaintext header is
the open question recorded under Phase 4b.

**CRC is mandatory.** It is the only way to tell "extracted correctly" from "extracted
garbage", and it is the pass/fail signal for every robustness test in Phases 5 and 6.

---

## 4. Dependencies — deliberately minimal

| Need | Choice | Why not the alternative |
|---|---|---|
| PNG/JPEG decode, PNG encode | `stb_image.h` + `stb_image_write.h`, vendored in `third_party/stb/` | OpenCV is a huge build for two functions |
| Video I/O | pipe raw frames to/from the `ffmpeg` binary | linking libavcodec on MSYS2 is a build and licensing tax; raw pipes give us every codec ffmpeg has |
| Hashing / keystream (Phase 4b, not built yet) | SHA-256 + ChaCha20, ~150 lines, self-contained | OpenSSL for one keystream is not worth the link |
| ECC (Phase 6) | Reed-Solomon RS(255,223), `src/ecc.cpp` | libfec is unmaintained on Windows |
| Tests | one `tests/roundtrip.cpp` of `CHECK`s, driven by CTest | GoogleTest is a dependency for asserting `a == b` |
| Formatting | `clang-format` (Google-based, 100 columns), checked in CI | — |

Explicit non-dependencies: OpenCV, Qt, Boost, libavcodec, OpenSSL, GoogleTest.

If the DCT phase ever needs *coefficient-level* access without a decode/re-encode
cycle, that is the one place where linking libavcodec becomes justified. Revisit there,
not before.

---

## 5. Repository layout

Grows per phase — nothing is scaffolded before the phase that needs it. This is the
tree as it stands; entries marked *planned* do not exist yet.

```
datum/
├── .github/workflows/ci.yml     # Windows (MSYS2 UCRT64) + Linux build and ctest, clang-format check
├── .clang-format
├── .gitignore
├── .vscode/                     # CMake Tools settings
├── CLAUDE.md                    # working notes for AI-assisted development
├── CMakeLists.txt               # datum_core lib, datum CLI, datum_robustness, datum_tests
├── LICENSE
├── README.md                    # user-facing guide
├── docs/
│   ├── ROADMAP.md               # this file
│   ├── QIM.md                   # delta-vs-BER, and where the default came from
│   ├── DCT.md                   # margin-vs-BER, ECC and temporal repetition
│   ├── RAW_BITS.md              # what --bits buys raw, measured
│   └── FORMAT.md                # planned: DTM1 spec, once Phase 4b settles the format
├── include/datum/
│   ├── image.hpp                # Image + load/save_png, luma, PSNR, SSIM
│   ├── io.hpp                   # whole-file byte I/O (wide-path safe)
│   ├── bitstream.hpp            # BitReader / BitWriter (MSB-first), bit_error_rate
│   ├── container.hpp            # Mode enum, DTM1 header, CRC-32, repeat bits
│   ├── codec.hpp                # Codec interface, parameter limits, embed/detect/extract
│   ├── ecc.hpp                  # Reed-Solomon RS(255,223) over GF(2^8)
│   ├── video.hpp                # ffprobe/ffmpeg pipes, embed_video / extract_video
│   ├── analyze.hpp              # chi-square + RS steganalysis
│   └── crypto.hpp               # planned (4b): SHA-256, ChaCha20, keyed permutation
├── src/                         # one .cpp per header; all five codecs live in codecs.cpp
├── apps/datum.cpp               # CLI: argument parsing and the five subcommands
├── third_party/stb/             # stb_image + stb_image_write, vendored
├── tools/robustness.cpp         # datum_robustness: BER sweep for qim/dct, built but not a test
└── tests/roundtrip.cpp          # every test, one executable, registered with CTest
```

There is no `samples/` directory: every fixture is generated in code or by ffmpeg's
`lavfi` sources, and `samples/local/` is gitignored for anyone keeping media nearby.

### Core interface

Every mode implements the same three operations, so the CLI, the video layer and the
test harness get written once:

```cpp
class Codec {
  public:
    virtual ~Codec() = default;
    virtual std::size_t capacity(const Image&) const = 0;              // bytes, header included
    virtual void embed(Image&, BitReader& source) const = 0;           // consumes bits
    virtual void extract(const Image&, BitWriter& sink, std::size_t bits) const = 0;
};
```

`BitReader` / `BitWriter` are the seam that lets a payload span many video frames
without any codec needing to know frames exist. `extract`'s `bits` is a target for the
whole sink, not a count to append, so calling it frame after frame resumes where the
previous frame stopped.

### CLI surface

What exists today (`apps/datum.cpp`). Every flag takes a value; unknown flags, and
flags that belong to a different mode, are refused rather than ignored.

```
datum info     -i <carrier>
datum capacity -i <carrier> [--mode M] [--bits N | --delta N | --margin N] [--repeat N]
datum embed    -i <cover> -o <stego> -d <payload> [--mode M]
               [--bits N | --delta N | --margin N] [--repeat N]
datum extract  -i <stego> -o <payload> [--mode M]
datum analyze  -i <carrier>
datum help
```

`--mode` defaults to `raw`. `--repeat` is video only. Output must be `.png` for images
and `.mkv` for video. A carrier is a video when its extension is one of `.mp4 .mkv
.mov .avi .webm .m4v .mpg .mpeg .wmv .flv`.

Planned for Phase 4b: `--key PASS` on `embed` and `extract`.

---

## 6. Phases

Each phase ends with a working CLI command, a passing test, and a README update.
A phase is not done until all three exist.

### Phase 0 — Skeleton ✅

- `CMakeLists.txt`: C++20, `-Wall -Wextra -Wpedantic -Werror` (`/W4 /WX` on MSVC),
  `Release` as the default build type, static runtime on MinGW
- Vendor stb; `Image` load/save; `datum info -i x.png`
- CTest wiring; GitHub Actions CI on `windows-latest` (later joined by an Ubuntu job
  and a `clang-format --dry-run --Werror` job)
- **Test:** load PNG → save PNG → byte-identical pixel buffer
- Delete `test.cpp`; move `.vscode` from single-file `cppbuild` to CMake Tools

### Phase 1 — L0 + L1 (visible modes) ✅

- `BitWriter` / `BitReader`, CRC-32, DTM1 header
- `binary` codec: threshold at 128, write 0x00 / 0xFF
- `raw` codec: R, G, B ← three payload bytes
- **Test:** embed random payloads (0, 1 and 4000 bytes for `raw`, 1 and 400 for
  `binary`) into 1–4 channel images → PNG on disk → auto-detected extract →
  byte-identical, CRC passes; one flipped bit must fail the CRC
- Payload-too-large must fail loudly, never truncate
- The alpha channel is never written: changing it changes which pixels are drawn at
  all. `Image::color_channels()` excludes it for every codec and for `analyze`.
- Added later: `raw --bits k` (1–8, default 8) puts the payload in the *top* k bits and
  centres the rest, a ±2^(7−k) drift margin — measured in [`RAW_BITS.md`](RAW_BITS.md)

### Phase 2 — L2 (LSB, invisible) ✅

- `lsb` codec with configurable k (1–4), raster order
- `datum capacity`; report PSNR after every embed
- **Test:** round trip at k = 1..4, plus PSNR(k=1) > 50 dB
- k is carried in the DTM1 header, so `extract` recovers it — no `--bits` needed to
  read; detection tries each k and keeps the one whose header agrees (`param == k`)

### Phase 3 — Video ✅

- `video.hpp`: `ffmpeg -i in -f rawvideo -pix_fmt rgb24 -` in,
  `ffmpeg -f rawvideo ... -c:v ffv1` out
- Stream frame by frame; never hold a whole video in RAM
- Payload spans frames through the shared `BitReader`
- Preserve the audio track (`-c:a copy` from a second input mapping)
- **Test:** 30-frame generated clip, embed 1 MB, extract, CRC passes

Decisions this phase forced, all of them measured rather than assumed:

- **FFV1 must be told `-pix_fmt bgr0`.** It is the only 8-bit RGB format FFV1
  accepts, and it is byte-exact against `rgb24` both ways. Left to itself ffmpeg
  would pick a YUV format and quantise the payload away. Verified by comparing the
  raw frames in and out.
- **`-frames:v N` on the decoder, not an early hang-up.** Extraction stops as soon
  as the payload is complete. Simply closing the pipe kills ffmpeg with a broken
  pipe, which is indistinguishable from a real decode failure — so the decoder is
  asked for exactly the frames the payload spans and exits 0. Extraction therefore
  costs frames-used, not video-length: 4 MB out of a 150-frame clip decodes 12.
- **The header must fit in the first frame**, enforced at embed time. Extraction
  reads frame 0 alone to learn the mode, `k` and length; anything else needs a
  second buffering pass for no practical gain (a 12×12 frame already holds it).
- **`BinaryCodec::extract` counted from zero per call**, which was invisible for
  images (called once) and wrong for video (called per frame). `bits` is now a
  total for the sink across frames, matching the other two codecs.
- **`DATUM_FFMPEG` / `DATUM_FFPROBE` overrides.** Unrelated packages ship old
  ffprobe builds that shadow the real one on `PATH`; a bare name is not always
  enough to find the right binary.
- `detect_codec` / `verify_payload` were lifted out of `extract_payload` so the
  image and video paths share one definition of "whose payload is this" and one
  CRC gate, rather than two that can drift apart.

### Phase 4 — Undetectability (part 1) ✅

Split deliberately. The two items below need no key and change no file format, so
they shipped on their own; the two keyed items are deferred to Phase 4b, where the
whole `--key` surface can be designed once instead of twice.

- LSB **matching** (±1) replaces LSB replacement ✅
- `datum analyze`: chi-square and RS analysis, aimed at our own output ✅
- **Test:** on a photo-like generated cover, the RS estimate for a 10%-filled `lsb`
  stego stays within 0.05 of the clean baseline, while LSB *replacement* at the same
  fill is caught by chi-square ✅ (chi-square is deliberately not asserted against our
  own output — see the last decision below)

Decisions this phase forced:

- **Matching generalises to k > 1 as "nearest congruent value", not "±1".** For k
  bits the two candidates are `sample ± d` and `sample ∓ (2^k − d)`; taking the
  nearer one caps the error at 2^(k−1) instead of 2^k − 1. Measured on a random
  cover: k=2 gains 2.2 dB, k=3 2.7 dB, k=4 2.7 dB. At k=1 the two candidates are
  always equidistant, so the tie-break *is* the whole decision and it has to be
  pseudorandom — that case is classic LSB matching.
- **Extraction did not change at all.** Matching lands on a value with the same low
  k bits, so the decoder, the header, the format and every existing test are
  untouched. This is why it could ship without the keyed half.
- **Chi-square must scan growing prefixes, not the whole image.** Our embedding is
  still raster-order, so a small payload leaves a signature in the first few per
  cent and nothing after it; averaged over the whole image it vanishes. Measured on
  a 4K photo, a 10%-filled replacement stego reads p = 0.000 whole-image and
  p = 0.963 with the prefix scan, which also located the boundary at 9%.
- **RS's rate estimate breaks down at full fill.** At p = 1 the R and S curves
  cross and the quadratic has no root left in range, so the estimate collapses to
  0 exactly when detection is easiest. The unambiguous signal there is that R and S
  met at all, which is what the test asserts.
- **The two tests cover each other's blind spots**, which is why both shipped.
  Chi-square is defeated by matching but catches replacement at any fill; RS is
  blind to replacement at full fill but tracks the payload rate accurately from
  10% to 50%.
- **Matching does not hide a full carrier.** Measured on a 4K photo, RS reads 1.2%
  clean, 4.4% at a tenth filled, 15.6% at half and 31.8% full. The ±1 nudge leaves
  no *replacement* signature, but it is still added noise and a filled carrier
  still reads as changed. The practical rule that falls out: do not fill the
  carrier. Phase 4b's keyed scatter is what spreads a small payload so the
  remaining signal has nowhere to concentrate.
- **Chi-square against matching is cover-dependent.** On real photographs it is
  fully defeated (p = 0.000 at every fill). On a synthetic cover with a smooth
  histogram the ±1 blur shrinks the pair differences ~4× and that is enough to
  even them out, so it still fires. The test asserts the RS result, which holds on
  every cover measured, and documents the chi-square result rather than pinning a
  fixture.

### Phase 4b — Keyed embedding (deferred)

Deferred from Phase 4 by choice, not by difficulty: both items hang off a `--key`
flag that does not exist yet, and both change what a stego file looks like, so they
belong in one release together with the format and CLI decisions they force.

- SHA-256 → ChaCha20 keystream; whiten payload **and** header
- Keyed permutation of embedding positions (Fisher–Yates over the position index,
  seeded from the key)
- The open question to settle first: auto-detection reads a plaintext `DTM1` magic
  today, and whitening the header removes it. Extraction without a key then has
  nothing to recognise, so `datum extract` needs either a keyless mode that keeps
  the old plaintext header or a "wrong key and no payload are indistinguishable"
  contract. That is a format decision, and it is the reason this is its own phase.
- **Test:** RS on a 10%-filled stego stays at the clean baseline *and* the payload
  positions are unrecoverable without the key

**The decision, written out.** Nothing below is chosen yet.

| Option | Keyless `embed` | Keyed `embed` | Cost |
|---|---|---|---|
| A. Keep keyless as today | plaintext header, raster order — every existing carrier still reads | whitened header + scatter; `extract` without `--key` reports "no payload" | Two behaviours to document; a keyless carrier stays trivially detectable |
| B. Always whiten | header whitened with a fixed public key | whitened with the passphrase | Old carriers need a compatibility read path; "no key" becomes "the public key" |

Either way the format version and the `flags.encrypted` bit are where the change is
recorded, and a keyed carrier must be indistinguishable from "no payload" without the
key. Auto-detection with a key has to try every `(mode, param)` candidate *through* the
keyed permutation, so its cost multiplies — measure it before promising it.

Also to decide in this phase: whether the scatter applies to every mode or to `lsb`
only. Every codec visits its positions (samples, pixels or blocks) in raster order
today, and a spanning video payload restarts that order in each frame, so a
permutation per frame keeps the video layer unchanged.

### Phase 5 — QIM (robust while the pixel grid survives) ✅

The cheap half of robustness, and the natural place to build the measurement harness
that Phase 6 also needs.

- `qim` codec with configurable Δ (2–32) behind `--delta`, Δ carried in the header so
  extraction recovers it ✅
- **Harness:** `tools/robustness.cpp` — `embed → re-encode → extract → BER`, swept over
  H.264 CRF 18/23/28 and JPEG quality 95/85/75 at fixed resolution, plus a 4:4:4
  control column and a lossless half-scale column ✅
- Δ-vs-BER table with the default picked from it: [`docs/QIM.md`](QIM.md) ✅
- PSNR **and SSIM** reported alongside BER — banding is the visible cost and PSNR
  understates it ✅
- **Test:** CRC passes after a real lossy re-encode at the default Δ, and BER goes to
  noise once a rescale is introduced ✅

The keyed scatter this phase was meant to reuse does not exist yet — Phase 4b is still
deferred — so `qim` embeds in raster order like every mode before it. Nothing about the
scatter is mode-specific, so it drops in later without touching this codec.

Decisions this phase forced, all of them measured rather than assumed:

- **The bit rides on the pixel's luma, not on each channel separately.** The
  per-channel design section 1 originally described was built first and measured first, and it never
  returned a clean CRC — not at any Δ, on any recipe, at any fill. Independent data in
  R, G and B *is* chroma, and 4:2:0 averages it over each 2×2 block before the
  quantiser sees it, which is the wall `raw` hit in docs/RAW_BITS.md. Shifting all
  three channels by the same offset leaves every channel difference untouched, so the
  colour survives subsampling and BT.601 luma read back from a decoded pixel is exactly
  the luma the codec kept. The `h264 4:4:4` control column is the proof: with the luma
  carrier, turning chroma subsampling off changes almost nothing.
- **Capacity is therefore one bit per pixel — 259 KB at 1080p, not 778 KB.** Section 1's
  original figure described a design that cannot do the one job the mode exists for.
- **A pixel with no headroom is desaturated, not skipped.** A fully saturated pixel
  cannot move as a whole without clipping, so its channels are first pulled toward
  their own luma until it can. Skipping it instead would need the decoder to agree
  about *which* pixels were skipped, from values a re-encode has already nudged, and
  one disagreement shifts every bit after it. At Δ ≤ 32 the fallback needs a spread
  above 191 of 255 before it fires at all.
- **Δ = 28, read off the table rather than guessed.** Two JPEG encoders disagree about
  what a quality number costs, so the default was chosen against both: 26 is where all
  four covers first came back clean and 24 fails on three of them, so the default sits
  one step above a boundary that is demonstrably cover-dependent. It is also the
  smallest step that reaches H.264 CRF 15.
- **H.264 CRF 23 survives at no Δ in range, and the margin is not the reason.**
  Neighbouring pixels get independent offsets, so the carrier *is* high-frequency noise
  and a rate-distortion encoder removes that first — CRF 18 is two orders of magnitude
  better than CRF 23 at an identical margin. Disabling psy-rd and forcing full-range
  conversion move the failure from "no header found" to "CRC mismatch" and no further.
  The fix is redundancy, which is Phase 6, not a larger Δ.
- **BER is a diagnostic, not a verdict.** A CRC needs every bit, so 0.0001 and 0.5 mean
  the same thing to whoever wanted the file. The table reports both, and the harness
  marks the cells where the payload actually came back.
- **The robustness test needs no ffmpeg.** A JPEG round trip through stb — already
  vendored for image I/O — is a real lossy re-encode and is deterministic everywhere,
  so the headline assertion is a plain CTest check instead of one that skips itself on
  a machine without ffmpeg. The rescale half is a 2×2 box average in process, for the
  same reason.
- **SSIM earned its place, but only just.** Across the sweep it tracks Δ more steeply
  than PSNR does at the low end (0.996 to 0.951 over Δ = 2..8, where PSNR moves 12 dB
  and neither is anywhere near visible) and more gently at the high end. It is in the
  table because banding is a structural change and PSNR is a per-sample one; it is not
  in the CLI, because one number after an embed is enough and PSNR is the one people
  already read.

### Phase 6 — Robustness / YouTube ✅

The hard phase. The harness was built before the algorithm, and every choice below was
made against a number it produced.

1. **Harness extended** (`tools/robustness.cpp`) with VP9 at CRF 32 and 40, a 30→24 fps
   conversion, an off-grid `3/4` rescale, and a `--mode` switch so the same sweep runs
   for `qim` and `dct` ✅
2. **`dct` codec on Y:** 8×8 blocks, the bit carried by which of the mid-frequency
   coefficients (4,1) and (3,2) is the larger, pushed at least `--margin` apart ✅
3. **Reed–Solomon RS(255,223)**, `include/datum/ecc.hpp`: 32 parity bytes per block,
   16 correctable byte errors, header and payload as separate codewords ✅
4. **Temporal repetition + majority vote:** `--repeat N` for video ✅
5. **Sync and registration:** *not done, deliberately* — see below
6. **Upload to an unlisted video:** not run. Every number here is ffmpeg locally, with
   flags chosen to bracket a delivery encode.

Measured outcome: **3490 payload bytes per 1080p frame**, and the payload comes back
byte for byte through JPEG 75, H.264 to CRF 28, VP9 to CRF 40, a frame rate conversion
and a rescale — the line `qim` could not cross at any Δ. The full table, and where the
default margin came from: [`docs/DCT.md`](DCT.md).

Decisions this phase forced, all of them measured rather than assumed:

- **The bit rides on a *relation between two coefficients*, not on a value.** Every
  mode before this reads a bit out of one sample: its low bits, its bucket, its
  quantisation bin. A resample replaces that sample with an average of its neighbours,
  and no margin defends against a replacement — which is why `qim` reads 0.18–0.43 on
  every rescale at every Δ. A DCT coefficient *is* an average over the block, so a
  resample perturbs it instead. Making the bit a comparison buys a second thing for
  free: anything that scales both coefficients equally cancels out of it.
- **8×8 blocks were enough; 16×16 was never needed.** The original estimate in section 1 assumed
  16×16 and 30× temporal repetition to survive a rescale. Measured, 8×8 clears the
  off-grid `3/4` rescale from margin 4 and a half-scale from margin 12, so the capacity
  the larger block would have cost buys nothing.
- **Off the grid turned out to be *easier* than on it.** `3/4 scale` moves every block
  boundary somewhere new and clears four steps earlier than `half scale` does, which
  lands every boundary back where it started. How much detail the filter destroys
  matters more than where the lattice falls — which is also why item 5 was dropped.
- **Reed–Solomon belongs to `dct` alone.** Every other mode is all-or-nothing: a
  lossless carrier gives a perfect stream and a lossy one gives noise, so parity would
  cost capacity and buy nothing. `dct` is the first mode whose stream arrives
  *slightly* wrong, and a CRC needs every bit. Half the passing cells in the sweep have
  a non-zero BER; RS is what stands between them and a failed extraction.
- **The header needs its own codeword.** Extraction has to read and trust the header
  before it knows how long the payload is, so the two cannot share a codeword. 32
  parity bytes over a 16-byte header is lavish on purpose: a header that will not parse
  loses everything behind it.
- **The 4:4:4 control column changed meaning.** In Phase 5 it existed to prove chroma
  subsampling was the problem. With the luma carrier it stopped mattering, and what is
  left is x264 spending more of the same CRF budget on full-resolution chroma and less
  on luma — so it is now the *hardest* column, not the easiest.
- **Fill matters for `dct` where it did not for `qim`,** and through the parity rather
  than the pixels. More payload is more RS blocks, and the payload is lost as soon as
  any single one takes more than 16 wrong bytes. At 100% fill the half-scale column
  clears four steps later than at 25%.
- **Repetition had to become whole copies in whole frames, not chunks.** The obvious
  design repeats each *bit* group across a group of frames, and it breaks on exactly
  the damage it exists for: a frame rate conversion *drops* frames, and one missing
  frame shifts every bit after it. Writing the entire stream into each of N frames
  instead makes every frame independent, so a lost frame costs one vote. It caps the
  payload at a single frame's worth, which for the mode's actual use is the right trade.
- **A single-frame harness cannot measure a dropped frame.** The `h264 24fps` column
  encodes a clip of identical frames, so what it reports is the encode under a
  different GOP and rate structure — real, but not the interesting half. The dropped
  frame is a video-layer question, and it is asserted in the test suite instead: with
  `--repeat 5` the payload survives its first frame being deleted, and with `--repeat 1`
  it does not.
- **Sync and registration (item 5) was dropped after measuring, not skipped.** It was
  written to solve "a rescale moves the block lattice", and the `3/4 scale` column says
  the lattice moving is not what hurts — it clears at margin 4. What is still not
  handled is a carrier that reaches the decoder at a *different size*, which needs
  putting back on its original grid by hand. Searching for the grid needs no format
  change when it is wanted: the DTM1 magic inside its own Reed–Solomon codeword is
  already a sync word, so the decoder can try offsets until the header decodes.
- **Embedding verifies itself block by block.** Asking for a coefficient change and
  assuming it landed is wrong: the block is rounded back to whole numbers and clamped
  into 0..255 afterwards, and one near black or white loses most of the push. Each
  block re-reads what the image now holds and asks for more if the relation is short,
  up to four passes. Without it the mode would spend its whole error budget before the
  carrier was even touched — so the round-trip test asserts a bit error rate of exactly
  zero on a lossless carrier, with no help from the parity.

### Phase 7 — Performance

- Parallelise over frames (`std::thread`, or `std::for_each(par_unseq)`)
- Profile before optimising; the ffmpeg pipe is the likely bottleneck, not the codec
- SIMD only if profiling actually points there

Known candidates, each already marked with a `ponytail:` comment where it lives:

- `dct` embedding runs a full forward and inverse 8×8 DCT per attempt, up to four
  attempts per block, when only two coefficients are read and changed — a rank-2
  update would do (`src/codecs.cpp`, `DctCodec::carry_block`).
- `probe` runs `ffprobe -count_packets`, which demuxes the whole file just to count
  frames (`src/video.cpp`).
- Auto-detection tries up to 46 `(mode, param)` candidates, each reading a header's
  worth of bits; cheap on a still, repeated once per `extract`.

Parallelising frames has one constraint to respect: a spanning payload feeds one
`BitReader` across frames, so frames must be *embedded* in order even if they are
decoded and encoded concurrently.

---

## 7. Metrics tracked in the README

| Metric | Meaning | Target |
|---|---|---|
| PSNR | Distortion vs cover | > 45 dB for invisible modes (`lsb`) |
| SSIM | Perceptual similarity | > 0.99 for `lsb`; reported, not targeted, for `qim` / `dct` |
| BER | Bit error rate after a round trip | 0 for L0–L5 within each mode's carrier; below the RS correction limit (~0.008) for L6 |
| bpp | Bits per pixel | mode dependent |
| Survival | CRC pass rate after a re-encode | 100% is the only passing grade |

---

## 8. Non-goals

- Not a DRM or forensic watermarking system (no collusion resistance).
- Not a replacement for encryption — hiding is not confidentiality. Encrypt first, hide second.
- Does not hide the *existence* of a file of unusual size or format.
- No GUI. CLI only; a GUI can wrap the CLI later if anyone wants one.

## 9. Risks

| Risk | Mitigation |
|---|---|
| Windows text-mode pipes corrupt frames | `_popen` with `"rb"` / `"wb"`; the video tests carry payloads containing every byte value |
| An old `ffprobe` shadows the real one on `PATH` | `DATUM_FFMPEG` / `DATUM_FFPROBE` overrides; the error message names them |
| YouTube changes its encoder ladder | The harness measures BER; re-tune parameters, never hardcode them |
| Binary test media bloats the repo | Fixtures are generated in code; `samples/local/` is gitignored |
| LSB detectable by standard steganalysis | Phase 4 exists for exactly this; `datum analyze` is the check |
| Scope creep into a general media library | The non-goals above; every dependency justified in section 4 |

## 10. Legal note

Uploading steganographic content to a third-party platform is subject to that
platform's terms of service. The Phase 5 and Phase 6 work is a robustness experiment
against lossy re-encoding; run it on your own content.

## 11. Open work and backlog

Everything not yet done, in one place. Phases 4b and 7 above are the planned work;
this list is what surrounds them. When an item is picked up, move its decision record
into the phase that builds it.

### Planned next

1. **Phase 4b — keyed embedding.** Decide option A or B above first, then build
   SHA-256 + ChaCha20 (`crypto.hpp`), header and payload whitening, and the keyed
   permutation. Bump the DTM1 version if the header changes meaning.
2. **`docs/FORMAT.md`** — the DTM1 spec as a standalone document, written once 4b has
   settled the format rather than before.
3. **Phase 7 — performance**, profile first (candidates listed in Phase 7).

### Known limitations (documented, not bugs)

- **Raster order.** Every mode fills the carrier from the top-left, so a small `lsb`
  payload is concentrated in the first rows instead of spread thin — a dense region
  for a detector to find. Fixed by 4b's keyed scatter.
- **Plaintext header.** `DTM1` sits readable in the carrier. Fixed by 4b's whitening.
- **`dct` has no registration.** A crop, letterbox, shift or a carrier delivered at a
  different size puts the 8×8 lattice at the wrong phase. The measured rescale cases
  pass because they return to the original size. Searching offsets until the RS-coded
  header decodes needs no format change (`ponytail:` in `src/codecs.cpp`).
- **`--repeat` reads the header from the first decoded frame only.** The payload is
  majority-voted across copies; the header is not. A first frame damaged past the
  header codeword's 16-byte correction loses the payload even when later copies are
  intact. Voting the header block too would close it.
- **Repeated payloads must fit in one frame** (3490 bytes at 1080p for `dct`). Larger
  payloads span frames and do not survive a dropped frame.
- **No real YouTube upload has been measured.** Every robustness number is local
  ffmpeg with flags chosen to bracket a delivery encode.
- **`binary` and `raw` are not in the harness.** `raw`'s numbers came from a one-off
  harness that was not kept; `binary` has only the 2026-09-16 spot check in §1.
  Adding both modes to `datum_robustness` would make their README rows measured.
- **Non-ASCII paths on Windows.** `argv` is ANSI (`ponytail:` in `apps/datum.cpp`), and
  the video layer builds shell commands from `path.string()`. Switch to `wmain` /
  `GetCommandLineW` and wide `_wpopen` if it matters.
- **`analyze` verdict thresholds are flat** (chi-square p > 0.5, RS rate > 10%), with no
  calibration set behind them (`ponytail:` in `apps/datum.cpp`).
- **Approximations with a known ceiling:** chi-square tail via Wilson–Hilferty instead
  of the incomplete gamma function (`src/analyze.cpp`); SSIM over 8×8 uniform windows
  instead of the 11×11 Gaussian (`src/image.cpp`). Both are good to ~1e-3.
- **MSVC is untested.** `CMakeLists.txt` has MSVC warning flags, but CI builds only
  MinGW-w64 (UCRT64) and Linux GCC.

### Known bugs

- **No guard for payloads of 4 GiB or more.** `make_header` narrows `payload.size()` to
  the `u32` `payload_len` without a check. An image can never hold that much, but a long
  video spanning frames can: the embed would succeed and the extract would fail its
  CRC. One check in `make_header` (or `embed_video`) would reject it up front.

### Test-suite debt

- `tests/roundtrip.cpp` chains tests with `&&`, so the first failure hides every test
  after it, and a test that *throws* (rather than failing a `CHECK`) terminates the
  process instead of printing `FAILED`. Catching exceptions per test and running them
  all would make a red CI run say more.

### Stretch goals (not scheduled)

- **Content-adaptive embedding** (HUGO, WOW, S-UNIWARD): spend bits in textured regions.
- **JPEG-domain embedding** (jsteg / F5 family) for JPEG-in, JPEG-out hiding. This is
  where linking libavcodec or a JPEG library might finally be justified (§4).
- **A multi-bit grey carrier** for `raw` — one value per pixel across R=G=B, 2–3 bits
  deep. [`RAW_BITS.md`](RAW_BITS.md) measured it surviving H.264 CRF 23 where
  per-channel `raw` cannot.
- **A real upload test** against an unlisted video, run on the project's own content.
