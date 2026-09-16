# `raw --bits N` — measured against real re-encoders

`raw` used to write one payload byte per colour channel: all 8 bits, no room to
spare. `--bits N` writes only the top `N` and sets the bits below them to the middle
of the bucket those top bits name.

```
N = 5, payload value 19          bucket 19 spans 152..159
        ┌───────── payload ──────┐┌── centre ──┐
sample  1  0  0  1  1             1  0  0        = 156
        └─ 19 in 5 bits ─┘        └─ 4 = 8/2 ──┘
```

The bucket is `2^(8-N)` wide, the sample sits at its centre, and decoding is
`sample >> (8-N)`. So the channel can drift by up to half a bucket in either
direction and still decode to the same `N` bits:

| `--bits N` | levels | bucket | drift margin | capacity vs `N = 8` |
|---:|---:|---:|---:|---:|
| 8 | 256 | 1 | ±0 | 100% |
| 5 | 32 | 8 | ±4 | 62% |
| 4 | 16 | 16 | ±8 | 50% |
| 3 | 8 | 32 | ±16 | 38% |
| 2 | 4 | 64 | ±32 | 25% |
| 1 | 2 | 128 | ±64 | 12% |

`N = 8` is the old behaviour byte for byte: a bucket of 1, a centre offset of 0.
Centring is what earns the margin — at `N = 8` the value sits at the bucket floor,
so *any* downward drift falls into the bucket below.

## What actually survives

640×480, `raw` filled to capacity with random bytes, ffmpeg 9.0.1, one frame in and
back out. Cells are wrong bits out of the whole stream; **bold** is a clean CRC, i.e.
the file came back byte for byte.

`rgb` is what `datum` writes today — R, G and B each carry their own `N` bits.

| carrier | N | margin | capacity | h264 4:2:0 crf18 | h264 4:2:0 crf23 | h264 4:4:4 crf18 | JPEG q2 |
|---|---:|---:|---:|---:|---:|---:|---:|
| rgb | 1 | ±64 | 115 KB | 190 967 | 200 349 | 13 | **0** |
| rgb | 2 | ±32 | 230 KB | 641 212 | 653 754 | 21 775 | **0** |
| rgb | 3 | ±16 | 346 KB | 1 110 077 | 1 120 586 | 296 036 | 2 554 |
| rgb | 4 | ±8 | 461 KB | 1 570 184 | 1 580 230 | 800 571 | 135 812 |
| rgb | 5 | ±4 | 576 KB | 2 032 797 | 2 043 205 | 1 282 143 | 618 735 |
| rgb | 8 | ±0 | 922 KB | 3 413 377 | 3 424 988 | 2 668 668 | 2 086 022 |

Two things fall out of that.

**The margin works.** Every column improves monotonically as `N` drops, and the
improvement is large — through 4:4:4 the error goes from 2.7 million bits at `N = 8`
to 13 at `N = 1`. Against JPEG the win is total: `--bits 2` recovers a 40 KB payload
byte for byte through a `-q:v 2` re-encode, where `--bits 8` cannot even find its own
header. That is a capability `datum` did not have before.

**It is not enough for mp4, and the margin is not why.** H.264's default `yuv420p`
averages chroma over each 2×2 pixel block. `raw` puts *independent* data in R, G and
B, so a pixel's colour is high-frequency noise, and averaging destroys it no matter
how wide the bucket is. A margin defends against a value being *nudged*; it does
nothing against a value being *replaced by the mean of its neighbours*.

The next table shows that is the whole story. `grey` writes one value per pixel into
R = G = B, so the chroma planes are flat and there is nothing for 4:2:0 to average
away; `greyN` additionally paints each value over an N×N block and averages it back
on the way out.

| carrier | N | capacity | h264 4:2:0 crf18 | h264 4:2:0 crf23 | h264 4:4:4 crf18 | JPEG q2 |
|---|---:|---:|---:|---:|---:|---:|
| grey | 1 | 38 KB | **0** | **0** | **0** | **0** |
| grey | 2 | 77 KB | 11 | 747 | 11 | **0** |
| grey | 3 | 115 KB | 3 311 | 59 556 | 3 359 | **0** |
| grey2 | 2 | 19 KB | **0** | **0** | **0** | **0** |
| grey2 | 3 | 29 KB | **0** | 4 669 | **0** | **0** |
| grey4 | 3 | 7 KB | **0** | **0** | **0** | **0** |
| grey4 | 5 | 12 KB | 1 638 | 9 785 | 1 929 | **0** |

`grey --bits 1` carries 38 KB per 640×480 frame through H.264 at CRF 23 with zero
wrong bits — more capacity *and* more robustness than any of the block variants,
because spreading a value over a block costs more than it buys once the chroma
problem is gone.

## Where that leaves things

`--bits N` shipped: it is a real robustness dial, and it makes `raw` survive JPEG.
Surviving mp4 needs the *other* half — one value per pixel across R = G = B instead of
three independent ones — which is a different carrier layout, not a different bit
depth, and is not implemented. `binary` mode already occupies that corner at `N = 1`
(black/white instead of 64/192, so an even wider margin); the measurements above say a
2- or 3-bit version of it is the interesting thing to build next.

The tables above came from a one-off harness that was not kept in the repository —
`datum_robustness` sweeps only `qim` and `dct`. The JPEG result reproduces by hand:

```sh
datum embed -i cover.png -o stego.png -d secret.zip --mode raw --bits 2
ffmpeg -i stego.png -q:v 2 stego.jpg
datum extract -i stego.jpg -o back.zip     # CRC ok
```
