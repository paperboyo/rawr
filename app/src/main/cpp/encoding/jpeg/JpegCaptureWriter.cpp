#include "encoding/jpeg/JpegCaptureWriter.h"
#include "encoding/jpeg/JpegExif.h"

#include <android/log.h>
#include <gainmap/UltrahdrMux.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/system_properties.h>
#include <turbojpeg.h>

#include "StripJpegEncoder.h"
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <limits>
#include <sstream>
#include <thread>
#include <utility>
#include <vector>

#include "diagnostics/logging/RuntimeTraceRecorder.h"
#include "encoding/jpeg/JpegTimingsDescription.h"
#include "encoding/jpeg/JpegTimingsFormat.h"
#include "encoding/jpeg/UltraHdrMapFit.h"

namespace rawrcam::encoding::jpeg {
namespace {

bool writeAll(int fd, const unsigned char* data, std::size_t size) {
    while (size > 0) {
        const ssize_t n = ::write(fd, data, size);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return false;
        data += static_cast<std::size_t>(n);
        size -= static_cast<std::size_t>(n);
    }
    return true;
}

std::string xmlEscape(const std::string& text) {
    std::string out;
    out.reserve(text.size() + 64);
    for (char ch : text) {
        switch (ch) {
            case '&':
                out += "&amp;";
                break;
            case '<':
                out += "&lt;";
                break;
            case '>':
                out += "&gt;";
                break;
            case '"':
                out += "&quot;";
                break;
            case '\'':
                out += "&apos;";
                break;
            default:
                out += ch;
                break;
        }
    }
    return out;
}

std::vector<unsigned char> buildXmp(const std::string& description) {
    static constexpr char kXmpId[] = "http://ns.adobe.com/xap/1.0/";
    const std::string escaped = xmlEscape(description);
    const std::string xml =
        "<x:xmpmeta xmlns:x=\"adobe:ns:meta/\">"
        "<rdf:RDF xmlns:rdf=\"http://www.w3.org/1999/02/22-rdf-syntax-ns#\">"
        "<rdf:Description xmlns:dc=\"http://purl.org/dc/elements/1.1/\">"
        "<dc:description><rdf:Alt><rdf:li xml:lang=\"x-default\">" +
        escaped +
        "</rdf:li></rdf:Alt></dc:description>"
        "</rdf:Description></rdf:RDF></x:xmpmeta>";
    std::vector<unsigned char> payload;
    payload.insert(payload.end(), kXmpId, kXmpId + sizeof(kXmpId));  // includes required NUL
    payload.insert(payload.end(), xml.begin(), xml.end());
    return payload;
}

int tjSubsampling(ChromaSubsampling s) {
    switch (s) {
        case ChromaSubsampling::Yuv444:
            return TJSAMP_444;
        case ChromaSubsampling::Yuv422:
            return TJSAMP_422;
        case ChromaSubsampling::Yuv420:
            return TJSAMP_420;
    }
    return TJSAMP_420;
}

}  // namespace

JpegCaptureWriter::~JpegCaptureWriter() { reset(); }

namespace {
// Pure byte mux (MPF + GContainer/hdrgm XMP + ISO 21496-1); ported constants
// live in gainmap::UltrahdrMux.
 bool assembleUltraHdr(const unsigned char* baseJpeg, std::size_t baseSize, const unsigned char* mapJpeg,
                       std::size_t mapSize, const std::vector<unsigned char>& exif, const JpegCaptureContext& context,
                       std::vector<unsigned char>& out, std::string& error) {
     gainmap::UltrahdrMuxParams params;
     params.gainMapMinLog2 = context.ultraHdr.gainMapMinLog2;
     params.gainMapMaxLog2 = context.ultraHdr.gainMapMaxLog2;
     params.gamma = context.ultraHdr.gamma;
     params.offsetSdr = context.ultraHdr.offsetSdr;
     params.offsetHdr = context.ultraHdr.offsetHdr;
     params.hdrCapacityMinLog2 = context.ultraHdr.hdrCapacityMinLog2;
     params.hdrCapacityMaxLog2 = context.ultraHdr.hdrCapacityMaxLog2;
     params.multiChannel = context.ultraHdr.multiChannelMap;
    gainmap::UltrahdrMuxResult result = gainmap::UltrahdrMux::assemble(
        baseJpeg, baseSize, mapJpeg, mapSize, exif.empty() ? nullptr : exif.data(), exif.size(), params, out);
    if (!result.ok) {
        error = result.error.empty() ? "ultrahdr_mux_failed" : result.error;
        return false;
    }
    return true;
}

// Legacy SDR fallback when the UltraHDR assembly fails (gain map encode or
// mux error, oversize metadata). Writes the already-encoded base JPEG with
// EXIF + XMP so the shot survives as SDR instead of a 0-byte file. Returns
// false only if the fallback itself cannot be written.
bool writeUltraHdrFallback(int fd, const unsigned char* baseJpeg, std::size_t baseSize, JpegCaptureContext& context,
                           std::uint32_t width, std::uint32_t height, double baseMs, std::uint64_t requestId,
                           const std::string& originalError, double& encodeMsOut) {
    // Honest timings: the file is SDR, so the description drops the gainmap
    // stage instead of claiming UltraHDR.
    context.imageDescription = appendProcessingTimes(context, baseMs);
    const auto exif = buildExif(context, width, height);
    const auto xmp = buildXmp(context.imageDescription);
    if (exif.size() + 2u > 65535u || xmp.size() + 2u > 65535u) return false;
    const unsigned char exifHeader[4] = {
        0xFFu,
        0xE1u,
        static_cast<unsigned char>(((exif.size() + 2u) >> 8u) & 0xffu),
        static_cast<unsigned char>((exif.size() + 2u) & 0xffu),
    };
    const unsigned char xmpHeader[4] = {
        0xFFu,
        0xE1u,
        static_cast<unsigned char>(((xmp.size() + 2u) >> 8u) & 0xffu),
        static_cast<unsigned char>((xmp.size() + 2u) & 0xffu),
    };
    if (!writeAll(fd, baseJpeg, 2u) || !writeAll(fd, exifHeader, sizeof(exifHeader)) ||
        !writeAll(fd, exif.data(), exif.size()) || !writeAll(fd, xmpHeader, sizeof(xmpHeader)) ||
        !writeAll(fd, xmp.data(), xmp.size()) || !writeAll(fd, baseJpeg + 2u, baseSize - 2u)) {
        return false;
    }
    encodeMsOut = baseMs;
    __android_log_print(ANDROID_LOG_WARN, "RawrCamNative",
                        "JPEG_UHDR_FALLBACK requestId=%llu original_error=%s fallback=legacy_sdr",
                        (unsigned long long)requestId, originalError.c_str());
    return true;
}
// The strip-parallel encoder (pixels identical to one tj3Compress8 with these settings); false = encode on one
// thread as before. The output, like tj3Compress8's, is released with tj3Free.
bool encodeParallel(const unsigned char* pixels, std::uint32_t width, std::uint32_t height, int quality, int subsamp,
                    std::uint64_t requestId, unsigned char** jpeg, std::size_t* size) {
    // `adb shell setprop debug.rawr.jpeg_strips 0` encodes on one thread (A/B checks of the strip encoder).
    char prop[PROP_VALUE_MAX] = {};
    if (__system_property_get("debug.rawr.jpeg_strips", prop) > 0 && prop[0] == '0') {
        __android_log_print(ANDROID_LOG_INFO, "RawrCamNative", "JPEG_STRIPS requestId=%llu one_thread reason=debug_prop",
                            (unsigned long long)requestId);
        return false;
    }
    int strips = 0;
    std::string why;
    if (encodeStripsRgba8(pixels, static_cast<int>(width), static_cast<int>(height), quality, subsamp, jpeg, size,
                          &strips, &why)) {
        __android_log_print(ANDROID_LOG_INFO, "RawrCamNative", "JPEG_STRIPS requestId=%llu strips=%d",
                            (unsigned long long)requestId, strips);
        return true;
    }
    __android_log_print(ANDROID_LOG_INFO, "RawrCamNative", "JPEG_STRIPS requestId=%llu one_thread reason=%s",
                        (unsigned long long)requestId, why.c_str());
    return false;
}
}  // namespace

bool JpegCaptureWriter::busy() const noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    return busy_;
}

