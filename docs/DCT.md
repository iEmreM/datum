# `dct` — measured against a delivery re-encode

`qim` was the first mode that did not need a lossless carrier, and it stopped at a
hard line: it survives a re-encode at the same size and nothing that *resizes*. `dct`
is the mode built to cross that line. It is the first one here that comes back from a
rescale, from H.264 at a delivery bitrate, from VP9, and from a frame rate conversion
— which together is most of what an upload does to a video.

```
embed:    Y      = the 8x8 block's luma plane
          F      = DCT-II(Y)                       # orthonormal, i.e. JPEG's own scaling
          a, b   = F[4][1], F[3][2]                # one mid-frequency pair
          push a and b apart, symmetrically, until (a - b) has the bit's sign
                   and is at least `margin` wide
          Y'     = IDCT(F) ; every pixel of the block moves by Y'-Y, all channels together
extract:  bit    = DCT(Y)[4][1] > DCT(Y)[3][2]
```

`margin` is the robustness dial and the only parameter. Everything below is a
measurement of what it buys and what it costs.

## Why a relation between coefficients, and not a pixel

Every mode before this one reads a bit out of *one sample's value*: its low bits
(`lsb`), its bucket (`raw`), its quantisation bin (`qim`). A resample replaces that
sample with a weighted average of its neighbours, and no margin defends against a
value being replaced rather than nudged. [`docs/QIM.md`](QIM.md) has the measurement:
`qim` reads 0.18–0.43 — which is what guessing scores — on every rescale at every
delta.

A DCT coefficient *is* an average over the whole block, weighted by a cosine. Resample
the block and the coefficient is perturbed, not replaced. That is the entire reason
this mode exists.

The bit rides on a *comparison* between two coefficients rather than on either one's
value, which buys a second thing: anything that scales both of them equally cancels.
A brightness change, a contrast curve, a quantiser that treats neighbouring
frequencies alike — none of them change which of the two is larger.

**The pair is (4,1) and (3,2)**, the classic Zhao-Koch choice, and it is not
arbitrary: JPEG's standard luma quantisation table gives both the same step of 22, so
a re-encode damages them equally. Lower frequencies carry the block's visible
structure and cannot be moved without it showing; higher ones are the first thing any
encoder rounds to zero, which takes the bit with them.

**The whole pixel moves together**, exactly as in `qim`: the luma change is applied to
R, G and B by the same amount, so every channel *difference* — the colour — comes
through untouched and 4:2:0 chroma subsampling has nothing of ours to average away.
This is the same lesson [`docs/RAW_BITS.md`](RAW_BITS.md) learned the hard way.

## Why Reed-Solomon is mandatory here and nowhere else

Every mode before `dct` is all-or-nothing. A lossless carrier hands the decoder a
perfect stream; a lossy one hands it noise. There is no middle, so parity bytes would
have bought nothing and cost capacity.

`dct` is the first mode whose stream arrives *slightly* wrong — a handful of blocks
out of thousands land on the wrong side of the comparison — and a CRC needs every
bit. RS(255,223) corrects up to 16 wrong bytes anywhere in a 255-byte block, which is
what turns "0.008 bit error rate" into "the file came back".

The stream is two independent codewords, not one:

```
| 48 bytes: 16-byte header + 32 parity | payload, 223 data + 32 parity per block |
```

The header gets its own codeword because extraction has to read and trust it *before*
it knows how long the payload behind it is. 32 parity bytes over 16 data bytes is
lavish, and deliberately so: a header that does not parse loses everything behind it,
while a payload block that does not decode loses only itself.

The correction limit is also the design target. 16 bytes of 255 is 6.3% of a block,
which isolated bit errors reach at a bit error rate of roughly 0.008. Anything above
that is not a tuning problem — it is past what the code can do, and the answer is a
wider margin or more copies.

## Temporal repetition, for video

`--repeat N` writes the whole stream into each of the first N frames and
majority-votes the copies back together. It caps the payload at what a single frame
holds, and buys the one thing a payload spanning frames cannot have: **independence**.

