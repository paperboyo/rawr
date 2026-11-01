#include <android/native_window_jni.h>
#include <jni.h>
#include <fcntl.h>
#include <unistd.h>

#include <cstdint>
#include <cmath>
#include <exception>
#include <memory>
#include <string>

#include "video/MovMuxer.h"
#include "video/Mp4MetadataPatcher.h"
#include "video/NativeAvRecorder.h"

namespace {
using rawrcam::video::NativeAvRecorder;

NativeAvRecorder* recorder(jlong handle) noexcept {
    return reinterpret_cast<NativeAvRecorder*>(static_cast<intptr_t>(handle));
}

std::string javaString(JNIEnv* env, jstring value) {
    if (!value) return {};
    const char* chars = env->GetStringUTFChars(value, nullptr);
    std::string out = chars ? chars : "";
    if (chars) env->ReleaseStringUTFChars(value, chars);
    return out;
}

rawrcam::video::MovMuxer::CaptureTime readCaptureTime(JNIEnv* env, jobject value) {
    if (!value) throw std::invalid_argument("movie capture time is required");
    const jclass cls = env->GetObjectClass(value);
    if (!cls) throw std::runtime_error("movie capture time transport failed");
    const jfieldID millis = env->GetFieldID(cls, "wallClockUnixMillis", "J");
    const jfieldID offset = millis ? env->GetFieldID(cls, "utcOffsetMinutes", "I") : nullptr;
    env->DeleteLocalRef(cls);
    if (!millis || !offset) throw std::runtime_error("movie capture time transport failed");
    const rawrcam::video::MovMuxer::CaptureTime result{
        env->GetLongField(value, millis), env->GetIntField(value, offset)};
    if (env->ExceptionCheck()) throw std::runtime_error("movie capture time transport failed");
    return result;
}

void throwJava(JNIEnv* env, const char* message) {
    jclass type = env->FindClass("java/lang/IllegalStateException");
    if (type) env->ThrowNew(type, message);
}
}  // namespace

extern "C" JNIEXPORT jlong JNICALL Java_com_rawr_camera_video_NativeAvEngine_nativeStart(
    JNIEnv* env, jobject, jint fd, jint width, jint height, jint fps, jint bitrate, jint intraFrames,
    jint rotationDegrees, jint bitrateMode, jint maxBFrames, jint audioChannels, jint audioBitrate, jint audioPreset,
    jint audioDeviceId, jfloat audioGainDb, jint audioLowCutHz, jint audioPcmBits, jint bitDepth,
    jint timestampPolicy, jstring deviceMake, jstring deviceModel, jstring software, jstring renderProfile, jstring gamut, jstring transfer, jboolean log, jstring indexPath, jdoubleArray gps,
    jobject captureTime) {
    bool ownsFd = false;
    try {
        NativeAvRecorder::Settings settings{};
        settings.width = width;
        settings.height = height;
        settings.fps = fps;
        settings.bitrate = bitrate;
        settings.intraFrames = intraFrames;
        settings.rotationDegrees = rotationDegrees;
        settings.bitrateMode = bitrateMode;
        settings.maxBFrames = maxBFrames;
        settings.audioChannels = audioChannels;
        settings.audioBitrate = audioBitrate;
        settings.audioPreset = audioPreset;
        settings.audioDeviceId = audioDeviceId;
        settings.audioGainDb = audioGainDb;
        settings.audioLowCutHz = audioLowCutHz;
        settings.audioPcmBits = audioPcmBits;
        settings.bitDepth = bitDepth;
        settings.deviceMake = javaString(env, deviceMake);
        settings.deviceModel = javaString(env, deviceModel);
        settings.software = javaString(env, software);
        settings.renderProfile = javaString(env, renderProfile);
        settings.gamut = javaString(env, gamut);
        settings.transfer = javaString(env, transfer);
        settings.log = log;
        settings.indexPath = javaString(env, indexPath);
        settings.captureTime = readCaptureTime(env, captureTime);
        if (gps) {
            if (env->GetArrayLength(gps) != 6) throw std::invalid_argument("invalid movie location payload");
            jdouble values[5];
            env->GetDoubleArrayRegion(gps, 0, 5, values);
            if (env->ExceptionCheck()) throw std::runtime_error("movie location transport failed");
            rawrcam::video::MovMuxer::Location location{values[0], values[1], std::nullopt, std::nullopt, 0};
            if (!std::isnan(values[2])) location.altitudeMeters = values[2];
            if (!std::isnan(values[3])) location.horizontalAccuracyMeters = values[3];
            if (std::isfinite(values[4]) && values[4] > 0 && values[4] <= 253402300799999.0)
                location.fixTimeMillis = static_cast<int64_t>(values[4]);
            settings.location = location;
        }
        settings.timestampPolicy = static_cast<NativeAvRecorder::TimestampPolicy>(timestampPolicy);
        auto instance = std::make_unique<NativeAvRecorder>(fd, settings);
        ownsFd = true;
        instance->start();
        return static_cast<jlong>(reinterpret_cast<intptr_t>(instance.release()));
    } catch (const std::exception& error) {
        if (!ownsFd && fd >= 0) close(fd);
        if (!env->ExceptionCheck()) throwJava(env, error.what());
        return 0;
    }
}

