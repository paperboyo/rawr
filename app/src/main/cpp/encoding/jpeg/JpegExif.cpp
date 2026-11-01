#include "encoding/jpeg/JpegExif.h"
#include "encoding/SoftwareName.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <limits>
#include <string>
#include <utility>

namespace rawrcam::encoding::jpeg {
namespace {

void put16(std::vector<unsigned char>& out, std::uint16_t v) {
    out.push_back(static_cast<unsigned char>(v & 0xffu));
    out.push_back(static_cast<unsigned char>((v >> 8u) & 0xffu));
}
void put32(std::vector<unsigned char>& out, std::uint32_t v) {
    out.push_back(static_cast<unsigned char>(v & 0xffu));
    out.push_back(static_cast<unsigned char>((v >> 8u) & 0xffu));
    out.push_back(static_cast<unsigned char>((v >> 16u) & 0xffu));
    out.push_back(static_cast<unsigned char>((v >> 24u) & 0xffu));
}
void patch32(std::vector<unsigned char>& out, std::size_t pos, std::uint32_t v) {
    out[pos + 0] = static_cast<unsigned char>(v & 0xffu);
    out[pos + 1] = static_cast<unsigned char>((v >> 8u) & 0xffu);
    out[pos + 2] = static_cast<unsigned char>((v >> 16u) & 0xffu);
    out[pos + 3] = static_cast<unsigned char>((v >> 24u) & 0xffu);
}

struct IfdEntry {
    std::uint16_t tag = 0;
    std::uint16_t type = 0;
    std::uint32_t count = 0;
    std::vector<unsigned char> data;
};

std::vector<unsigned char> asciiData(const std::string& s) {
    std::vector<unsigned char> out(s.begin(), s.end());
    out.push_back(0);
    return out;
}
std::vector<unsigned char> shortData(std::uint16_t v) {
    std::vector<unsigned char> out;
    put16(out, v);
    return out;
}
std::vector<unsigned char> longData(std::uint32_t v) {
    std::vector<unsigned char> out;
    put32(out, v);
    return out;
}
std::vector<unsigned char> rationalData(double value, std::uint32_t denominator = 1000000u) {
    if (!std::isfinite(value) || value < 0.0) value = 0.0;
    const double scaled = std::round(value * static_cast<double>(denominator));
    const auto numerator = static_cast<std::uint32_t>(
        std::clamp(scaled, 0.0, static_cast<double>(std::numeric_limits<std::uint32_t>::max())));
    std::vector<unsigned char> out;
    put32(out, numerator);
    put32(out, denominator);
    return out;
}

std::string exifDateTime(std::int64_t millis, std::int16_t offsetMinutes) {
    if (millis <= 0) return {};
    const time_t localSeconds = static_cast<time_t>(millis / 1000 + static_cast<std::int64_t>(offsetMinutes) * 60);
    struct tm local{};
    if (!gmtime_r(&localSeconds, &local)) return {};
    char buf[20]{};
    if (std::strftime(buf, sizeof(buf), "%Y:%m:%d %H:%M:%S", &local) == 0) return {};
    return std::string(buf);
}

std::string exifOffset(std::int16_t offsetMinutes) {
    const int total = static_cast<int>(offsetMinutes);
    const char sign = total < 0 ? '-' : '+';
    const int absMinutes = std::abs(total);
    char buf[8]{};
    const int hours = std::min(absMinutes / 60, 99);
    const int minutes = std::min(absMinutes % 60, 59);
    std::snprintf(buf, sizeof(buf), "%c%02d:%02d", sign, hours, minutes);
    return std::string(buf);
}

IfdEntry rationals(std::uint16_t tag, const std::uint32_t (*pairs)[2], std::uint32_t count) {
    IfdEntry entry{tag, 5, count, {}};
    for (std::uint32_t i = 0; i < count; ++i) {
        put32(entry.data, pairs[i][0]);
        put32(entry.data, pairs[i][1]);
    }
    return entry;
}
IfdEntry raw(std::uint16_t tag, std::uint16_t type, const void* data, std::uint32_t count) {
    const auto* begin = static_cast<const unsigned char*>(data);
    return {tag, type, count, {begin, begin + count}};
}

std::vector<IfdEntry> gpsIfd(const tinydng_gps& gps) {
    const char latitudeRef[2]{gps.latitude_ref, 0}, longitudeRef[2]{gps.longitude_ref, 0};
    const char speedRef[2]{gps.speed_ref, 0}, trackRef[2]{gps.track_ref, 0};
    std::vector<IfdEntry> ifd;
    if (gps.has_version) ifd.push_back(raw(0, 1, gps.version, 4));
    ifd.push_back(raw(1, 2, latitudeRef, 2));
    ifd.push_back(rationals(2, gps.latitude, 3));
    ifd.push_back(raw(3, 2, longitudeRef, 2));
    ifd.push_back(rationals(4, gps.longitude, 3));
    if (gps.has_altitude) {
        ifd.push_back(raw(5, 1, &gps.altitude_ref, 1));
        ifd.push_back(rationals(6, &gps.altitude, 1));
    }
    if (gps.has_time) ifd.push_back(rationals(7, gps.timestamp, 3));
    if (gps.has_speed) {
        ifd.push_back(raw(12, 2, speedRef, 2));
        ifd.push_back(rationals(13, &gps.speed, 1));
    }
    if (gps.has_track) {
        ifd.push_back(raw(14, 2, trackRef, 2));
        ifd.push_back(rationals(15, &gps.track, 1));
    }
    if (gps.map_datum[0]) ifd.push_back(raw(18, 2, gps.map_datum, std::uint32_t(std::strlen(gps.map_datum) + 1)));
    if (gps.processing_method_size) ifd.push_back(raw(27, 7, gps.processing_method, gps.processing_method_size));
    if (gps.has_time) ifd.push_back(raw(29, 2, gps.datestamp, 11));
    if (gps.has_horizontal_error) ifd.push_back(rationals(31, &gps.horizontal_error, 1));
    return ifd;
}

}  // namespace

// Builds a compact EXIF APP1 payload. TIFF offsets are relative to the TIFF header
// immediately following the six-byte "Exif\0\0" signature.
std::vector<unsigned char> buildExif(const JpegCaptureContext& c, std::uint32_t width, std::uint32_t height) {
    const std::string dateTime = exifDateTime(c.wallClockUnixMillis, c.utcOffsetMinutes);
    const std::string offset = exifOffset(c.utcOffsetMinutes);
    const std::string subsec =
        c.wallClockUnixMillis > 0 ? std::to_string(c.wallClockUnixMillis % 1000 + 1000).substr(1) : std::string{};

    std::vector<IfdEntry> ifd0;
    if (!c.imageDescription.empty()) {
        ifd0.push_back(
            {0x010E, 2, static_cast<std::uint32_t>(c.imageDescription.size() + 1), asciiData(c.imageDescription)});
    }
    if (!c.deviceMake.empty())
        ifd0.push_back({0x010F, 2, static_cast<std::uint32_t>(c.deviceMake.size() + 1), asciiData(c.deviceMake)});
    if (!c.deviceModel.empty())
        ifd0.push_back({0x0110, 2, static_cast<std::uint32_t>(c.deviceModel.size() + 1), asciiData(c.deviceModel)});
    ifd0.push_back({0x0112, 3, 1, shortData(c.exifOrientation)});
    ifd0.push_back({0x0213, 3, 1, shortData(1)});
    const std::string software = rawrcam::encoding::kSoftwareName;
    ifd0.push_back({0x0131, 2, static_cast<std::uint32_t>(software.size() + 1), asciiData(software)});
    if (!dateTime.empty()) ifd0.push_back({0x0132, 2, 20, asciiData(dateTime)});
    // ExifIFDPointer placeholder; value is patched after IFD0 layout is known.
    ifd0.push_back({0x8769, 4, 1, longData(0)});
    if (c.gps) ifd0.push_back({0x8825, 4, 1, longData(0)});

    // A re-rendered DNG keeps its source capture time; DateTime stays the
    // render (file modification) time, qualified by OffsetTime.
    const bool sourceTime = !c.dateTimeOriginal.empty();
    const std::string original = sourceTime ? c.dateTimeOriginal : dateTime;
    const std::string digitized = sourceTime && !c.dateTimeDigitized.empty() ? c.dateTimeDigitized : original;
    const std::string originalOffset = sourceTime ? c.offsetTimeOriginal : offset;
    const std::string originalSubsec = sourceTime ? c.subsecTimeOriginal : subsec;
    auto ascii = [](std::uint16_t tag, const std::string& s) {
        return IfdEntry{tag, 2, static_cast<std::uint32_t>(s.size() + 1), asciiData(s)};
    };

    std::vector<IfdEntry> exif;
    if (c.exposureTimeNs > 0)
        exif.push_back({0x829A, 5, 1, rationalData(static_cast<double>(c.exposureTimeNs) / 1.0e9, 1000000u)});
    if (c.aperture && *c.aperture > 0.0f) exif.push_back({0x829D, 5, 1, rationalData(*c.aperture, 10000u)});
    if (c.sensitivity > 0 && c.sensitivity <= 65535)
        exif.push_back({0x8827, 3, 1, shortData(static_cast<std::uint16_t>(c.sensitivity))});
    const std::array<uint8_t, 4> version = c.sourceExifVersion.value_or(
        c.gps ? std::array<uint8_t, 4>{'0', '3', '0', '0'} : std::array<uint8_t, 4>{'0', '2', '3', '2'});
    exif.push_back({0x9000, 7, 4, {version.begin(), version.end()}});
    exif.push_back({0x9101, 7, 4, {1, 2, 3, 0}});
    if (!original.empty()) {
        exif.push_back(ascii(0x9003, original));
        exif.push_back(ascii(0x9004, digitized));
        if (!originalOffset.empty()) exif.push_back(ascii(0x9011, originalOffset));
    }
    if (sourceTime && !dateTime.empty()) exif.push_back(ascii(0x9010, offset));
    if (!originalSubsec.empty()) exif.push_back(ascii(0x9291, originalSubsec));
    if (c.focalLengthMm && *c.focalLengthMm > 0.0f)
        exif.push_back({0x920A, 5, 1, rationalData(*c.focalLengthMm, 10000u)});
    if (c.focalLength35mm && *c.focalLength35mm > 0) exif.push_back({0xA405, 3, 1, shortData(*c.focalLength35mm)});
    if (c.lensSpecification) {
        std::vector<unsigned char> spec;
        for (double v : *c.lensSpecification) {
            const auto r = rationalData(v, 10000u);
            spec.insert(spec.end(), r.begin(), r.end());
        }
        exif.push_back({0xA432, 5, 4, spec});
    }
    if (!c.lensMake.empty()) exif.push_back(ascii(0xA433, c.lensMake));
    if (!c.lensModel.empty()) exif.push_back(ascii(0xA434, c.lensModel));
    exif.push_back({0xA001, 3, 1, shortData(1)});  // sRGB
    exif.push_back({0xA000, 7, 4, {'0', '1', '0', '0'}});
    exif.push_back({0xA002, 4, 1, longData(width)});
    exif.push_back({0xA003, 4, 1, longData(height)});

    std::vector<unsigned char> tiff;
    tiff.insert(tiff.end(), {'I', 'I'});
    put16(tiff, 42);
    put32(tiff, 8);

    auto appendIfd = [&](const std::vector<IfdEntry>& entries) -> std::uint32_t {
        const std::uint32_t ifdOffset = static_cast<std::uint32_t>(tiff.size());
        put16(tiff, static_cast<std::uint16_t>(entries.size()));
        const std::size_t tableStart = tiff.size();
        tiff.resize(tiff.size() + entries.size() * 12u + 4u, 0);
        std::vector<std::pair<std::size_t, std::vector<unsigned char>>> external;
        for (std::size_t i = 0; i < entries.size(); ++i) {
            const auto& e = entries[i];
            const std::size_t p = tableStart + i * 12u;
            tiff[p + 0] = static_cast<unsigned char>(e.tag & 0xffu);
            tiff[p + 1] = static_cast<unsigned char>(e.tag >> 8u);
            tiff[p + 2] = static_cast<unsigned char>(e.type & 0xffu);
            tiff[p + 3] = static_cast<unsigned char>(e.type >> 8u);
            patch32(tiff, p + 4, e.count);
            if (e.data.size() <= 4u) {
                std::copy(e.data.begin(), e.data.end(), tiff.begin() + static_cast<std::ptrdiff_t>(p + 8));
            } else {
                external.emplace_back(p + 8, e.data);
            }
        }
        for (auto& [offsetPos, bytes] : external) {
            patch32(tiff, offsetPos, static_cast<std::uint32_t>(tiff.size()));
            tiff.insert(tiff.end(), bytes.begin(), bytes.end());
            if ((tiff.size() & 1u) != 0u) tiff.push_back(0);
        }
        return ifdOffset;
    };

    std::sort(ifd0.begin(), ifd0.end(), [](const IfdEntry& a, const IfdEntry& b) { return a.tag < b.tag; });
    std::sort(exif.begin(), exif.end(), [](const IfdEntry& a, const IfdEntry& b) { return a.tag < b.tag; });
    const std::uint32_t ifd0Offset = appendIfd(ifd0);
    const std::uint32_t exifOffset = appendIfd(exif);
    const std::uint32_t gpsOffset = c.gps ? appendIfd(gpsIfd(*c.gps)) : 0;
    // Locate ExifIFDPointer entry in IFD0 and patch its value.
    const std::size_t countPos = ifd0Offset;
    const std::uint16_t count =
        static_cast<std::uint16_t>(tiff[countPos] | (static_cast<std::uint16_t>(tiff[countPos + 1]) << 8u));
    for (std::uint16_t i = 0; i < count; ++i) {
        const std::size_t p = countPos + 2u + static_cast<std::size_t>(i) * 12u;
        const std::uint16_t tag = static_cast<std::uint16_t>(tiff[p] | (static_cast<std::uint16_t>(tiff[p + 1]) << 8u));
        if (tag == 0x8769u) patch32(tiff, p + 8u, exifOffset);
        if (tag == 0x8825u) patch32(tiff, p + 8u, gpsOffset);
    }

    std::vector<unsigned char> payload{'E', 'x', 'i', 'f', 0, 0};
    payload.insert(payload.end(), tiff.begin(), tiff.end());
    return payload;
}

}  // namespace rawrcam::encoding::jpeg