A spanning payload is one stream cut across frames, so a frame that never arrives
shifts every bit after it and the payload is gone. That is not hypothetical — a frame
rate conversion drops frames by definition, and 30 to 24 fps drops one in five. With
repetition, a missing frame costs one vote. So does a blended one, and so does one the
encoder spent no bits on.

The test suite asserts exactly this, on a real clip: with `--repeat 5` the payload
survives its first frame being deleted, and with `--repeat 1` it does not.

The count lives in the top five bits of the header's `flags` byte, so extraction never
has to be told. Five bits is 1–32 copies, which is far past the point where the
capacity it costs is worth having.

## The sweep

`datum_robustness --mode dct` embeds, re-encodes, extracts and reports the bit error
rate. 640×480 `testsrc2` cover, payload filling 25% of capacity, ffmpeg 9.0.1, JPEG
written by stb at the stated quality. Cells are BER; **bold** is a clean CRC, i.e. the
file came back byte for byte *after* Reed-Solomon had its turn. PSNR and SSIM are the
cost of embedding, measured against the cover before any re-encode.

| margin | PSNR | SSIM | jpeg 95 | jpeg 85 | jpeg 75 | h264 crf18 | h264 crf23 | h264 crf28 | h264 444 crf23 | vp9 crf32 | vp9 crf40 | h264 24fps | half scale | 3/4 scale |
|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| 2 | 48.6 | 0.997 | **0.0026** | 0.3466 | 0.4936 | 0.5052 | 0.5052 | 0.4858 | 0.5071 | 0.4207 | 0.5026 | 0.5052 | 0.0689 | 0.0348 |
| 4 | 46.3 | 0.992 | **0.0000** | **0.0019** | **0.0097** | 0.1244 | 0.4987 | 0.4948 | 0.5000 | 0.0271 | 0.2700 | 0.1089 | 0.0503 | **0.0135** |
| 6 | 44.2 | 0.984 | **0.0000** | **0.0000** | **0.0019** | 0.1070 | 0.1250 | 0.4942 | 0.1327 | **0.0000** | 0.0722 | 0.1115 | 0.0425 | **0.0058** |
| 8 | 42.6 | 0.975 | **0.0000** | **0.0000** | **0.0006** | 0.1031 | 0.1108 | 0.4807 | 0.1063 | **0.0000** | **0.0168** | 0.0651 | 0.0354 | **0.0032** |
| 10 | 41.0 | 0.962 | **0.0000** | **0.0000** | **0.0000** | 0.0612 | 0.0960 | 0.4852 | 0.1095 | **0.0000** | **0.0006** | 0.0258 | 0.0335 | **0.0000** |
| 12 | 39.6 | 0.951 | **0.0000** | **0.0000** | **0.0000** | **0.0226** | 0.0883 | 0.1566 | 0.0934 | **0.0000** | **0.0000** | **0.0045** | **0.0277** | **0.0000** |
| 16 | 37.3 | 0.923 | **0.0000** | **0.0000** | **0.0000** | **0.0019** | 0.0612 | 0.0960 | 0.1044 | **0.0000** | **0.0000** | **0.0064** | **0.0200** | **0.0000** |
| 20 | 35.4 | 0.897 | **0.0000** | **0.0000** | **0.0000** | **0.0006** | **0.0148** | 0.0747 | 0.0786 | **0.0000** | **0.0000** | **0.0000** | **0.0155** | **0.0000** |
| 24 | 33.8 | 0.871 | **0.0000** | **0.0000** | **0.0000** | **0.0000** | **0.0045** | 0.0657 | 0.0715 | **0.0000** | **0.0000** | **0.0000** | **0.0110** | **0.0000** |
| 28 | 32.5 | 0.850 | **0.0000** | **0.0000** | **0.0000** | **0.0000** | **0.0000** | **0.0129** | 0.0238 | **0.0000** | **0.0000** | **0.0000** | **0.0071** | **0.0000** |
| **32** | 31.3 | 0.830 | **0.0000** | **0.0000** | **0.0000** | **0.0000** | **0.0006** | **0.0032** | 0.0271 | **0.0000** | **0.0000** | **0.0000** | **0.0064** | **0.0000** |
| 48 | 27.8 | 0.779 | **0.0000** | **0.0000** | **0.0000** | **0.0000** | **0.0000** | **0.0006** | **0.0000** | **0.0000** | **0.0000** | **0.0000** | **0.0006** | **0.0000** |

