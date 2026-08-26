#include "datum/video.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <stdexcept>
#include <string>
#include <vector>

#include "datum/bitstream.hpp"

// ffmpeg is driven as a subprocess rather than linked as libavcodec: the whole
// dependency is one binary on PATH, and the data crossing the boundary is raw
// rgb24 frames, which is exactly what the codecs already speak.
#ifdef _WIN32
#define datum_popen _popen
#define datum_pclose _pclose
// _popen inherits the parent's text mode; without binary, CRLF translation mangles frames.
#define datum_read_mode "rb"
#define datum_write_mode "wb"
#else
#define datum_popen popen
#define datum_pclose pclose
// POSIX popen takes only "r"/"w" — a "b" suffix is EINVAL, and its pipes are binary already.
#define datum_read_mode "r"
#define datum_write_mode "w"
#endif

namespace datum {
namespace {

/// Frames cross the pipe as rgb24, and FFV1 stores them as bgr0 — both are plain
/// 8-bit RGB, so the conversion each way is a reorder and byte-exact. Anything
/// YUV would round the values and take the payload with it.
constexpr const char* kRawPixelFormat = "rgb24";
constexpr const char* kStoredPixelFormat = "bgr0";
constexpr int kRawChannels = 3;

/// Overridable because ffmpeg is not always on PATH under the name we expect —
/// and on Windows an old ffprobe from an unrelated package can shadow the real one.
std::string tool(const char* variable, const char* fallback) {
    const char* value = std::getenv(variable);
    return value != nullptr && value[0] != '\0' ? std::string(value) : std::string(fallback);
}

std::string quote(const std::filesystem::path& path) {
    return "\"" + path.string() + "\"";
}

/// cmd.exe strips the outermost pair of quotes from what it is handed, so a
/// command whose program path is quoted needs a second pair to survive.
std::string finalize(const std::string& command) {
#ifdef _WIN32
    return "\"" + command + "\"";
#else
    return command;
#endif
}

std::string lowercase(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return text;
}

/// A pipe to or from a child process. Closing is explicit so the child's exit
/// status can be checked — an ffmpeg that died halfway must not look like a
/// video that simply ended.
class Pipe {
  public:
    Pipe(const std::string& command, const char* mode) {
        file_ = datum_popen(command.c_str(), mode);
        if (file_ == nullptr) {
            throw std::runtime_error("cannot start ffmpeg (is it on PATH?)");
        }
    }

    ~Pipe() {
        if (file_ != nullptr) {
            datum_pclose(file_);
        }
    }

    Pipe(const Pipe&) = delete;
    Pipe& operator=(const Pipe&) = delete;

    /// Fills `buffer` with exactly one frame. False at a clean end of stream;
    /// throws on a partial frame, which means ffmpeg was cut off mid-picture.
    bool read_frame(std::vector<uint8_t>& buffer) {
        const std::size_t got = std::fread(buffer.data(), 1, buffer.size(), file_);
        if (got == 0) {
            return false;
        }
        if (got != buffer.size()) {
            throw std::runtime_error("ffmpeg produced a truncated frame");
        }
        return true;
    }

    void write_frame(const std::vector<uint8_t>& buffer) {
        if (std::fwrite(buffer.data(), 1, buffer.size(), file_) != buffer.size()) {
            throw std::runtime_error("ffmpeg stopped accepting frames");
        }
    }

    std::string read_all() {
        std::string text;
        std::array<char, 4096> chunk{};
        std::size_t got = 0;
        while ((got = std::fread(chunk.data(), 1, chunk.size(), file_)) > 0) {
            text.append(chunk.data(), got);
        }
        return text;
    }

    /// Waits for the child and returns its exit status. Safe to call once.
    int close() {
        const int status = datum_pclose(file_);
        file_ = nullptr;
        return status;
    }

    void close_or_throw(const char* what) {
        if (close() != 0) {
            throw std::runtime_error(std::string("ffmpeg failed while ") + what +
                                     " (run the same file through ffmpeg by hand to see why)");
        }
    }