bool JpegCaptureWriter::start(std::uint64_t requestId, const void* rgba8, std::size_t rgbaBytes, std::uint32_t width,
                              std::uint32_t height, JpegCaptureContext context) {
    if (requestId == 0 || !rgba8 || width == 0 || height == 0 || !context.outputFd.valid() || context.quality < 95 ||
        context.quality > 100) {
        __android_log_print(ANDROID_LOG_WARN, "RawrCamNative", "JPEG_WRITER_REJECTED requestId=%llu invalid_args",
                            (unsigned long long)requestId);
        return false;
    }
    const std::uint64_t required = static_cast<std::uint64_t>(width) * height * 4u;
    if (required > std::numeric_limits<std::size_t>::max() || rgbaBytes != static_cast<std::size_t>(required)) {
        __android_log_print(ANDROID_LOG_WARN, "RawrCamNative", "JPEG_WRITER_REJECTED requestId=%llu size_mismatch",
                            (unsigned long long)requestId);
        return false;
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (busy_ || completion_) {
            __android_log_print(ANDROID_LOG_WARN, "RawrCamNative", "JPEG_WRITER_REJECTED requestId=%llu busy=%d",
                                (unsigned long long)requestId, busy_ ? 1 : 0);
            return false;
        }
        busy_ = true;
    }
    joinWorker();
    const auto* pixels = static_cast<const unsigned char*>(rgba8);
    try {
        worker_ = std::thread([this, requestId, pixels, width, height, context = std::move(context)]() mutable {
            // Background priority: file encoding must never preempt camera or
            // preview threads, especially on throttled silicon.
            ::setpriority(PRIO_PROCESS, 0, 10);
            JpegWriteCompletion done{};
            done.requestId = requestId;
            done.displayName = context.displayName;
            done.filmFallbackMemory = context.filmFallbackMemory;
            const int fd = context.outputFd.get();
            diagnostics::RuntimeTraceRecorder::instance().record(diagnostics::RuntimeTraceStage::JpegEncodeBegin, 0,
                                                                 requestId);
            const auto t0 = std::chrono::steady_clock::now();
            tjhandle handle = tj3Init(TJINIT_COMPRESS);
            unsigned char* jpegBuf = nullptr;
            std::size_t jpegSize = 0;
            try {
                if (!handle) {
                    done.error = "tj3Init failed";
                } else if (tj3Set(handle, TJPARAM_QUALITY, context.quality) != 0 ||
                           tj3Set(handle, TJPARAM_SUBSAMP, tjSubsampling(context.subsampling)) != 0 ||
                           // Explicitly select libjpeg-turbo's accurate integer DCT (ISLOW-equivalent).
                           // This is already the TurboJPEG default, but setting FASTDCT=0 makes the
                           // production contract and device diagnostics independent of library defaults.
                           tj3Set(handle, TJPARAM_FASTDCT, 0) != 0) {
                    done.error = tj3GetErrorStr(handle);
                } else if (!encodeParallel(pixels, width, height, context.quality, tjSubsampling(context.subsampling),
                                           requestId, &jpegBuf, &jpegSize) &&
                           tj3Compress8(handle, pixels, static_cast<int>(width), 0, static_cast<int>(height), TJPF_RGBA,
                                        &jpegBuf, &jpegSize) != 0) {
                    done.error = tj3GetErrorStr(handle);
                } else if (jpegSize < 2 || jpegBuf[0] != 0xFFu || jpegBuf[1] != 0xD8u) {
                    done.error = "libjpeg-turbo returned invalid JPEG SOI";
                } else {
                    const auto encodedAt = std::chrono::steady_clock::now();
                    done.encodeMs = std::chrono::duration<double, std::milli>(encodedAt - t0).count();
                    context.imageDescription = appendProcessingTimes(context, done.encodeMs);
                    const auto exif = buildExif(context, width, height);
                    const auto xmp = buildXmp(context.imageDescription);
                    if (exif.size() + 2u > 65535u || xmp.size() + 2u > 65535u) {
                        done.error = "metadata APP1 payload too large";
                    } else {
                        const unsigned char exifHeader[4] = {
                            0xFFu,
                            0xE1u,
                            static_cast<unsigned char>(((exif.size() + 2u) >> 8u) & 0xffu),
                            static_cast<unsigned char>((exif.size() + 2u) & 0xffu),
                        };
                        const unsigned char xmpHeader[4] = {
                            0xFFu,
                            0xE1u,
                            static_cast<unsigned char>(((xmp.size() + 2u) >> 8u) & 0xffu),
                            static_cast<unsigned char>((xmp.size() + 2u) & 0xffu),
                        };
                        if (!writeAll(fd, jpegBuf, 2u) || !writeAll(fd, exifHeader, sizeof(exifHeader)) ||
                            !writeAll(fd, exif.data(), exif.size()) || !writeAll(fd, xmpHeader, sizeof(xmpHeader)) ||
                            !writeAll(fd, xmp.data(), xmp.size()) || !writeAll(fd, jpegBuf + 2u, jpegSize - 2u)) {
                            done.error = "JPEG write failed: " + std::string(std::strerror(errno));
                        }
                    }
                }
            } catch (const std::exception& error) {
                done.error = error.what();
            } catch (...) {
                done.error = "jpeg_encoding_failed";
            }
            if (jpegBuf) tj3Free(jpegBuf);
            if (handle) tj3Destroy(handle);
            if (done.error.empty()) {
                const auto fs0 = std::chrono::steady_clock::now();
                if (::fsync(fd) != 0) done.error = "fsync failed: " + std::string(std::strerror(errno));
                const auto fs1 = std::chrono::steady_clock::now();
                done.fsyncMs = std::chrono::duration<double, std::milli>(fs1 - fs0).count();
            }
            struct stat st{};
            if (done.error.empty() && ::fstat(fd, &st) == 0 && st.st_size >= 0)
                done.fileBytes = static_cast<std::uint64_t>(st.st_size);
            // Closed explicitly: a failed close can mean lost data on some file systems.
            if (::close(context.outputFd.release()) != 0 && done.error.empty())
                done.error = "close failed: " + std::string(std::strerror(errno));
            done.success = done.error.empty();
            if (done.success) {
                __android_log_print(ANDROID_LOG_INFO, "RawrCamNative",
                                    "JPEG_WRITE_DONE requestId=%llu bytes=%llu encodeMs=%.1f fsyncMs=%.1f",
                                    (unsigned long long)done.requestId, (unsigned long long)done.fileBytes,
                                    done.encodeMs, done.fsyncMs);
            } else {
                __android_log_print(ANDROID_LOG_ERROR, "RawrCamNative", "JPEG_WRITE_FAIL requestId=%llu error=%s",
                                    (unsigned long long)done.requestId, done.error.c_str());
            }
            diagnostics::RuntimeTraceRecorder::instance().record(diagnostics::RuntimeTraceStage::JpegEncodeEnd, 0,
                                                                 requestId, -1, 0, 0, done.success ? 1u : 0u);
            std::lock_guard<std::mutex> lock(mutex_);
            completion_ = std::move(done);
            busy_ = false;
        });
    } catch (...) {
        // The unstarted closure, which owns the destination, has already closed it.
        std::lock_guard<std::mutex> lock(mutex_);
        busy_ = false;
        return false;
    }
    return true;
}