Five things fall out of it.

**The rescale columns clear, and that is the phase.** `half scale` and `3/4 scale` use
a *lossless* codec, so the only thing they measure is the resampling. `qim` sits at
0.18–0.43 in both at every delta — the noise floor. `dct` clears `3/4 scale` from
margin 4 and `half scale` from margin 12. The line every spatial mode stopped at is
behind us.

**Off the grid is easier than on it, which is backwards until you look.** `3/4 scale`
moves every 8×8 block boundary somewhere new and still clears four margins earlier
than `half scale` does. Halving throws away three quarters of the samples outright;
scaling by 3/4 keeps more of the block's energy and only smears where its edges fall.
The block lattice moving turns out to matter less than how much detail the filter
destroys.

**H.264 is the hard encoder, VP9 is not.** VP9 at CRF 32 clears from margin 6 and CRF
40 from margin 8, while H.264 at CRF 23 needs 20 and CRF 28 needs 32. That is worth
knowing because VP9 is what YouTube serves most viewers — but the H.264 ladder is what
it encodes first, so the H.264 column is the one that decides.

**The 4:4:4 control is now the *harder* column, not the easier one.** In
[`docs/QIM.md`](QIM.md) it existed to prove chroma subsampling was the problem; with
the luma carrier it stopped mattering, and what is left is x264's own behaviour — at
the same CRF it spends more of its budget on full-resolution chroma and less on luma.
It is a useful worst case, not an anomaly.

**Bit error rate stopped being the whole story.** Half the bold cells above have a
non-zero BER: 0.0064 came back clean at margin 32 because Reed-Solomon put it right.
BER is now a measure of how much of the error budget a recipe spends, and the bold is
still the only verdict.

### How full the carrier is *does* matter here

For `qim` it barely did — the same sweep at 5% and 100% fill moved every cell by less
than 0.005. For `dct` it matters, through the parity rather than the pixels. A larger
payload is more RS blocks, and the payload is lost as soon as *any one* of them takes
more than 16 wrong bytes. At 100% fill on the same cover the `half scale` column does
not clear until margin 28, four steps later than at 25%.

Distortion moves with fill for the ordinary reason — untouched blocks are untouched:

| fill | payload | PSNR @ margin 32 | SSIM |
|---|---:|---:|---:|
| 5% | 22 B | 34.0 dB | 0.911 |
| 25% | 114 B | 31.3 dB | 0.830 |
| 100% | 456 B | 26.5 dB | 0.470 |

The rule is the same one that applies to every mode here: **do not fill the carrier.**

## Picking the default

The smallest `margin` at which the payload came back byte for byte, over three
generated covers, through the recipes an upload actually applies (the 4:4:4 control
excluded, since no delivery pipeline uses it):

| cover | jpeg 75 | h264 crf23 | h264 crf28 | vp9 crf40 | 24 fps | half scale | 3/4 scale |
|---|--:|--:|--:|--:|--:|--:|--:|
| testsrc2 | 4 | 20 | **28** | 8 | 12 | 12 | 4 |
| mandelbrot | 4 | 8 | 16 | 6 | 6 | 4 | 2 |
| smptehdbars | 4 | 16 | 24 | 8 | 8 | 4 | 2 |

**The default is `margin = 32`.** 28 is where every cover first cleared every column
— but it cleared the hardest of them, `h264 crf28` on `testsrc2`, at a bit error rate
of 0.0129, close enough to Reed-Solomon's limit that a different x264 build could take
it. 24 already fails on two covers out of three. The boundary is plainly
cover-dependent, at 16, 24 and 28 across the three, so the default sits one step above
it rather than on it — the same reasoning that put `qim`'s default at 28 rather than
26.

`testsrc2` is the hardest of the three because it is full of fine detail and noise,
which is exactly where a rate-distortion encoder spends least, so the default is set
against it rather than against the average. 48 buys the 4:4:4 control column and
nothing else, for 3.5 dB.