extern "C" JNIEXPORT jintArray JNICALL
Java_com_rawr_camera_video_NativeAvEngine_nativeSupportedTimestampPolicies(JNIEnv* env, jobject) {
    constexpr auto realTime = NativeAvRecorder::TimestampPolicy::PreserveRealtime;
    constexpr auto repeat = NativeAvRecorder::TimestampPolicy::FixedCadenceRepeat;
    jint values[2]{};
    jsize count = 0;
    if (NativeAvRecorder::supportsTimestampPolicy(realTime)) values[count++] = static_cast<jint>(realTime);
    if (NativeAvRecorder::supportsTimestampPolicy(repeat)) values[count++] = static_cast<jint>(repeat);
    jintArray result = env->NewIntArray(count);
    if (result) env->SetIntArrayRegion(result, 0, count, values);
    return result;
}

extern "C" JNIEXPORT jobject JNICALL Java_com_rawr_camera_video_NativeAvEngine_nativeSurface(JNIEnv* env, jobject,
                                                                                             jlong handle) {
    NativeAvRecorder* instance = recorder(handle);
    if (!instance || !instance->inputWindow()) {
        throwJava(env, "native video surface unavailable");
        return nullptr;
    }
    return ANativeWindow_toSurface(env, instance->inputWindow());
}

extern "C" JNIEXPORT jstring JNICALL Java_com_rawr_camera_video_NativeAvEngine_nativeStats(JNIEnv* env, jobject,
                                                                                           jlong handle) {
    NativeAvRecorder* instance = recorder(handle);
    if (!instance) {
        throwJava(env, "native recorder handle is null");
        return nullptr;
    }
    const std::string stats = instance->statsJson();
    return env->NewStringUTF(stats.c_str());
}

extern "C" JNIEXPORT jstring JNICALL Java_com_rawr_camera_video_NativeAvEngine_nativeStop(JNIEnv* env, jobject,
                                                                                          jlong handle) {
    std::unique_ptr<NativeAvRecorder> instance(recorder(handle));
    if (!instance) {
        throwJava(env, "native recorder handle is null");
        return nullptr;
    }
    const bool ok = instance->stop();
    const std::string result =
        std::string("{\"ok\":") + (ok ? "true" : "false") + ",\"stats\":" + instance->statsJson() + "}";
    return env->NewStringUTF(result.c_str());
}

// Finishes a recording interrupted before stop(): rebuilds moov from its sample
// index, then writes device/LOG metadata as stop() would. Takes ownership of fd.
extern "C" JNIEXPORT void JNICALL Java_com_rawr_camera_video_NativeAvEngine_nativeRecover(
    JNIEnv* env, jobject, jint fd, jstring indexPath, jstring deviceMake, jstring deviceModel, jstring software,
    jstring renderProfile, jstring gamut, jstring transfer, jint bitDepth, jboolean log) {
    const int indexFd = open(javaString(env, indexPath).c_str(), O_RDONLY | O_CLOEXEC);
    try {
        if (indexFd < 0) throw std::runtime_error("sample index unavailable");
        rawrcam::video::MovMuxer::recover(fd, indexFd);
        std::string error;
        const bool patched = rawrcam::video::patchMp4DeviceMetadata(
            fd, {javaString(env, deviceMake), javaString(env, deviceModel), javaString(env, software),
                 javaString(env, renderProfile), javaString(env, gamut), javaString(env, transfer),
                 std::to_string(bitDepth), log != JNI_FALSE}, &error);
        if (!patched && log) throw std::runtime_error("LOG metadata finalization failed: " + error);
        (void)fsync(fd);
    } catch (const std::exception& error) {
        throwJava(env, error.what());
    }
    if (indexFd >= 0) close(indexFd);
    close(fd);
}