bool JpegCaptureWriter::startUltraHdr(std::uint64_t requestId, const void* rgba8, std::size_t rgbaBytes,
                                      std::uint32_t width, std::uint32_t height, const void* mapRgba8,
                                      std::size_t mapBytes, std::uint32_t mapWidth, std::uint32_t mapHeight,
                                      JpegCaptureContext context) {
    if (!context.ultraHdr.enabled) {
        return start(requestId, rgba8, rgbaBytes, width, height, std::move(context));
    }
    if (requestId == 0 || !rgba8 || width == 0 || height == 0 || !mapRgba8 || mapWidth == 0 || mapHeight == 0 ||
        !context.outputFd.valid() || context.quality < 95 || context.quality > 100) {
        __android_log_print(ANDROID_LOG_WARN, "RawrCamNative", "JPEG_UHDR_REJECTED requestId=%llu invalid_args",
                            (unsigned long long)requestId);
        return false;
    }
    const std::uint64_t required = static_cast<std::uint64_t>(width) * height * 4u;
    const std::uint64_t mapRequired = static_cast<std::uint64_t>(mapWidth) * mapHeight * 4u;
    if (required > std::numeric_limits<std::size_t>::max() || rgbaBytes != static_cast<std::size_t>(required) ||
        mapRequired > std::numeric_limits<std::size_t>::max() / 2u ||
        (mapBytes != static_cast<std::size_t>(mapRequired) && mapBytes != static_cast<std::size_t>(mapRequired * 2u))) {
        __android_log_print(ANDROID_LOG_WARN, "RawrCamNative", "JPEG_UHDR_REJECTED requestId=%llu size_mismatch",
                            (unsigned long long)requestId);
        return false;
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (busy_ || completion_) {
            __android_log_print(ANDROID_LOG_WARN, "RawrCamNative", "JPEG_UHDR_REJECTED requestId=%llu busy=%d",
                                (unsigned long long)requestId, busy_ ? 1 : 0);
            return false;
        }
        busy_ = true;
    }
    joinWorker();
    const auto* pixels = static_cast<const unsigned char*>(rgba8);
    const auto* mapPixels = static_cast<const unsigned char*>(mapRgba8);
    const bool hasCaptureColor = mapBytes == mapRequired * 2u;
    const int mapQuality = hasCaptureColor ? 95 : std::clamp(context.ultraHdr.gainmapQuality, 1, 100);
    try {
        worker_ = std::thread([this, requestId, pixels, width, height, mapPixels, mapWidth, mapHeight, mapQuality,
                               hasCaptureColor, context = std::move(context)]() mutable {
            ::setpriority(PRIO_PROCESS, 0, 10);
            JpegWriteCompletion done{};
            done.requestId = requestId;
            done.displayName = context.displayName;
            done.filmFallbackMemory = context.filmFallbackMemory;
            const int fd = context.outputFd.get();
            diagnostics::RuntimeTraceRecorder::instance().record(diagnostics::RuntimeTraceStage::JpegEncodeBegin, 0,
                                                                 requestId);
            const auto t0 = std::chrono::steady_clock::now();
            tjhandle baseHandle = tj3Init(TJINIT_COMPRESS);
            tjhandle mapHandle = tj3Init(TJINIT_COMPRESS);
            unsigned char* baseBuf = nullptr;
            std::size_t baseSize = 0;
            unsigned char* mapBuf = nullptr;
            std::size_t mapSize = 0;
            double baseMs = 0.0;
            double mapMs = 0.0;
            try {
                if (!baseHandle || !mapHandle) {
                    done.error = "tj3Init failed";
                } else if (tj3Set(baseHandle, TJPARAM_QUALITY, context.quality) != 0 ||
                           tj3Set(baseHandle, TJPARAM_SUBSAMP, tjSubsampling(context.subsampling)) != 0 ||
                           tj3Set(baseHandle, TJPARAM_FASTDCT, 0) != 0) {
                    done.error = tj3GetErrorStr(baseHandle);
                 } else if (tj3Set(mapHandle, TJPARAM_QUALITY, mapQuality) != 0 ||
                            // Gain map is multi-channel RGB: 4:4:4 keeps
                            // chroma full-res so subsample ringing can't
                            // print a border along boost shoulders. The
                            // writer fits the ceiling + shoulder + chroma
                            // knee on a copy just above (fitAndRemapMap).
                            tj3Set(mapHandle, TJPARAM_SUBSAMP, TJSAMP_444) != 0 ||
                           tj3Set(mapHandle, TJPARAM_FASTDCT, 0) != 0) {
                    done.error = tj3GetErrorStr(mapHandle);
                } else {
                     // The base encode runs beside the fit + map encode: both
                     // only read `pixels`, so the file is byte-identical to
                     // the serial order and the critical path drops by the
                     // base encode.
                     std::string baseError;
                     std::thread baseWorker([&] {
                         ::setpriority(PRIO_PROCESS, 0, 10);
                         if (!encodeParallel(pixels, width, height, context.quality,
                                             tjSubsampling(context.subsampling), requestId, &baseBuf, &baseSize) &&
                             tj3Compress8(baseHandle, pixels, static_cast<int>(width), 0, static_cast<int>(height),
                                          TJPF_RGBA, &baseBuf, &baseSize) != 0) {
                             baseError = tj3GetErrorStr(baseHandle);
                         } else if (baseSize < 2 || baseBuf[0] != 0xFFu || baseBuf[1] != 0xD8u) {
                             baseError = "libjpeg-turbo returned invalid base JPEG SOI";
                         }
                         baseMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0)
                                      .count();
                     });
                     struct JoinOnExit {
                         std::thread& thread;
                         ~JoinOnExit() {
                             if (thread.joinable()) thread.join();
                         }
                     } joinBase{baseWorker};
                     std::string uhError;
                     // Single-pass adaptive ceiling: fit + shoulder + chroma
                     // knee on a worker-local copy (caller's readback stays
                     // untouched for telemetry). Metadata follows the fit.
                     std::vector<unsigned char> fittedMap;
                     float fittedMax = context.ultraHdr.gainMapMaxLog2;
                     const unsigned char* mapEncodePixels = mapPixels;
                     const auto fitT0 = std::chrono::steady_clock::now();
                     const bool didFit =
                         fitAndRemapMap(mapPixels, mapWidth, mapHeight, context.ultraHdr, fittedMap, fittedMax,
                                        pixels, width, height);
                     bool colorRestored = false;
                     if (didFit) {
                         mapEncodePixels = fittedMap.data();
                         context.ultraHdr.gainMapMaxLog2 = fittedMax;
                         context.ultraHdr.hdrCapacityMaxLog2 = fittedMax;
                         if (hasCaptureColor) {
                             const auto* chromaticity = mapPixels + size_t(mapWidth) * mapHeight * 4;
                             colorRestored = restoreHighlightColor(
                                 fittedMap, chromaticity, mapWidth, mapHeight, pixels, width, height,
                                 context.ultraHdr, fittedMax, context.ultraHdr.gainMapMinLog2,
                                 context.ultraHdr.gainMapMaxLog2);
                         }
                     }
                     const auto fitT1 = std::chrono::steady_clock::now();
                     __android_log_print(ANDROID_LOG_INFO, "RawrCamNative",
                                         "JPEG_UHDR_FIT requestId=%llu fit=%d max=%.2f base=%.2f/%.2f fitMs=%.1f mapPx=%ux%u color=%d range=%.2f/%.2f",
                                         (unsigned long long)requestId, didFit ? 1 : 0, fittedMax,
                                         context.ultraHdr.baseShare, context.ultraHdr.baseExponent,
                                         std::chrono::duration<double, std::milli>(fitT1 - fitT0).count(), mapWidth,
                                         mapHeight, colorRestored ? 1 : 0, context.ultraHdr.gainMapMinLog2,
                                         context.ultraHdr.gainMapMaxLog2);
                     if (!encodeParallel(mapEncodePixels, mapWidth, mapHeight, mapQuality, TJSAMP_444, requestId,
                                         &mapBuf, &mapSize) &&
                         tj3Compress8(mapHandle, mapEncodePixels, static_cast<int>(mapWidth), 0,
                                      static_cast<int>(mapHeight), TJPF_RGBA, &mapBuf, &mapSize) != 0) {
                        uhError = tj3GetErrorStr(mapHandle);
                    } else if (mapSize < 2 || mapBuf[0] != 0xFFu || mapBuf[1] != 0xD8u) {
                        uhError = "libjpeg-turbo returned invalid gain map JPEG SOI";
                    }
                    mapMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - fitT0).count();
                    baseWorker.join();
                    const double bothMs =
                        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
                    __android_log_print(ANDROID_LOG_INFO, "RawrCamNative",
                                        "JPEG_UHDR_ENCODE requestId=%llu baseMs=%.1f mapPathMs=%.1f wallMs=%.1f",
                                        (unsigned long long)requestId, baseMs, mapMs, bothMs);
                    if (!baseError.empty()) {
                        done.error = baseError;
                    } else if (uhError.empty()) {
                        // Printed JPEG number is the wall time of both turbo
                        // encodes (they overlap); the mux below folds into
                        // done.encodeMs (logcat-exact) and is excluded from
                        // the printed total by contract.
                        context.imageDescription = appendProcessingTimes(context, bothMs, mapMs, true);
                        const auto exif = buildExif(context, width, height);
                        if (exif.size() + 2u > 65535u) {
                            uhError = "metadata APP1 payload too large";
                        } else {
                            std::vector<unsigned char> file;
                            std::string muxError;
                            if (!assembleUltraHdr(baseBuf, baseSize, mapBuf, mapSize, exif, context, file, muxError)) {
                                uhError = muxError;
                            } else {
                                const auto encodedAt = std::chrono::steady_clock::now();
                                done.encodeMs = std::chrono::duration<double, std::milli>(encodedAt - t0).count();
                                if (!writeAll(fd, file.data(), file.size())) {
                                    uhError = "JPEG write failed: " + std::string(std::strerror(errno));
                                }
                            }
                        }
                    }
                    if (baseError.empty() && !uhError.empty()) {
                        // Degrade to SDR instead of losing the shot: the base
                        // pixels are already encoded, so splice EXIF + XMP
                        // around them exactly like the legacy path.
                        double fallbackEncodeMs = 0.0;
                        if (writeUltraHdrFallback(fd, baseBuf, baseSize, context, width, height, baseMs, requestId,
                                                  uhError, fallbackEncodeMs)) {
                            done.encodeMs = fallbackEncodeMs;
                        } else {
                            done.error = uhError;
                        }
                    }
                }
            } catch (const std::exception& error) {
                done.error = error.what();
            } catch (...) {
                done.error = "jpeg_encoding_failed";
            }
            if (baseBuf) tj3Free(baseBuf);
            if (mapBuf) tj3Free(mapBuf);
            if (baseHandle) tj3Destroy(baseHandle);
            if (mapHandle) tj3Destroy(mapHandle);
            if (done.error.empty()) {
                const auto fs0 = std::chrono::steady_clock::now();
                if (::fsync(fd) != 0) done.error = "fsync failed: " + std::string(std::strerror(errno));
                const auto fs1 = std::chrono::steady_clock::now();
                done.fsyncMs = std::chrono::duration<double, std::milli>(fs1 - fs0).count();
            }
            struct stat st{};
            if (done.error.empty() && ::fstat(fd, &st) == 0 && st.st_size >= 0)
                done.fileBytes = static_cast<std::uint64_t>(st.st_size);
            // Closed explicitly: a failed close can mean lost data on some file systems.
            if (::close(context.outputFd.release()) != 0 && done.error.empty())
                done.error = "close failed: " + std::string(std::strerror(errno));
            done.success = done.error.empty();
            if (done.success) {
                __android_log_print(ANDROID_LOG_INFO, "RawrCamNative",
                                    "JPEG_UHDR_DONE requestId=%llu bytes=%llu encodeMs=%.1f fsyncMs=%.1f",
                                    (unsigned long long)done.requestId, (unsigned long long)done.fileBytes,
                                    done.encodeMs, done.fsyncMs);
            } else {
                __android_log_print(ANDROID_LOG_ERROR, "RawrCamNative", "JPEG_UHDR_FAIL requestId=%llu error=%s",
                                    (unsigned long long)done.requestId, done.error.c_str());
            }
            diagnostics::RuntimeTraceRecorder::instance().record(diagnostics::RuntimeTraceStage::JpegEncodeEnd, 0,
                                                                 requestId, -1, 0, 0, done.success ? 1u : 0u);
            std::lock_guard<std::mutex> lock(mutex_);
            completion_ = std::move(done);
            busy_ = false;
        });
    } catch (...) {
        // The unstarted closure, which owns the destination, has already closed it.
        std::lock_guard<std::mutex> lock(mutex_);
        busy_ = false;
        return false;
    }
    return true;
}

std::optional<JpegWriteCompletion> JpegCaptureWriter::pollCompletion() {
    std::optional<JpegWriteCompletion> out;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!completion_) return std::nullopt;
        out = std::move(completion_);
        completion_.reset();
    }
    joinWorker();
    return out;
}

void JpegCaptureWriter::reset() noexcept {
    joinWorker();
    std::lock_guard<std::mutex> lock(mutex_);
    busy_ = false;
    completion_.reset();
}

void JpegCaptureWriter::joinWorker() {
    if (worker_.joinable()) worker_.join();
}

}  // namespace rawrcam::encoding::jpeg