## The visible cost

At the default margin and 25% fill, `dct` reads 31.3 dB and SSIM 0.830. That is not an
invisible mode — `lsb` is the invisible one — but it is a much cheaper picture than
`qim` pays for far less robustness:

| | PSNR | SSIM | survives |
|---|---:|---:|---|
| `qim --delta 28` | 23.6 dB | 0.823 | jpeg 95, vp9 crf32 |
| `dct --margin 32` | 31.3 dB | 0.830 | all of the above, plus jpeg 75, h264 to crf 28, vp9 crf40, 24 fps, both rescales |

Same cover, same fill, ~8 dB apart in the right direction. The reason is *where* the
change lands. `qim` pulls every pixel in a flat region to the nearer of two grey levels
56 apart, which bands — a low-frequency change, and low frequency is what an eye is
most sensitive to. `dct` adds a ripple at a mid-to-high spatial frequency inside one
8×8 block, which reads as a little extra texture. It is visible on close inspection of
a flat sky. It does not look like a fault in the picture.

`datum analyze` reads a `dct` stego as clean, and that means nothing at all: both
attacks it runs hunt the LSB *replacement* signature, which this mode does not produce
anywhere near the low bits. Concealment is still `lsb`'s job.

## What this does not do

- **No registration.** A crop, a rotation, a letterbox or a shift moves the 8×8 block
  lattice, and the decoder reads it at the original phase. A rescale that lands back on
  the same frame size is fine — that is the `3/4 scale` column — but a carrier that
  reaches the decoder at a *different* size or offset has to be put back on its
  original grid first, by hand. Searching for the grid is the natural next step, and
  it needs no format change, because the DTM1 magic and its Reed-Solomon codeword are
  already a sync word: the decoder can try offsets until the header decodes.
- **Repetition needs the payload to fit in one frame**, since each copy is a whole
  stream. At 1080p that is 3490 bytes. Bigger payloads have to span frames, and a
  spanning payload cannot survive a dropped frame.
- **This is not a measurement against YouTube.** Every number here comes from ffmpeg
  running locally with flags chosen to bracket a delivery encode. A real upload is the
  last step and has not been run; the roadmap says so, and so does this.

## Capacity

One bit per 8×8 block of luma, minus the header codeword and 32 parity bytes per 223
of payload.

```
1920x1080: (1920/8) x (1080/8)      = 32 400 blocks = 4050 carrier bytes per frame
           minus the 48-byte header codeword and RS parity
                                    = 3490 payload bytes per frame
video, spanning frames              = 3490 x frames         (fragile: no redundancy)
video, --repeat N                   = 3490 bytes, whatever the length
```

A 90-frame 1080p clip holds 318 KB spanning, or 3.4 KB repeated. The roadmap's original
estimate was 0.1–1 KB per second of video and this is the same order — the difference is that
`--repeat` spends frames on redundancy rather than on more payload, which is the trade
the mode is for.

## Reproducing

```sh
cmake --build build --target datum_robustness
build/datum_robustness --mode dct --fill 0.25          # generated cover, needs ffmpeg
build/datum_robustness --mode dct -i photo.png --fill 0.1
```

A cover passed with `-i` has to be 3-channel RGB with both dimensions a multiple of 8;
the harness says so and prints the `ffmpeg` crop that fixes it. The rescale recipes are
why — they have to land back on the original size exactly for the two images to stay
comparable sample by sample.

By hand. Note that `datum` still writes the stego losslessly — the re-encode is
something the world does to it afterwards, which is the whole scenario:

```sh
datum embed -i cover.png -o stego.png -d secret.zip --mode dct
ffmpeg -i stego.png -vf scale=iw*3/4:ih*3/4,scale=iw*4/3:ih*4/3 -q:v 5 shrunk.jpg
datum extract -i shrunk.jpg -o back.zip                # CRC ok

datum embed -i clip.mp4 -o stego.mkv -d secret.zip --mode dct --repeat 9
ffmpeg -i stego.mkv -vf fps=24 -c:v libx264 -crf 23 delivered.mp4
datum extract -i delivered.mp4 -o back.zip             # CRC ok
```
