#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>

#include "datum/codec.hpp"
#include "datum/container.hpp"

namespace datum {

/// True for the container extensions we hand to ffmpeg. Used to pick the video
/// path over the image path, so the CLI stays one set of subcommands.
bool is_video(const std::filesystem::path& file);

/// What we need to know about a video stream before rewriting it. Frames are
/// decoded to rgb24, so a frame is `width * height * 3` bytes.
struct VideoInfo {
    int width = 0;
    int height = 0;
    std::size_t frames = 0;  ///< 0 when the container does not say
    std::string frame_rate;  ///< ffmpeg's rational, e.g. "30000/1001"

    std::size_t frame_bytes() const {
        return static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 3;
    }
};

/// Runs ffprobe. Throws std::runtime_error if it fails or reports no video stream.
VideoInfo probe(const std::filesystem::path& file);

/// Decodes frame 0 alone, as rgb24, so the still-image tools can be pointed at a
/// video. Both detection and analysis only ever need the first frame.
Image first_frame(const std::filesystem::path& file);

/// Payload bytes the whole video holds in this mode, header excluded. Returns 0
/// when the frame count is unknown.
std::size_t video_capacity(const VideoInfo& info, Mode mode, uint8_t param);

struct VideoStats {
    std::size_t frames = 0;       ///< frames written, i.e. the video's length
    std::size_t frames_used = 0;  ///< frames the payload actually reached into
    double psnr = 0.0;            ///< over the whole video, +infinity if untouched
};

/// Streams `in` through ffmpeg frame by frame, embedding as it goes, and writes
/// `out` as lossless FFV1/MKV with the original audio copied across. The payload
/// spans frames: one BitReader feeds every frame in turn.
///
/// Throws std::runtime_error if the payload does not fit, the destination is not
/// `.mkv`, or ffmpeg fails.
VideoStats embed_video(const std::filesystem::path& in,
                       const std::filesystem::path& out,
                       Mode mode,
                       uint8_t param,
                       std::span<const uint8_t> payload);

/// Recovers a payload written by `embed_video` and verifies its CRC. Pass a mode
/// to force one, or nullopt to auto-detect from the first frame.
Extracted extract_video(const std::filesystem::path& in, std::optional<Mode> mode);

}  // namespace datum