  private:
    FILE* file_ = nullptr;
};

/// Reads the video as raw rgb24 frames on stdout. `-nostdin` matters: without it
/// ffmpeg competes with us for the terminal's stdin.
///
/// `max_frames` (0 for all) is not an optimisation but a correctness knob: if we
/// simply stopped reading early, ffmpeg would die on a broken pipe and we could
/// no longer tell a deliberate hang-up from a real decode failure. Asking for
/// exactly what we need lets it finish and exit 0.
std::string decode_command(const std::filesystem::path& in, std::size_t max_frames) {
    const std::string limit =
        max_frames == 0 ? std::string() : " -frames:v " + std::to_string(max_frames);
    return finalize(tool("DATUM_FFMPEG", "ffmpeg") + " -v error -nostdin -i " + quote(in) +
                    " -map 0:v:0" + limit + " -f rawvideo -pix_fmt " + kRawPixelFormat + " -");
}

/// Takes raw rgb24 frames on stdin and muxes them with the original's audio.
/// `-map 1:a?` is optional-by-design, so a silent video is not an error.
std::string encode_command(const std::filesystem::path& in,
                           const std::filesystem::path& out,
                           const VideoInfo& info) {
    return finalize(tool("DATUM_FFMPEG", "ffmpeg") + " -v error -y -f rawvideo -pix_fmt " +
                    kRawPixelFormat + " -s " + std::to_string(info.width) + "x" +
                    std::to_string(info.height) + " -r " + info.frame_rate + " -i - -i " +
                    quote(in) + " -map 0:v:0 -map 1:a? -c:v ffv1 -pix_fmt " + kStoredPixelFormat +
                    " -c:a copy " + quote(out));
}

/// The frame's shape without its bytes. capacity() only reads the dimensions, so
/// this is enough to size a payload against a video we have not decoded yet.
Image frame_shape(const VideoInfo& info) {
    Image shape;
    shape.width = info.width;
    shape.height = info.height;
    shape.channels = kRawChannels;
    return shape;
}

/// Decoding frame 0 with an already-probed `info`, so callers that need the rest
/// of the info do not pay for a second probe (which demuxes the whole file).
Image decode_first_frame(const std::filesystem::path& file, const VideoInfo& info) {
    Image frame = frame_shape(info);
    frame.pixels.resize(info.frame_bytes());
    Pipe decoder(decode_command(file, 1), datum_read_mode);
    if (!decoder.read_frame(frame.pixels)) {
        throw std::runtime_error("no frames decoded from " + file.string());
    }
    decoder.close_or_throw("reading the video");
    return frame;
}

std::string find_value(const std::string& text, const std::string& key) {
    const std::string needle = key + "=";
    std::size_t at = text.find(needle);
    // Must be at the start of a line, or "width=" would match "codec_width=".
    while (at != std::string::npos && at != 0 && text[at - 1] != '\n' && text[at - 1] != '\r') {
        at = text.find(needle, at + 1);
    }
    if (at == std::string::npos) {
        return {};
    }
    const std::size_t start = at + needle.size();
    const std::size_t end = text.find_first_of("\r\n", start);
    return text.substr(start, end == std::string::npos ? std::string::npos : end - start);
}

}  // namespace

bool is_video(const std::filesystem::path& file) {
    static const std::array<const char*, 10> kExtensions = {
        ".mp4", ".mkv", ".mov", ".avi", ".webm", ".m4v", ".mpg", ".mpeg", ".wmv", ".flv"};
    const std::string extension = lowercase(file.extension().string());
    return std::find(kExtensions.begin(), kExtensions.end(), extension) != kExtensions.end();
}

VideoInfo probe(const std::filesystem::path& file) {
    // ponytail: -count_packets demuxes the whole file to count frames. That is a
    // second pass over the bytes; drop it and stream-count if it ever hurts.
    const std::string command =
        finalize(tool("DATUM_FFPROBE", "ffprobe") + " -v error -select_streams v:0 -count_packets" +
                 " -show_entries stream=width,height,r_frame_rate,nb_read_packets" +
                 " -of default=noprint_wrappers=1:nokey=0 " + quote(file));

    Pipe probe_pipe(command, "r");
    const std::string output = probe_pipe.read_all();
    probe_pipe.close();  // ffprobe's status adds nothing the parse below does not catch

    VideoInfo info;
    info.width = std::atoi(find_value(output, "width").c_str());
    info.height = std::atoi(find_value(output, "height").c_str());
    if (info.width <= 0 || info.height <= 0) {
        throw std::runtime_error("no video stream in " + file.string() +
                                 " (or ffprobe is missing — set DATUM_FFPROBE to its path)");
    }

    const std::string frames = find_value(output, "nb_read_packets");
    info.frames = frames.empty() ? 0 : static_cast<std::size_t>(std::atoll(frames.c_str()));

    // A rate of 0/0 means the container did not say; 25 is ffmpeg's own default.
    info.frame_rate = find_value(output, "r_frame_rate");
    if (info.frame_rate.empty() || info.frame_rate.rfind("0/", 0) == 0) {
        info.frame_rate = "25";
    }
    return info;
}

Image first_frame(const std::filesystem::path& file) {
    return decode_first_frame(file, probe(file));
}

std::size_t video_capacity(const VideoInfo& info, Mode mode, uint8_t param) {
    const std::size_t per_frame = make_codec(mode, param)->capacity(frame_shape(info));
    const std::size_t total = per_frame * info.frames;
    return total > kHeaderSize ? total - kHeaderSize : 0;
}

VideoStats embed_video(const std::filesystem::path& in,
                       const std::filesystem::path& out,
                       Mode mode,
                       uint8_t param,
                       std::span<const uint8_t> payload) {
    if (lowercase(out.extension().string()) != ".mkv") {
        throw std::runtime_error("refusing to write " + out.string() +
                                 ": only .mkv (lossless FFV1) preserves exact pixel values");
    }

    const VideoInfo info = probe(in);
    const auto codec = make_codec(mode, param);
    const std::size_t per_frame = codec->capacity(frame_shape(info));

    // Extraction reads the header from the first frame alone, so it has to fit
    // there. Enforcing it here keeps the decoder side simple.
    if (per_frame < kHeaderSize) {
        throw std::runtime_error("a " + std::to_string(info.width) + "x" +
                                 std::to_string(info.height) + " frame holds only " +
                                 std::to_string(per_frame) + " bytes in " +
                                 std::string(mode_name(mode)) + " mode, too little for a header");
    }

    Header header;
    header.mode = mode;
    header.param = param;
    header.payload_len = static_cast<uint32_t>(payload.size());
    header.payload_crc = crc32(payload);

    std::vector<uint8_t> stream = serialize(header);
    stream.insert(stream.end(), payload.begin(), payload.end());

    // Only an upfront courtesy — the frame count can be unknown, so the real
    // guarantee is the exhausted() check after the loop.
    if (info.frames > 0 && stream.size() > per_frame * info.frames) {
        throw std::runtime_error("payload needs " + std::to_string(stream.size()) + " bytes but " +
                                 std::to_string(info.frames) + " frames hold only " +
                                 std::to_string(per_frame * info.frames) + " in " +
                                 std::string(mode_name(mode)) + " mode");
    }

    // 0: every frame, the whole video is rewritten
    Pipe decoder(decode_command(in, 0), datum_read_mode);
    Pipe encoder(encode_command(in, out, info), datum_write_mode);

    Image frame = frame_shape(info);
    frame.pixels.resize(info.frame_bytes());
    std::vector<uint8_t> before;

    BitReader source(stream);
    VideoStats stats;
    double sum_squared_error = 0.0;
    std::size_t samples = 0;

    while (decoder.read_frame(frame.pixels)) {
        if (!source.exhausted()) {
            // Untouched frames contribute no error, so only the ones we write
            // into need copying — the distortion is still measured over the
            // whole video, exactly as it is for a still image.
            before = frame.pixels;
            codec->embed(frame, source);
            ++stats.frames_used;
            for (std::size_t i = 0; i < frame.pixels.size(); ++i) {
                const double diff =
                    static_cast<double>(before[i]) - static_cast<double>(frame.pixels[i]);
                sum_squared_error += diff * diff;
            }
        }
        encoder.write_frame(frame.pixels);
        samples += frame.pixels.size();
        ++stats.frames;
    }

    decoder.close_or_throw("reading the video");
    encoder.close_or_throw("writing the video");

    if (stats.frames == 0) {
        throw std::runtime_error("no frames decoded from " + in.string());
    }
    if (!source.exhausted()) {
        throw std::runtime_error("the video ran out " + std::to_string(source.remaining() / 8) +
                                 " bytes before the payload was finished");
    }

    stats.psnr = psnr_from_squared_error(sum_squared_error, samples);
    return stats;
}

Extracted extract_video(const std::filesystem::path& in, std::optional<Mode> mode) {
    const VideoInfo info = probe(in);

    // First pass: one frame, just to learn who wrote this and how long it is.
    Image frame = decode_first_frame(in, info);
    const std::optional<Detected> found = detect_codec(frame, mode);
    if (!found) {
        throw std::runtime_error(
            "no datum payload in the first frame (or the wrong mode was given)");
    }

    const std::size_t needed = (kHeaderSize + found->header.payload_len) * 8;
    const std::size_t per_frame = found->codec->capacity(frame) * 8;
    if (per_frame == 0) {
        throw std::runtime_error("frames are too small to carry a payload in this mode");
    }

    // Second pass: exactly the frames the payload spans, rounded up. capacity()
    // floors to whole bytes, so this can only ever over-ask, never under-ask.
    Pipe decoder(decode_command(in, (needed + per_frame - 1) / per_frame), datum_read_mode);
    BitWriter sink;
    while (sink.size() < needed && decoder.read_frame(frame.pixels)) {
        found->codec->extract(frame, sink, needed);
    }
    decoder.close_or_throw("reading the video");

    if (sink.size() < needed) {
        throw std::runtime_error("the video ended before the payload was complete");
    }
    return verify_payload(found->header, sink.bytes());
}

}  // namespace datum
