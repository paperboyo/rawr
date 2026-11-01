#pragma once

#include <cstddef>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace rawrcam::video {

// Streaming ISO-BMFF / QuickTime writer for one HEVC track and one audio track.
// Samples go straight into a 64-bit mdat as they arrive; finish() appends moov.
// Unlike AMediaMuxer it can carry uncompressed PCM, and the same sample tables
// serve both MP4 (AAC) and MOV (PCM) output. Disk writes run on a background
// thread so a storage stall never blocks the encoder drains or the microphone.
class MovMuxer final {
public:
    enum class Brand { Mp4, QuickTime };

    struct CaptureTime {
        int64_t wallClockUnixMillis;
        int32_t utcOffsetMinutes;
    };

    struct Location {
        double latitude;
        double longitude;
        std::optional<double> altitudeMeters;
        std::optional<double> horizontalAccuracyMeters;
        int64_t fixTimeMillis = 0;
    };

    struct Video {
        // Encoder csd-0: Annex B VPS, SPS and PPS.
        std::vector<uint8_t> annexBConfig;
        int width = 0;
        int height = 0;
        // Media timescale is fps * 1000, so frame-grid PTS map to whole ticks.
        int fps = 30;
        int rotationDegrees = 0;
        // MediaFormat color-standard / color-transfer / color-range; 0 = omit colr.
        int colorStandard = 0;
        int colorTransfer = 0;
        int colorRange = 0;
    };
    struct Audio {
        enum class Codec { Aac, Pcm };
        Codec codec = Codec::Aac;
        int sampleRate = 48'000;
        int channels = 2;
        // AAC: AudioSpecificConfig (csd-0) and nominal bitrate.
        std::vector<uint8_t> aacConfig;
        int bitrate = 0;
        // PCM: little-endian, interleaved; 16/24/32-bit integer or 32-bit float.
        int bitsPerSample = 16;
        bool floatSamples = false;
    };

    // indexFd (optional, owned): append-only sample index, written only after the
    // samples it describes reach fd, so recover() can finish an interrupted file.
    MovMuxer(int fd, Brand brand, int indexFd = -1, std::optional<Location> location = std::nullopt,
             std::optional<CaptureTime> captureTime = std::nullopt);
    ~MovMuxer();

    // Rebuilds moov for a file whose recording stopped without finish(), from its
    // sample index. Truncates anything past the last indexed sample. Throws if the
    // index has no complete video and audio track.
    static void recover(int fd, int indexFd);
    MovMuxer(const MovMuxer&) = delete;
    MovMuxer& operator=(const MovMuxer&) = delete;

    void setVideo(const Video& video);
    void setAudio(const Audio& audio);
    [[nodiscard]] bool hasVideo() const noexcept { return video_.present; }
    [[nodiscard]] bool hasAudio() const noexcept { return audio_.present; }

    // Annex B access unit; written length-prefixed. PTS in microseconds from the movie origin.
    void writeVideo(const uint8_t* data, size_t size, int64_t ptsUs, bool sync);
    // One AAC access unit, or any whole number of PCM frames.
    void writeAudio(const uint8_t* data, size_t size, int64_t ptsUs);
    // Appends moov and fixes the mdat size. Throws on I/O failure or a missing track.
    void finish();

private:
    struct Chunk {
        uint64_t offset = 0;
        uint32_t samples = 0;
    };
    struct Track {
        bool present = false;
        uint32_t timescale = 0;
        std::vector<Chunk> chunks;
        std::vector<uint32_t> sizes;     // Per sample; empty for PCM (fixed size).
        std::vector<int64_t> ptsTicks;   // Per sample (video, AAC) or per chunk (PCM).
        std::vector<uint32_t> syncSamples;
        uint64_t pcmFrames = 0;
    };

    struct Pending {
        std::vector<uint8_t> data;
        std::vector<uint8_t> index;
    };
    struct Recovering {};
    MovMuxer(int fd, Brand brand, Recovering);

    void write(const uint8_t* data, size_t size);
    void indexRecord(uint8_t type, const std::vector<uint8_t>& payload);
    void noteVideo(uint64_t offset, uint32_t size, int64_t ptsUs, bool sync);
    void noteAudio(uint64_t offset, uint32_t size, int64_t ptsUs);
    // Hands staged data and its index records to the writer thread at a sample boundary.
    void flush();
    // Flushes when the staging buffer is full, at video sync samples, or 0.5 s after the
    // last flush, which bounds what an interrupted recording can lose.
    void flushIfDue(bool sync);
    void writerLoop() noexcept;
    void throwIfWriteFailed();
    std::vector<uint8_t> moov() const;
    std::vector<uint8_t> videoTrak(uint32_t movieTimescale) const;
    std::vector<uint8_t> audioTrak(uint32_t movieTimescale) const;

    int fd_ = -1;
    Brand brand_;
    std::optional<Location> location_;
    std::optional<CaptureTime> captureTime_;
    Video videoConfig_;
    Audio audioConfig_;
    Track video_;
    Track audio_;
    uint64_t mdatStart_ = 0;
    uint64_t position_ = 0;
    std::vector<uint8_t> buffer_;
    std::vector<uint8_t> indexBuffer_;
    int indexFd_ = -1;
    int64_t lastFlushNs_ = 0;
    bool replaying_ = false;
    bool finished_ = false;
    std::mutex queueMutex_;
    std::condition_variable queueChanged_;
    std::deque<Pending> queue_;
    size_t queuedBytes_ = 0;
    bool stopWriter_ = false;
    std::string writeError_;
    std::thread writer_;
};

}  // namespace rawrcam::video
