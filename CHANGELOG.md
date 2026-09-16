# Changelog

All notable changes to `datum` are recorded here. The format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and the project uses
[Semantic Versioning](https://semver.org/spec/v2.0.0.html).

While the version is below 1.0, the command-line interface and the DTM1 carrier format
may still change. Every such change is listed here, along with whether carriers made by
earlier versions can still be read.

## [Unreleased]

## [0.1.0] - 2026-09-16

The first public release, covering roadmap Phases 0–6.

### Added

- **Five embedding modes** for still images and video frames:
  - `lsb`: payload in the low 1–4 bits of each colour channel, using LSB *matching*
    instead of replacement (invisible at `--bits 1`).
  - `raw`: payload in the top 1–8 bits of each colour channel, with the remaining bits
    centred; `--bits 2` survives a high-quality JPEG re-save.
  - `binary`: one bit per pixel, painted black or white.
  - `qim`: Quantization Index Modulation on a pixel's luma (`--delta`, default 28);
    survives a high-quality JPEG re-save at the same size.
  - `dct`: one bit per 8×8 luma block, carried by the relation between two
    mid-frequency DCT coefficients (`--margin`, default 32), with Reed–Solomon
    RS(255,223) error correction; survives JPEG quality 75, H.264 up to CRF 28, VP9 up
    to CRF 40, a 30 → 24 fps conversion and a resize back to the original size.
- **Commands:** `info`, `capacity`, `embed`, `extract`, `analyze`, `help`, `--version`.
- **DTM1 carrier format, version 1:** a 16-byte header with magic, mode, strength
  setting, payload length and CRC-32, so `extract` detects the mode and its setting on
  its own and never returns a corrupted payload silently.
- **Video support** through the `ffmpeg` and `ffprobe` binaries: lossless FFV1/MKV
  output, payloads spanning frames, audio tracks copied across, frames streamed one at
  a time, and `--repeat N` whole-payload copies with majority voting.
- **Steganalysis:** `datum analyze` runs chi-square and RS analysis against a carrier.
- **Measurement harness:** `datum_robustness` sweeps `qim` and `dct` settings across
  JPEG, H.264, VP9, a frame-rate conversion and two rescales, reporting BER, PSNR and SSIM.
- **Safety checks:** lossy output formats, oversized payloads, unknown flags and flags
  that belong to another mode are all rejected with a clear error.
- **Prebuilt binaries** for Windows x86-64 and Linux x86-64, both statically linked.

### Known limitations

- There is no encryption and no key yet: anyone with `datum` can extract a payload.
  Encrypt files before hiding them. Keyed embedding (`--key`) is planned for the next
  release and may change the carrier format.
- `dct` carriers must reach the decoder at their original size; crops and size changes
  are not supported.
- None of the robustness figures have been measured against a real YouTube upload.
- Payloads of 4 GiB or more are not rejected up front; extraction fails the CRC.

See [`docs/ROADMAP.md` §11](docs/ROADMAP.md#11-open-work-and-backlog) for the full list.

[Unreleased]: https://github.com/iEmreM/datum/compare/v0.1.0...HEAD
[0.1.0]: https://github.com/iEmreM/datum/releases/tag/v0.1.0
