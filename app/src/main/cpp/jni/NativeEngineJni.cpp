#include <android/asset_manager_jni.h>
#include <android/log.h>
#include <sys/system_properties.h>
#include <tonemap/TonemapMath.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

#include "camera/CameraProbe.h"
#include "camera/CameraMetadataTags.h"
#include "camera/VendorCameraGate.h"
#include "camera/CameraProfileJson.h"
#include "camera/CameraVideoCapabilities.h"
#include "camera/NativeCameraController.h"
#include "capture/CaptureRequest.h"
#include "color/DcpProfile.h"
#include "diagnostics/logging/RuntimeTraceRecorder.h"
#include "jni/CaptureGps.h"
#include "jni/DevelopSettingsReader.h"
#include "jni/SessionEngineApi.h"
#include "tonemap/ColorRenderProfile.h"
#include "vulkan/VulkanDispatch.h"

namespace {
using rawrcam::session::EngineHandle;
EngineHandle handle(jlong h) noexcept { return reinterpret_cast<EngineHandle>(static_cast<intptr_t>(h)); }
std::string jString(JNIEnv* env, jstring value) {
    if (!value) return {};
    const char* c = env->GetStringUTFChars(value, nullptr);
    std::string out(c ? c : "");
    if (c) env->ReleaseStringUTFChars(value, c);
    return out;
}
bool multiframeChromaDenoise(JNIEnv* env, jfloatArray values) {
    const jsize count = values ? env->GetArrayLength(values) : 0;
    if (count != 23 && count != 25 && count != 26 && count != 28 && count != 29) return false;
    jfloat flag = 0.0f;
    env->GetFloatArrayRegion(values, 22, 1, &flag);
    return flag > 0.5f;
}
rawrcam::capture::multiframe::MultiframeTuning multiframeTuning(JNIEnv* env, jfloatArray values) {
    rawrcam::capture::multiframe::MultiframeTuning out{};
    if (!values) return out;
    const jsize count = env->GetArrayLength(values);
    // 22 tuning values, optionally followed by the multiframe chroma-denoise
    // flag (read separately by multiframeChromaDenoise()), then the merge
    // algorithm, HDR+ strength, (optionally) HDR+ tile size and (optionally)
    // the bracket EV (headroom limit), long-frame count and (optionally) the
    // long-frame EV.
    if (count != 22 && count != 23 && count != 25 && count != 26 && count != 28 && count != 29) return out;
    std::array<jfloat, 22> v{};
    env->GetFloatArrayRegion(values, 0, 22, v.data());
    out.outputScale = v[0];
    out.lkIterations = static_cast<std::uint32_t>(std::max(0L, std::lround(v[1])));
    out.hessianEpsilon = v[2];
    out.kDetail = v[3];
    out.kDenoise = v[4];
    out.dThreshold = v[5];
    out.dTransition = v[6];
    out.kStretch = v[7];
    out.kShrink = v[8];
    out.flatSigma = v[9];
    out.detailFloorSigma = v[10];
    out.scaleBandwidthGain = v[11];
    out.coverageNeffLo = v[12];
    out.coverageNeffHi = v[13];
    out.coverageMassLo = v[14];
    out.coverageMassHi = v[15];
    out.robustnessT = v[16];
    out.robustnessS1 = v[17];
    out.robustnessS2 = v[18];
    out.motionThreshold = v[19];
    out.fallbackChromaGain = v[20];
    out.fallbackLumaGain = v[21];
    const auto inRange = [](float value, float minimum, float maximum) noexcept {
        return std::isfinite(value) && value >= minimum && value <= maximum;
    };
    if (!inRange(out.outputScale, 1.0f, 1.386f) || out.lkIterations < 2u || out.lkIterations > 5u ||
        !inRange(out.hessianEpsilon, 1.0e-12f, 1.0e-7f) || !inRange(out.kDetail, 0.08f, 0.33f) ||
        !inRange(out.kDenoise, 1.0f, 8.0f) || !inRange(out.dThreshold, 0.20f, 1.0f) ||
        !inRange(out.dTransition, 0.30f, 2.0f) || !inRange(out.kStretch, 1.0f, 6.0f) ||
        !inRange(out.kShrink, 1.0f, 10.0f) || !inRange(out.robustnessT, 0.05f, 0.25f) ||
        !inRange(out.robustnessS1, 1.0f, 4.0f) || !inRange(out.robustnessS2, 6.0f, 24.0f) ||
        !inRange(out.motionThreshold, 0.5f, 1.0f))
        return {};
    if (!inRange(out.flatSigma, -1.0f, 2.0f) || !inRange(out.detailFloorSigma, 0.0f, 0.25f) ||
        !inRange(out.scaleBandwidthGain, 0.0f, 2.0f) || !inRange(out.coverageNeffLo, 0.0f, 6.0f) ||
        !inRange(out.coverageNeffHi, 0.0f, 6.0f) || out.coverageNeffHi < out.coverageNeffLo ||
        !inRange(out.coverageMassLo, 0.0f, 0.1f) || !inRange(out.coverageMassHi, 0.0f, 0.1f) ||
        out.coverageMassHi < out.coverageMassLo)
        return {};
    if (!inRange(out.fallbackChromaGain, 0.0f, 8.0f) || !inRange(out.fallbackLumaGain, 0.0f, 8.0f)) return {};
    if (count >= 25) {
        std::array<jfloat, 6> merge{0.0f, 13.0f, 32.0f, -2.0f, 2.0f, 0.0f};
        env->GetFloatArrayRegion(values, 23, std::min<jsize>(count - 23, jsize(merge.size())), merge.data());
        if (!(merge[0] == 0.0f || merge[0] == 1.0f || merge[0] == 2.0f || merge[0] == 3.0f) ||
            !inRange(merge[1], 1.0f, 22.0f) || !(merge[2] == 16.0f || merge[2] == 32.0f) ||
            !inRange(merge[3], -4.0f, -1.0f) || !inRange(merge[4], 0.0f, 16.0f) || !inRange(merge[5], 0.0f, 3.0f))
            return {};
        out.longFrameEv = merge[5];
        out.mergeAlgorithm = static_cast<std::uint32_t>(merge[0]);
        out.hdrplusStrength = merge[1];
        out.hdrplusTileSize = static_cast<std::uint32_t>(merge[2]);
        out.bracketEv = merge[3];
        out.bracketFrames = static_cast<std::uint32_t>(std::lround(merge[4]));
    }
    return out;
}

// Shared shutter-time capture-context builders for single-frame and
// prepared-multiframe entry points. Field-for-field identical to the
// previously duplicated inline blocks; argument evaluation order across
// distinct jstrings carries no dependencies.
rawrcam::encoding::dng::DngCaptureContext makeDngContext(jint fd, jint rotation, int64_t wallClockMillis,
                                                         int16_t utcOffsetMinutes, std::string make, std::string model,
                                                         std::string displayName) {
    rawrcam::encoding::dng::DngCaptureContext out{};
    out.outputFd = rawrcam::encoding::OutputFd(fd);
    out.deviceRotationDegrees = rotation;
    out.wallClockUnixMillis = wallClockMillis;
    out.utcOffsetMinutes = utcOffsetMinutes;
    out.deviceMake = std::move(make);
    // Capture DNG identity stays stable even when JPEG uses a marketing name.
    char rawModel[PROP_VALUE_MAX]{};
    __system_property_get("ro.product.model", rawModel);
    out.deviceModel = rawModel[0] ? std::string(rawModel) : model;
    out.deviceDisplayModel = std::move(model);
    out.displayName = std::move(displayName);
    return out;
}
// Collapsed denoise intent at the JNI boundary (see DenoiseConfig
// toModesArray/toStrengthsArray): modes[master, waveletMaxScale],
// strengths[waveletStrength, waveletDetail, waveletForceY]. One IntArray +
// one FloatArray instead of positional params.
using rawrcam::jni::DenoiseIntent;
using rawrcam::jni::parseDenoiseSpec;
void fillJpegContext(JNIEnv* env, rawrcam::capture::JpegCaptureRequest& jpeg, jint fd, jint rotation,
                     int64_t wallClockMillis, int16_t utcOffsetMinutes, const std::string& make,
                     const std::string& model, jstring displayName, jint quality, jboolean pipelineDiagnosticsEnabled,
                     jint colorRenderProfile, jstring importedLutProfileId, jstring rendererDisplayName,
                     jint demosaicAlgorithm, jboolean dualAutoContrast, jfloat dualContrastPercent, jint fccSteps,
                     jboolean lensShadingCorrectionEnabled, jboolean highlightReconstructionEnabled,
                     jint jpegSubsampling, jboolean distortionCorrectionEnabled, jstring filmDescription,
                     jboolean defringeEnabled, jfloat defringeStrength, jfloat defringeEdgeThreshold,
                     jfloat defringeLumaFloor, const DenoiseIntent& denoise, jboolean ultraHdrEnabled,
                     jint ultraHdrGainmapQuality) {
    jpeg.output.outputFd = rawrcam::encoding::OutputFd(fd);
    jpeg.output.filmDescription = jString(env, filmDescription);
    jpeg.output.deviceRotationDegrees = rotation;
    jpeg.output.wallClockUnixMillis = wallClockMillis;
    jpeg.output.utcOffsetMinutes = utcOffsetMinutes;
    jpeg.output.deviceMake = make;
    jpeg.output.deviceModel = model;
    jpeg.output.displayName = jString(env, displayName);
    jpeg.output.quality = quality;
    jpeg.develop = rawrcam::jni::readCaptureDevelopSettings(
        {pipelineDiagnosticsEnabled, colorRenderProfile, jString(env, importedLutProfileId), dualAutoContrast,
         dualContrastPercent, fccSteps, defringeEnabled, defringeStrength, defringeEdgeThreshold, defringeLumaFloor,
         denoise, lensShadingCorrectionEnabled, highlightReconstructionEnabled, distortionCorrectionEnabled,
         demosaicAlgorithm});
    jpeg.output.rendererDisplayName = jString(env, rendererDisplayName);
    jpeg.output.subsampling = jpegSubsampling == 0
                                  ? rawrcam::encoding::jpeg::ChromaSubsampling::Yuv444
                                  : (jpegSubsampling == 1 ? rawrcam::encoding::jpeg::ChromaSubsampling::Yuv422
                                                          : rawrcam::encoding::jpeg::ChromaSubsampling::Yuv420);
    jpeg.output.ultraHdr.enabled = ultraHdrEnabled == JNI_TRUE;
    jpeg.output.ultraHdr.gainmapQuality = std::clamp(static_cast<int>(ultraHdrGainmapQuality), 1, 100);
}
}  // namespace

extern "C" JNIEXPORT jlong JNICALL Java_com_rawr_camera_integration_NativePreviewEngine_create(
    JNIEnv* env, jobject, jstring filesDir, jstring nativeLibraryDir, jstring customDriverPath) {
    rawrcam::vulkan::dispatch::configure(jString(env, nativeLibraryDir), jString(env, customDriverPath));
    const std::string files = jString(env, filesDir);
    if (!files.empty()) rawrcam::vulkan::dispatch::setPipelineCacheFile(files + "/vk_pipeline_cache.bin");
    return static_cast<jlong>(reinterpret_cast<intptr_t>(rawrcam::session::createEngine(files)));
}
extern "C" JNIEXPORT void JNICALL Java_com_rawr_camera_integration_NativePreviewEngine_destroy(JNIEnv*, jobject,
                                                                                               jlong h) {
    rawrcam::session::destroyEngine(handle(h));
}
extern "C" JNIEXPORT jboolean JNICALL Java_com_rawr_camera_integration_NativePreviewEngine_setSurface(JNIEnv* env,
                                                                                                      jobject, jlong h,
                                                                                                      jobject surface,
                                                                                                      jint rotation) {
    return rawrcam::session::setSurface(handle(h), env, surface, rotation) ? JNI_TRUE : JNI_FALSE;
}
extern "C" JNIEXPORT jboolean JNICALL Java_com_rawr_camera_integration_NativePreviewEngine_setVideoSurface(
    JNIEnv* env, jobject, jlong h, jobject surface, jint width, jint height, jint bitDepth) {
    return rawrcam::session::setVideoSurface(handle(h), env, surface, static_cast<uint32_t>(std::max(width, 0)),
                                             static_cast<uint32_t>(std::max(height, 0)),
                                             static_cast<uint32_t>(std::max(bitDepth, 0)))
               ? JNI_TRUE
               : JNI_FALSE;
}
extern "C" JNIEXPORT void JNICALL Java_com_rawr_camera_integration_NativePreviewEngine_prewarmVideo(JNIEnv*, jobject,
                                                                                                    jlong h, jint width,
                                                                                                    jint height,
                                                                                                    jint bitDepth) {
    rawrcam::session::requestVideoPrewarm(handle(h), static_cast<uint32_t>(std::max(width, 0)),
                                          static_cast<uint32_t>(std::max(height, 0)),
                                          static_cast<uint32_t>(std::max(bitDepth, 0)));
}
extern "C" JNIEXPORT void JNICALL Java_com_rawr_camera_integration_NativePreviewEngine_releaseVideoProcessing(JNIEnv*,
                                                                                                              jobject,
                                                                                                              jlong h) {
    rawrcam::session::releaseVideoProcessing(handle(h));
}
extern "C" JNIEXPORT void JNICALL Java_com_rawr_camera_integration_NativePreviewEngine_setVideoPreviewCrop(
    JNIEnv*, jobject, jlong h, jint width, jint height) {
    rawrcam::session::setVideoPreviewCropOut(handle(h), static_cast<uint32_t>(std::max(width, 0)),
                                             static_cast<uint32_t>(std::max(height, 0)));
}
extern "C" JNIEXPORT jstring JNICALL Java_com_rawr_camera_integration_NativePreviewEngine_videoStats(JNIEnv* env,
                                                                                                     jobject, jlong h) {
    return env->NewStringUTF(rawrcam::session::videoStats(handle(h)).c_str());
}
extern "C" JNIEXPORT void JNICALL Java_com_rawr_camera_integration_NativePreviewEngine_setVideoImageSettings(
    JNIEnv*, jobject, jlong h, jboolean lensShadingEnabled, jboolean highlightEnabled, jint highlightMethod,
    jfloat highlightThreshold, jfloat highlightCompression, jint fccSteps, jfloat defringeStrength,
    jfloat waveletDenoiseStrength, jfloat waveletDenoiseDetail,
    jfloat waveletDenoiseForceY, jint waveletDenoiseScales, jboolean distortionEnabled) {
    const auto finiteClamped = [](float value, float fallback, float lo, float hi) {
        return std::isfinite(value) ? std::clamp(value, lo, hi) : fallback;
    };
    rawrcam::video::VideoProcessingConfig config{};
    config.lensShadingEnabled = lensShadingEnabled == JNI_TRUE;
    config.distortionEnabled = distortionEnabled == JNI_TRUE;
    config.highlightEnabled = highlightEnabled == JNI_TRUE;
    config.highlightMethod = static_cast<uint32_t>(std::clamp(static_cast<int>(highlightMethod), 0, 1));
    config.highlightThreshold = finiteClamped(highlightThreshold, 1.0f, 0.5f, 2.0f);
    config.highlightCompression = finiteClamped(highlightCompression, 100.0f, 0.0f, 500.0f);
    config.fccSteps = static_cast<uint32_t>(std::clamp(static_cast<int>(fccSteps), 0, 1));
    config.defringeStrength = finiteClamped(defringeStrength, 0.0f, 0.0f, 1.0f);
    config.waveletDenoiseStrength = finiteClamped(waveletDenoiseStrength, 0.0f, 0.0f, 8.0f);
    config.waveletDenoiseDetail = finiteClamped(waveletDenoiseDetail, 1.0f, 0.0f, 1.8f);
    config.waveletDenoiseForceY = finiteClamped(waveletDenoiseForceY, 0.25f, 0.0f, 1.0f);
    config.waveletDenoiseScales = std::clamp(static_cast<int>(waveletDenoiseScales), 1, 7);
    rawrcam::session::setVideoImageSettings(handle(h), config);
}
extern "C" JNIEXPORT jboolean JNICALL Java_com_rawr_camera_integration_NativePreviewEngine_setRecordingFps(JNIEnv*,
                                                                                                           jobject,
                                                                                                           jlong h,
                                                                                                           jint fps) {
    return rawrcam::session::setRecordingFps(handle(h), fps) ? JNI_TRUE : JNI_FALSE;
}
extern "C" JNIEXPORT void JNICALL Java_com_rawr_camera_integration_NativePreviewEngine_setScopeDeviceRotationDegrees(
    JNIEnv*, jobject, jlong h, jint rotation) {
    rawrcam::session::setScopeDeviceRotationDegrees(handle(h), rotation);
}
extern "C" JNIEXPORT void JNICALL Java_com_rawr_camera_integration_NativePreviewEngine_setCameraActive(JNIEnv*, jobject,
                                                                                                       jlong h,
                                                                                                       jboolean v) {
    rawrcam::session::setCameraActive(handle(h), v == JNI_TRUE);
}
extern "C" JNIEXPORT void JNICALL
Java_com_rawr_camera_integration_NativePreviewEngine_setPreviewForeground(JNIEnv*, jobject, jlong h, jboolean v) {
    rawrcam::session::setPreviewForeground(handle(h), v == JNI_TRUE);
}
extern "C" JNIEXPORT void JNICALL Java_com_rawr_camera_integration_NativePreviewEngine_setInitialConfigReady(JNIEnv*,
                                                                                                             jobject,
                                                                                                             jlong h) {
    rawrcam::session::setInitialConfigReady(handle(h));
}
extern "C" JNIEXPORT void JNICALL Java_com_rawr_camera_integration_NativePreviewEngine_setStillCaptureInFlight(
    JNIEnv*, jobject, jlong h, jboolean begin) {
    rawrcam::session::setStillCaptureInFlight(handle(h), begin == JNI_TRUE);
}
extern "C" JNIEXPORT void JNICALL Java_com_rawr_camera_integration_NativePreviewEngine_tickCameraSession(JNIEnv*,
                                                                                                         jobject,
                                                                                                         jlong h) {
    rawrcam::session::tickCameraSession(handle(h));
}
extern "C" JNIEXPORT jboolean JNICALL Java_com_rawr_camera_integration_NativePreviewEngine_canRecoverStills(JNIEnv*,
                                                                                                            jobject,
                                                                                                            jlong h) {
    return rawrcam::session::canRecoverStills(handle(h)) ? JNI_TRUE : JNI_FALSE;
}
extern "C" JNIEXPORT void JNICALL Java_com_rawr_camera_integration_NativePreviewEngine_setVideoMode(JNIEnv*, jobject,
                                                                                                    jlong h,
                                                                                                    jboolean video,
                                                                                                    jint fps) {
    rawrcam::session::setVideoMode(handle(h), video == JNI_TRUE, fps);
}
extern "C" JNIEXPORT void JNICALL Java_com_rawr_camera_integration_NativePreviewEngine_setOisEnabled(JNIEnv*, jobject,
                                                                                                     jlong h,
                                                                                                     jboolean v) {
    rawrcam::session::setOisEnabled(handle(h), v == JNI_TRUE);
}
extern "C" JNIEXPORT void JNICALL Java_com_rawr_camera_integration_NativePreviewEngine_setAntibandingMode(JNIEnv*,
                                                                                                          jobject,
                                                                                                          jlong h,
                                                                                                          jint v) {
    rawrcam::session::setAntibandingMode(handle(h), static_cast<uint8_t>(std::clamp(v, 0, 3)));
}
extern "C" JNIEXPORT void JNICALL Java_com_rawr_camera_integration_NativePreviewEngine_setRawrAutoExposure(
    JNIEnv*, jobject, jlong h, jboolean enabled, jint nightLook, jint isoMin, jint isoMax, jlong shutterStartNs,
    jlong shutterMaxNs, jfloat balanceEv, jfloat maxDigitalGainEv, jint speed, jint metering) {
    rawrcam::session::setRawrAutoExposure(handle(h), enabled == JNI_TRUE, nightLook, isoMin, isoMax, shutterStartNs,
                                          shutterMaxNs, balanceEv, maxDigitalGainEv, speed, metering);
}
extern "C" JNIEXPORT void JNICALL Java_com_rawr_camera_integration_NativePreviewEngine_selectLens(JNIEnv* env, jobject,
                                                                                                  jlong h, jstring v) {
    rawrcam::session::selectLens(handle(h), jString(env, v));
}
extern "C" JNIEXPORT jboolean JNICALL
Java_com_rawr_camera_integration_NativePreviewEngine_setCameraProfile(JNIEnv* env, jobject, jlong h, jstring json) {
    return rawrcam::session::setCameraProfile(handle(h), jString(env, json)) ? JNI_TRUE : JNI_FALSE;
}
extern "C" JNIEXPORT jstring JNICALL
Java_com_rawr_camera_integration_NativePreviewEngine_builtInCameraProfile(JNIEnv* env, jobject) {
    const auto& profile = rawrcam::camera::deviceBuiltInCameraProfile();
    return env->NewStringUTF(rawrcam::camera::serializeCameraProfile(profile).c_str());
}
// [error, name, summary] for a lens camera profile (.dcp) file; error is empty
// when the app can use it. DCP names are free-form bytes: ASCII only, so
// NewStringUTF never sees invalid modified UTF-8.
extern "C" JNIEXPORT jobjectArray JNICALL
Java_com_rawr_camera_integration_NativePreviewEngine_inspectCameraProfile(JNIEnv* env, jobject, jstring path) {
    std::string error;
    auto profile = rawrcam::color::loadDcp(jString(env, path), &error);
    if (profile && !profile->embeddable()) error = "This profile doesn't allow embedding in DNG files";
    if (profile && error.empty() && !rawrcam::color::dcpColorCalibration(*profile))
        error = "The profile's matrices are not invertible";
    const auto ascii = [](std::string s) {
        for (auto& c : s)
            if (static_cast<unsigned char>(c) < 0x20 || static_cast<unsigned char>(c) > 0x7e) c = '?';
        return s;
    };
    const std::string fields[3] = {ascii(error), profile ? ascii(profile->name) : std::string(),
                                   profile ? ascii(rawrcam::color::describe(*profile)) : std::string()};
    jobjectArray out = env->NewObjectArray(3, env->FindClass("java/lang/String"), nullptr);
    for (jsize i = 0; i < 3; ++i) {
        jstring value = env->NewStringUTF(fields[i].c_str());
        env->SetObjectArrayElement(out, i, value);
        env->DeleteLocalRef(value);
    }
    return out;
}
extern "C" JNIEXPORT void JNICALL Java_com_rawr_camera_integration_NativePreviewEngine_setPreferredCameraId(JNIEnv* env,
                                                                                                            jobject,
                                                                                                            jlong h,
                                                                                                            jstring v) {
    rawrcam::session::setPreferredCameraId(handle(h), jString(env, v));
}
extern "C" JNIEXPORT void JNICALL
Java_com_rawr_camera_integration_NativePreviewEngine_setPreferredAccessRoute(JNIEnv* env, jobject, jlong h, jstring v) {
    rawrcam::session::setPreferredAccessRoute(handle(h), jString(env, v));
}
extern "C" JNIEXPORT void JNICALL
Java_com_rawr_camera_integration_NativePreviewEngine_setPreferredColorMode(JNIEnv* env, jobject, jlong h, jstring v) {
    rawrcam::session::setPreferredColorMode(handle(h), jString(env, v));
}
extern "C" JNIEXPORT void JNICALL Java_com_rawr_camera_integration_NativePreviewEngine_setPipelineDiagnostic(JNIEnv*,
                                                                                                             jobject,
                                                                                                             jlong h,
                                                                                                             jint v) {
    rawrcam::session::setPipelineDiagnostic(handle(h), static_cast<uint32_t>(std::max(v, 0)));
}
extern "C" JNIEXPORT void JNICALL Java_com_rawr_camera_integration_NativePreviewEngine_setPersistentDiagnosticsEnabled(
    JNIEnv*, jobject, jlong h, jboolean v) {
    rawrcam::session::setPersistentDiagnosticsEnabled(handle(h), v == JNI_TRUE);
}
extern "C" JNIEXPORT void JNICALL
Java_com_rawr_camera_integration_NativePreviewEngine_setPersistZslRingEnabled(JNIEnv*, jobject, jlong h, jboolean v) {
    rawrcam::session::setPersistZslRingEnabled(handle(h), v == JNI_TRUE);
}
extern "C" JNIEXPORT jint JNICALL Java_com_rawr_camera_integration_NativePreviewEngine_prepareExperimentalMultiframe(
    JNIEnv*, jobject, jlong h, jint maxFrames, jint longFrames) {
    return static_cast<jint>(rawrcam::session::prepareExperimentalMultiframe(
        handle(h), static_cast<uint32_t>(std::clamp(maxFrames, 2, 30)), static_cast<uint32_t>(std::clamp(longFrames, 0, 28))));
}
extern "C" JNIEXPORT void JNICALL
Java_com_rawr_camera_integration_NativePreviewEngine_cancelPreparedMultiframeCapture(JNIEnv*, jobject, jlong h) {
    rawrcam::session::cancelPreparedMultiframeCapture(handle(h));
}
extern "C" JNIEXPORT void JNICALL Java_com_rawr_camera_integration_NativePreviewEngine_setInternalTraceCaptureEnabled(
    JNIEnv*, jobject, jlong h, jboolean v) {
    rawrcam::session::setInternalTraceCaptureEnabled(handle(h), v == JNI_TRUE);
}
extern "C" JNIEXPORT void JNICALL Java_com_rawr_camera_integration_NativePreviewEngine_setDistortionCorrectionEnabled(
    JNIEnv*, jobject, jlong h, jboolean v) {
    rawrcam::session::setDistortionCorrectionEnabled(handle(h), v == JNI_TRUE);
}
extern "C" JNIEXPORT void JNICALL Java_com_rawr_camera_integration_NativePreviewEngine_setLensShadingCorrectionEnabled(
    JNIEnv*, jobject, jlong h, jboolean v) {
    rawrcam::session::setLensShadingCorrectionEnabled(handle(h), v == JNI_TRUE);
}
extern "C" JNIEXPORT void JNICALL
Java_com_rawr_camera_integration_NativePreviewEngine_setHighlightReconstructionEnabled(JNIEnv*, jobject, jlong h,
                                                                                       jboolean v) {
    rawrcam::session::setHighlightReconstructionEnabled(handle(h), v == JNI_TRUE);
}
extern "C" JNIEXPORT void JNICALL Java_com_rawr_camera_integration_NativePreviewEngine_setMaxAePostGain(JNIEnv*,
                                                                                                        jobject,
                                                                                                        jlong h,
                                                                                                        jfloat v) {
    rawrcam::session::setMaxAePostGain(handle(h), v);
}
extern "C" JNIEXPORT void JNICALL Java_com_rawr_camera_integration_NativePreviewEngine_setAePriorityDisabled(
    JNIEnv*, jobject, jlong h, jboolean v) {
    rawrcam::session::setAePriorityDisabled(handle(h), v == JNI_TRUE);
}
extern "C" JNIEXPORT void JNICALL Java_com_rawr_camera_integration_NativePreviewEngine_setAutoMinFps(JNIEnv*, jobject,
                                                                                                     jlong h, jint v) {
    rawrcam::session::setAutoMinFps(handle(h), v);
}
extern "C" JNIEXPORT void JNICALL
Java_com_rawr_camera_integration_NativePreviewEngine_setPostGainKneeWidthEv(JNIEnv*, jobject, jlong h, jfloat v) {
    rawrcam::session::setPostGainKneeWidthEv(handle(h), v);
}
extern "C" JNIEXPORT void JNICALL Java_com_rawr_camera_integration_NativePreviewEngine_setAutoKeyMode(JNIEnv*, jobject,
                                                                                                      jlong h, jint v) {
    rawrcam::session::setAutoKeyMode(handle(h), v);
}
extern "C" JNIEXPORT void JNICALL Java_com_rawr_camera_integration_NativePreviewEngine_setLocalTone(
    JNIEnv*, jobject, jlong h, jfloat shadowsEv, jfloat highlightsEv, jfloat sigma, jint coarsestMip, jboolean boostLocalContrast) {
    rawrcam::session::setLocalTone(handle(h), shadowsEv, highlightsEv, sigma, static_cast<unsigned>(coarsestMip), boostLocalContrast == JNI_TRUE);
}
extern "C" JNIEXPORT void JNICALL Java_com_rawr_camera_integration_NativePreviewEngine_setExpectedMergeFrames(
    JNIEnv*, jobject, jlong h, jint frames) {
    rawrcam::session::setExpectedMergeFrames(handle(h), static_cast<uint32_t>(std::clamp(frames, 1, 30)));
}
extern "C" JNIEXPORT void JNICALL
Java_com_rawr_camera_integration_NativePreviewEngine_setExperimentalZeroCopy(JNIEnv*, jobject, jlong h, jboolean v) {
    rawrcam::session::setExperimentalZeroCopy(handle(h), v == JNI_TRUE);
}
extern "C" JNIEXPORT void JNICALL Java_com_rawr_camera_integration_NativePreviewEngine_setExperimentalMultiframeEnabled(
    JNIEnv*, jobject, jlong h, jboolean v) {
    rawrcam::session::setExperimentalMultiframeEnabled(handle(h), v == JNI_TRUE);
}
extern "C" JNIEXPORT void JNICALL Java_com_rawr_camera_integration_NativePreviewEngine_setHdrPlusHeadroomMaxEv(
    JNIEnv*, jobject, jlong h, jfloat v) {
    rawrcam::session::setHdrPlusHeadroomMaxEv(handle(h), v);
}
extern "C" JNIEXPORT void JNICALL
Java_com_rawr_camera_integration_NativePreviewEngine_setPersistentEngineEnabled(JNIEnv*, jobject, jlong h, jboolean v) {
    rawrcam::session::setPersistentEngineEnabled(handle(h), v == JNI_TRUE);
}
extern "C" JNIEXPORT void JNICALL
Java_com_rawr_camera_integration_NativePreviewEngine_setCpuRawCopyProbeFrames(JNIEnv*, jobject, jlong h, jint v) {
    rawrcam::session::setCpuRawCopyProbeFrames(handle(h), static_cast<uint32_t>(std::max(v, 0)));
}

extern "C" JNIEXPORT void JNICALL Java_com_rawr_camera_integration_NativePreviewEngine_setMonitoringOverlay(
    JNIEnv*, jobject, jlong h, jint mode, jfloat sensitivity) {
    rawrcam::session::setMonitoringOverlay(handle(h), static_cast<uint32_t>(std::max(mode, 0)), sensitivity);
}
extern "C" JNIEXPORT void JNICALL Java_com_rawr_camera_integration_NativePreviewEngine_setScopePresentationState(
    JNIEnv* env, jobject, jlong h, jintArray types, jintArray modes, jintArray rotations, jfloatArray rects) {
    rawrcam::monitoring::ScopePresentationState state{};
    if (types && modes && rotations && rects && env->GetArrayLength(types) >= 3 && env->GetArrayLength(modes) >= 3 &&
        env->GetArrayLength(rotations) >= 3 && env->GetArrayLength(rects) >= 15) {
        jint tv[3]{};
        jint mv[3]{};
        jint qv[3]{};
        jfloat rv[15]{};
        env->GetIntArrayRegion(types, 0, 3, tv);
        env->GetIntArrayRegion(modes, 0, 3, mv);
        env->GetIntArrayRegion(rotations, 0, 3, qv);
        env->GetFloatArrayRegion(rects, 0, 15, rv);
        for (uint32_t i = 0; i < 3; ++i) {
            const int t = std::clamp(static_cast<int>(tv[i]), 0, 2);
            state.placements[i].type = static_cast<rawrcam::monitoring::ScopeType>(t);
            const auto finiteUnit = [](float value) noexcept {
                return std::isfinite(value) ? std::clamp(value, 0.0f, 1.0f) : 0.0f;
            };
            state.placements[i].x = finiteUnit(rv[i * 5 + 0]);
            state.placements[i].y = finiteUnit(rv[i * 5 + 1]);
            state.placements[i].width = finiteUnit(rv[i * 5 + 2]);
            state.placements[i].height = finiteUnit(rv[i * 5 + 3]);
            state.placements[i].cornerRadius = std::clamp(finiteUnit(rv[i * 5 + 4]), 0.0f, 0.5f);
            state.placements[i].variant = static_cast<uint32_t>(std::clamp(static_cast<int>(mv[i]), 0, 1));
            state.placements[i].presentationQuarterTurns =
                static_cast<uint32_t>(((static_cast<int>(qv[i]) % 4) + 4) % 4);
        }
    }
    rawrcam::session::setScopePresentationState(handle(h), state);
}
static std::optional<spektrafilm_native::FilmLook> decodeFilmLook(JNIEnv* env, jfloatArray values, jintArray enums) {
    constexpr jsize kFloatCount = 98;
    constexpr jsize kIntCount = 27;
    if (!values || !enums || env->GetArrayLength(values) != kFloatCount || env->GetArrayLength(enums) != kIntCount) {
        return std::nullopt;
    }
    jfloat f[kFloatCount];
    jint e[kIntCount];
    env->GetFloatArrayRegion(values, 0, kFloatCount, f);
    env->GetIntArrayRegion(enums, 0, kIntCount, e);
    for (jsize i = 0; i < kFloatCount; ++i) {
        if (!std::isfinite(f[i])) return std::nullopt;
    }
    spektrafilm_native::FilmLook look{};
    look.filmExposureEv = f[0];
    look.printExposureEv = f[1];
    look.filmPushPullStops = f[2];
    look.filmGamma = f[3];
    look.printPushPullStops = f[4];
    look.printGamma = f[5];
    look.printShadowShape = f[6];
    look.printHighlightShape = f[7];
    look.negativeBleachBypassAmount = f[8];
    look.negativeLeucoCyanCoupling = f[9];
    look.printBleachBypassAmount = f[10];
    look.preflashExposure = f[11];
    look.preflashMFilterShift = f[12];
    look.preflashYFilterShift = f[13];
    look.filterC = f[14];
    look.filterMShift = f[15];
    look.filterYShift = f[16];
    look.printerLightsR = f[17];
    look.printerLightsG = f[18];
    look.printerLightsB = f[19];
    look.enlargerScale = f[20];
    look.enlargerOffsetXPercent = f[21];
    look.enlargerOffsetYPercent = f[22];
    look.grainAmount = f[23];
    look.grainSaturation = f[24];
    look.grainParticleAreaUm2 = f[25];
    look.grainParticleScaleR = f[26];
    look.grainParticleScaleG = f[27];
    look.grainParticleScaleB = f[28];
    look.grainParticleScaleLayer0 = f[29];
    look.grainParticleScaleLayer1 = f[30];
    look.grainParticleScaleLayer2 = f[31];
    look.grainDensityMinR = f[32];
    look.grainDensityMinG = f[33];
    look.grainDensityMinB = f[34];
    look.grainUniformityR = f[35];
    look.grainUniformityG = f[36];
    look.grainUniformityB = f[37];
    look.grainFinalBlurUm = f[38];
    look.grainBlurDyeCloudsUm = f[39];
    look.grainMicroStructureScale = f[40];
    look.grainMicroStructureSigmaNm = f[41];
    look.cameraUvCutNm = f[42];
    look.cameraIrCutNm = f[43];
    look.dirCouplersAmount = std::clamp(f[44], 0.0f, 1.0f);
    look.dirCouplersDiffusionUm = f[45];
    look.dirCouplersDiffusionTailUm = f[46];
    look.dirCouplersDiffusionTailWeight = f[47];
    look.dirCouplersInhibitionSameLayer = f[48];
    look.dirCouplersInhibitionInterlayer = f[49];
    look.dirCouplersGammaSameLayerR = f[50];
    look.dirCouplersGammaSameLayerG = f[51];
    look.dirCouplersGammaSameLayerB = f[52];
    look.dirCouplersGammaRToG = f[53];
    look.dirCouplersGammaRToB = f[54];
    look.dirCouplersGammaGToR = f[55];
    look.dirCouplersGammaGToB = f[56];
    look.dirCouplersGammaBToR = f[57];
    look.dirCouplersGammaBToG = f[58];
    look.scannerWhiteLevel = f[59];
    look.scannerBlackLevel = f[60];
    look.glarePercent = f[61];
    look.glareRoughness = f[62];
    look.glareBlur = f[63];
    look.scannerMtf50LpMm = f[64];
    look.scannerUnsharpRadiusUm = f[65];
    look.scannerUnsharpAmount = f[66];
    look.scatterAmount = f[67];
    look.scatterScale = f[68];
    look.halationAmount = f[69];
    look.halationScale = f[70];
    look.halationStrengthR = f[71];
    look.halationStrengthG = f[72];
    look.halationStrengthB = f[73];
    look.halationFirstSigmaUmR = f[74];
    look.halationFirstSigmaUmG = f[75];
    look.halationFirstSigmaUmB = f[76];
    look.halationBoostEv = f[77];
    look.halationBoostRange = f[78];
    look.halationProtectEv = f[79];
    look.cameraDiffusionStrength = f[80];
    look.cameraDiffusionSpatialScale = f[81];
    look.cameraDiffusionHaloWarmth = f[82];
    look.cameraDiffusionCoreIntensity = f[83];
    look.cameraDiffusionCoreSize = f[84];
    look.cameraDiffusionHaloIntensity = f[85];
    look.cameraDiffusionHaloSize = f[86];
    look.cameraDiffusionBloomIntensity = f[87];
    look.cameraDiffusionBloomSize = f[88];
    look.printDiffusionStrength = f[89];
    look.printDiffusionSpatialScale = f[90];
    look.printDiffusionHaloWarmth = f[91];
    look.printDiffusionCoreIntensity = f[92];
    look.printDiffusionCoreSize = f[93];
    look.printDiffusionHaloIntensity = f[94];
    look.printDiffusionHaloSize = f[95];
    look.printDiffusionBloomIntensity = f[96];
    look.printDiffusionBloomSize = f[97];
    look.film = e[0];
    look.paper = e[1];
    look.inputColorSpace = e[2];
    look.outputColorSpace = e[3];
    look.rgbToRawMethod = e[4];
    look.filmPushPullMode = e[5];
    look.printerLightsGang = e[6] != 0;
    look.printerLightCalibration = e[7] != 0;
    look.grainEnabled = e[8] != 0;
    look.grainModel = e[9];
    look.filmFormat = e[10];
    look.grainSublayersEnabled = e[11] != 0;
    look.grainSubLayerCount = e[12];
    look.grainSeed = static_cast<uint32_t>(e[13]);
    look.grainAnimate = e[14] != 0;
    look.cameraUvFilterEnabled = e[15] != 0;
    look.cameraIrFilterEnabled = e[16] != 0;
    look.process = e[17];
    look.scanNegativeInvert = e[18] != 0;
    look.scannerEnabled = e[19] != 0;
    look.scannerWhiteCorrection = e[20] != 0;
    look.scannerBlackCorrection = e[21] != 0;
    look.halationEnabled = e[22] != 0;
    look.cameraDiffusionEnabled = e[23] != 0;
    look.cameraDiffusionFamily = e[24];
    look.printDiffusionEnabled = e[25] != 0;
    look.printDiffusionFamily = e[26];
    return look;
}
static void freezeCaptureIntent(JNIEnv* env, rawrcam::encoding::dng::DngCaptureContext& context, jfloatArray toneValues,
                                jfloatArray filmValues, jintArray filmEnums, bool filmEnabled) {
    if (auto film = decodeFilmLook(env, filmValues, filmEnums))
        context.captureFilm = std::make_shared<spektrafilm_native::FilmLook>(*film);
    context.captureFilmEnabled = filmEnabled;
    if (toneValues && env->GetArrayLength(toneValues) >= 9) {
        jfloat values[9];
        env->GetFloatArrayRegion(toneValues, 0, 9, values);
        auto tone = tonemap::TonemapPresets::NeutralBaseline().params;
        tone.exposureEV = values[0];
        tone.blackPointEV = values[1];
        tone.shadowLiftEV = values[2];
        tone.midtoneLiftEV = values[3];
        tone.contrast = values[4];
        tone.whitePointEV = values[5];
        tone.highlightBiasEV = values[6];
        tone.saturation = values[7];
        tone.vibrance = values[8];
        // Optional AgX tail after the three highlight values: the
        // tonemap_integration::kAgxControlCount controls.
        constexpr jsize kAgxOffset = 12;
        constexpr auto kAgxCount = static_cast<jsize>(rawrcam::tonemap_integration::kAgxControlCount);
        if (env->GetArrayLength(toneValues) >= kAgxOffset + kAgxCount) {
            jfloat agx[kAgxCount];
            env->GetFloatArrayRegion(toneValues, kAgxOffset, kAgxCount, agx);
            (void)rawrcam::tonemap_integration::agxFromControls(agx, &tone.agx);
        }
        context.captureTone = std::make_shared<tonemap::TonemapParams>(tone);
    }
}
static void fillHighlightIntent(JNIEnv* env, rawrcam::capture::JpegCaptureRequest& jpeg, jfloatArray toneValues) {
    // The first nine values are the stable tone contract; three optional values
    // carry the still highlight method and its RawTherapee controls.
    if (!toneValues || env->GetArrayLength(toneValues) < 12) return;
    jfloat values[3];
    env->GetFloatArrayRegion(toneValues, 9, 3, values);
    jpeg.develop.highlightReconstructionMethod = std::isfinite(values[0]) && values[0] >= 0.5f ? 1u : 0u;
    jpeg.develop.highlightThreshold = std::isfinite(values[1]) ? std::clamp(values[1], 0.5f, 2.0f) : 1.0f;
    jpeg.develop.highlightCompression = std::isfinite(values[2]) ? std::clamp(values[2], 0.0f, 300.0f) : 45.0f;
}
// Capture sharpening travels in the capture recipe (render intent, also
// frozen into the DNG so the Renderer starts from the captured level).
static void fillCaptureSharpening(JNIEnv* env, rawrcam::capture::JpegCaptureRequest& jpeg, jstring captureRecipe) {
    if (!captureRecipe) return;
    rawrcam::jni::Json recipe(env, captureRecipe);
    if (env->ExceptionCheck()) {  // malformed recipe: keep the stage off
        env->ExceptionClear();
        return;
    }
    jpeg.develop.captureSharpenIterations = rawrcam::jni::captureSharpenIterations(recipe);
}
static std::optional<tinydng_gps> captureGps(JNIEnv* env, jobject value) {
    const auto gps = rawrcam::jni::readCaptureGps(env, value);
    if (!gps) return std::nullopt;
    return rawrcam::metadata::makeGps(gps->latitude, gps->longitude, gps->altitude, gps->accuracy,
                                      gps->fixTimeMillis, gps->providerCode);
}

extern "C" JNIEXPORT jlong JNICALL Java_com_rawr_camera_integration_NativePreviewEngine_requestRawStillCapture(
    JNIEnv* env, jobject, jlong h, jint dngFd, jint jpegFd, jint jpegQuality, jint jpegSubsampling,
    jboolean pipelineDiagnosticsEnabled, jint colorRenderProfile, jstring importedLutProfileId,
    jstring rendererDisplayName, jint demosaicAlgorithm, jboolean dualAutoContrast, jfloat dualContrastPercent,
    jint fccSteps, jboolean lensShadingCorrectionEnabled, jboolean highlightReconstructionEnabled, jint rotation,
    jlong wallClockMillis, jint utcOffsetMinutes, jstring make, jstring model, jstring dngDisplayName,
    jstring jpegDisplayName, jboolean distortionCorrectionEnabled, jstring filmDescription, jboolean defringeEnabled,
    jfloat defringeStrength, jfloat defringeEdgeThreshold, jfloat defringeLumaFloor, jintArray denoiseModes,
    jfloatArray denoiseStrengths, jstring captureRecipe, jfloatArray captureTone, jfloatArray captureFilmValues,
    jintArray captureFilmEnums, jboolean captureFilmEnabled, jboolean ultraHdrEnabled, jint ultraHdrGainmapQuality,
    jint dngCompression, jobject gps) {
    const std::string deviceMake = jString(env, make);
    const std::string deviceModel = jString(env, model);
    const DenoiseIntent denoise = parseDenoiseSpec(env, denoiseModes, denoiseStrengths);
    rawrcam::encoding::dng::DngCaptureContext dng =
        makeDngContext(dngFd, rotation, static_cast<int64_t>(wallClockMillis), static_cast<int16_t>(utcOffsetMinutes),
                       deviceMake, deviceModel, jString(env, dngDisplayName));
    dng.compression = dngCompression == 1 ? rawrcam::encoding::dng::DngCompression::Uncompressed
                                          : rawrcam::encoding::dng::DngCompression::LosslessJpeg;
    dng.processingRecipe = jString(env, captureRecipe);
    dng.gps = captureGps(env, gps);
    dng.appLensShadingCorrection = lensShadingCorrectionEnabled == JNI_TRUE;
    dng.appDistortionCorrection = distortionCorrectionEnabled == JNI_TRUE;
    freezeCaptureIntent(env, dng, captureTone, captureFilmValues, captureFilmEnums, captureFilmEnabled == JNI_TRUE);
    rawrcam::capture::JpegCaptureRequest jpeg{};
    fillJpegContext(env, jpeg, jpegFd, rotation, static_cast<int64_t>(wallClockMillis),
                    static_cast<int16_t>(utcOffsetMinutes), dng.deviceMake, deviceModel, jpegDisplayName,
                    jpegQuality, pipelineDiagnosticsEnabled, colorRenderProfile, importedLutProfileId,
                    rendererDisplayName, demosaicAlgorithm, dualAutoContrast, dualContrastPercent, fccSteps,
                    lensShadingCorrectionEnabled, highlightReconstructionEnabled, jpegSubsampling,
                    distortionCorrectionEnabled, filmDescription, defringeEnabled, defringeStrength,
                    defringeEdgeThreshold, defringeLumaFloor, denoise, ultraHdrEnabled, ultraHdrGainmapQuality);
    fillHighlightIntent(env, jpeg, captureTone);
    fillCaptureSharpening(env, jpeg, captureRecipe);
    jpeg.output.gps = dng.gps;
    return static_cast<jlong>(rawrcam::session::requestRawStillCapture(handle(h), std::move(dng), std::move(jpeg)));
}
extern "C" JNIEXPORT jlong JNICALL Java_com_rawr_camera_integration_NativePreviewEngine_startPreparedMultiframeCapture(
    JNIEnv* env, jobject, jlong h, jint baseDngFd, jint mergedDngFd, jint jpegFd, jint jpegQuality,
    jint jpegSubsampling, jboolean pipelineDiagnosticsEnabled, jint colorRenderProfile, jstring importedLutProfileId,
    jstring rendererDisplayName, jint demosaicAlgorithm, jboolean dualAutoContrast, jfloat dualContrastPercent,
    jint fccSteps, jboolean lensShadingCorrectionEnabled, jboolean highlightReconstructionEnabled, jint rotation,
    jlong wallClockMillis, jint utcOffsetMinutes, jstring make, jstring model, jstring baseDngDisplayName,
    jstring mergedDngDisplayName, jstring jpegDisplayName, jboolean dumpRzslRequested, jfloatArray tuningValues,
    jint baseFrameMode, jboolean distortionCorrectionEnabled, jstring filmDescription, jboolean defringeEnabled,
    jfloat defringeStrength, jfloat defringeEdgeThreshold, jfloat defringeLumaFloor, jintArray denoiseModes,
    jfloatArray denoiseStrengths, jstring captureRecipe, jfloatArray captureTone, jfloatArray captureFilmValues,
    jintArray captureFilmEnums, jboolean captureFilmEnabled, jboolean ultraHdrEnabled, jint ultraHdrGainmapQuality,
    jint dngCompression, jobject gps) {
    const std::string deviceMake = jString(env, make);
    const std::string deviceModel = jString(env, model);
    const DenoiseIntent denoise = parseDenoiseSpec(env, denoiseModes, denoiseStrengths);
    rawrcam::encoding::dng::DngCaptureContext base = makeDngContext(
        baseDngFd, rotation, static_cast<int64_t>(wallClockMillis), static_cast<int16_t>(utcOffsetMinutes), deviceMake,
        deviceModel, jString(env, baseDngDisplayName));
    base.compression = dngCompression == 1 ? rawrcam::encoding::dng::DngCompression::Uncompressed
                                           : rawrcam::encoding::dng::DngCompression::LosslessJpeg;
    base.sourceRole = "base";
    base.processingRecipe = jString(env, captureRecipe);
    base.gps = captureGps(env, gps);
    base.appLensShadingCorrection = lensShadingCorrectionEnabled == JNI_TRUE;
    base.appDistortionCorrection = distortionCorrectionEnabled == JNI_TRUE;
    freezeCaptureIntent(env, base, captureTone, captureFilmValues, captureFilmEnums, captureFilmEnabled == JNI_TRUE);
    rawrcam::encoding::dng::DngCaptureContext merged = base;
    merged.outputFd = rawrcam::encoding::OutputFd(mergedDngFd);
    merged.sourceRole = "merged";
    merged.displayName = jString(env, mergedDngDisplayName);
    rawrcam::capture::JpegCaptureRequest jpeg{};
    fillJpegContext(env, jpeg, jpegFd, rotation, static_cast<int64_t>(wallClockMillis),
                    static_cast<int16_t>(utcOffsetMinutes), deviceMake, deviceModel, jpegDisplayName, jpegQuality,
                    pipelineDiagnosticsEnabled, colorRenderProfile, importedLutProfileId, rendererDisplayName,
                    demosaicAlgorithm, dualAutoContrast, dualContrastPercent, fccSteps, lensShadingCorrectionEnabled,
                    highlightReconstructionEnabled, jpegSubsampling, distortionCorrectionEnabled, filmDescription,
                    defringeEnabled, defringeStrength, defringeEdgeThreshold, defringeLumaFloor, denoise,
                    ultraHdrEnabled, ultraHdrGainmapQuality);
    fillHighlightIntent(env, jpeg, captureTone);
    fillCaptureSharpening(env, jpeg, captureRecipe);
    jpeg.develop.multiframeChromaDenoise = multiframeChromaDenoise(env, tuningValues);
    jpeg.output.gps = base.gps;
    auto baseMode = rawrcam::capture::multiframe::MultiframeBaseFrameMode::Sharpest;
    if (baseFrameMode == 0) baseMode = rawrcam::capture::multiframe::MultiframeBaseFrameMode::Middle;
    return static_cast<jlong>(rawrcam::session::startPreparedMultiframeCapture(
        handle(h), std::move(base), std::move(merged), std::move(jpeg), dumpRzslRequested == JNI_TRUE,
        multiframeTuning(env, tuningValues), baseMode));
}
extern "C" JNIEXPORT jstring JNICALL
Java_com_rawr_camera_integration_NativePreviewEngine_pollDngWriteCompletion(JNIEnv* env, jobject, jlong h) {
    auto s = rawrcam::session::pollDngWriteCompletion(handle(h));
    return env->NewStringUTF(s.c_str());
}
extern "C" JNIEXPORT jstring JNICALL
Java_com_rawr_camera_integration_NativePreviewEngine_pollJpegWriteCompletion(JNIEnv* env, jobject, jlong h) {
    auto s = rawrcam::session::pollJpegWriteCompletion(handle(h));
    return env->NewStringUTF(s.c_str());
}
extern "C" JNIEXPORT jstring JNICALL Java_com_rawr_camera_integration_NativePreviewEngine_runHighlightReplay(
    JNIEnv* env, jobject, jlong h, jstring inputPath) {
    auto r = rawrcam::session::runHighlightReplay(handle(h), jString(env, inputPath));
    return env->NewStringUTF(r.c_str());
}
extern "C" JNIEXPORT jstring JNICALL
Java_com_rawr_camera_integration_NativePreviewEngine_cameraControlSnapshot(JNIEnv* env, jobject, jlong h) {
    auto s = rawrcam::session::cameraControlSnapshot(handle(h));
    return env->NewStringUTF(s.c_str());
}
extern "C" JNIEXPORT jstring JNICALL
Java_com_rawr_camera_integration_NativePreviewEngine_cameraVideoCapabilitiesSnapshot(JNIEnv* env, jobject) {
    const auto snapshot = rawrcam::camera::cameraVideoCapabilitiesSnapshot();
    return env->NewStringUTF(snapshot.c_str());
}
extern "C" JNIEXPORT jstring JNICALL Java_com_rawr_camera_integration_NativePreviewEngine_probeCameras(JNIEnv* env,
                                                                                                     jobject) {
    return env->NewStringUTF(rawrcam::camera::probeCamerasJson().c_str());
}
extern "C" JNIEXPORT jstring JNICALL Java_com_rawr_camera_integration_NativePreviewEngine_probeCameraKeys(
    JNIEnv* env, jobject, jstring cameraId, jobjectArray names) {
    std::vector<std::string> keyNames;
    const jsize count = names ? env->GetArrayLength(names) : 0;
    for (jsize i = 0; i < count; ++i) {
        auto* name = static_cast<jstring>(env->GetObjectArrayElement(names, i));
        keyNames.push_back(jString(env, name));
        env->DeleteLocalRef(name);
    }
    return env->NewStringUTF(rawrcam::camera::probeCameraKeysJson(jString(env, cameraId), keyNames).c_str());
}
extern "C" JNIEXPORT jint JNICALL
Java_com_rawr_camera_integration_NativePreviewEngine_videoRotationDegrees(JNIEnv*, jobject, jlong h, jint degrees) {
    return rawrcam::session::videoRotationDegrees(handle(h), degrees);
}
extern "C" JNIEXPORT jstring JNICALL
Java_com_rawr_camera_integration_NativePreviewEngine_faceDetectionsSnapshot(JNIEnv* env, jobject, jlong h) {
    auto s = rawrcam::session::faceDetectionsSnapshot(handle(h));
    return env->NewStringUTF(s.c_str());
}
extern "C" JNIEXPORT void JNICALL Java_com_rawr_camera_integration_NativePreviewEngine_setExposureMode(JNIEnv*, jobject,
                                                                                                       jlong h,
                                                                                                       jint v) {
    rawrcam::session::setExposureMode(handle(h), v);
}
extern "C" JNIEXPORT void JNICALL
Java_com_rawr_camera_integration_NativePreviewEngine_setManualExposureTimeNs(JNIEnv*, jobject, jlong h, jlong v) {
    rawrcam::session::setManualExposureTimeNs(handle(h), static_cast<int64_t>(v));
}
extern "C" JNIEXPORT void JNICALL Java_com_rawr_camera_integration_NativePreviewEngine_setShutterAngleDegrees(
    JNIEnv*, jobject, jlong h, jdouble degrees) {
    rawrcam::session::setShutterAngleDegrees(handle(h), degrees);
}
extern "C" JNIEXPORT void JNICALL Java_com_rawr_camera_integration_NativePreviewEngine_setManualSensitivity(JNIEnv*,
                                                                                                            jobject,
                                                                                                            jlong h,
                                                                                                            jint v) {
    rawrcam::session::setManualSensitivity(handle(h), static_cast<int32_t>(v));
}
extern "C" JNIEXPORT void JNICALL
Java_com_rawr_camera_integration_NativePreviewEngine_setExposureCompensationSteps(JNIEnv*, jobject, jlong h, jint v) {
    rawrcam::session::setExposureCompensationSteps(handle(h), static_cast<int32_t>(v));
}
extern "C" JNIEXPORT void JNICALL Java_com_rawr_camera_integration_NativePreviewEngine_setWhiteBalanceMode(
    JNIEnv*, jobject, jlong h, jint v, jlong requestId) {
    rawrcam::session::setWhiteBalanceMode(handle(h), v, requestId);
}
extern "C" JNIEXPORT void JNICALL Java_com_rawr_camera_integration_NativePreviewEngine_setWhiteBalanceTempTint(
    JNIEnv*, jobject, jlong h, jint temperatureK, jint tint, jint editedAxes, jlong requestId) {
    rawrcam::session::setWhiteBalanceTempTint(handle(h), temperatureK, tint, editedAxes, requestId);
}
extern "C" JNIEXPORT void JNICALL Java_com_rawr_camera_integration_NativePreviewEngine_setWhiteBalanceLocked(
    JNIEnv*, jobject, jlong h, jint temperatureK, jint tint, jlong requestId) {
    rawrcam::session::setWhiteBalanceLocked(handle(h), temperatureK, tint, requestId);
}
extern "C" JNIEXPORT void JNICALL Java_com_rawr_camera_integration_NativePreviewEngine_setSpotAeTarget(
    JNIEnv*, jobject, jlong h, jboolean active, jfloat x, jfloat y) {
    rawrcam::session::setSpotAeTarget(handle(h), active == JNI_TRUE, static_cast<float>(x), static_cast<float>(y));
}
extern "C" JNIEXPORT void JNICALL Java_com_rawr_camera_integration_NativePreviewEngine_setFocusMode(JNIEnv*, jobject,
                                                                                                    jlong h, jint v,
                                                                                                    jfloat x, jfloat y,
                                                                                                    jlong requestId) {
    rawrcam::session::setFocusMode(handle(h), v, x, y, static_cast<uint64_t>(requestId));
}
extern "C" JNIEXPORT void JNICALL Java_com_rawr_camera_integration_NativePreviewEngine_focusAt(JNIEnv*, jobject,
                                                                                               jlong h, jfloat x,
                                                                                               jfloat y,
                                                                                               jlong requestId) {
    rawrcam::session::focusAt(handle(h), x, y, static_cast<uint64_t>(requestId));
}
extern "C" JNIEXPORT void JNICALL Java_com_rawr_camera_integration_NativePreviewEngine_clearTapAf(JNIEnv*, jobject,
                                                                                                  jlong h,
                                                                                                  jlong requestId) {
    rawrcam::session::clearTapAf(handle(h), static_cast<uint64_t>(requestId));
}
extern "C" JNIEXPORT void JNICALL Java_com_rawr_camera_integration_NativePreviewEngine_setManualFocusNormalized(
    JNIEnv*, jobject, jlong h, jfloat v, jlong requestId) {
    rawrcam::session::setManualFocusNormalized(handle(h), v, static_cast<uint64_t>(requestId));
}
extern "C" JNIEXPORT void JNICALL Java_com_rawr_camera_integration_NativePreviewEngine_setQuickToneNativeValue(
    JNIEnv*, jobject, jlong h, jint target, jfloat v) {
    rawrcam::session::setQuickToneNativeValue(handle(h), target, v);
}
extern "C" JNIEXPORT void JNICALL Java_com_rawr_camera_integration_NativePreviewEngine_setColorRenderProfile(
    JNIEnv* env, jobject, jlong h, jint v, jstring profileId) {
    rawrcam::session::setColorRenderProfile(handle(h), static_cast<uint32_t>(std::max(v, 0)), jString(env, profileId));
}
extern "C" JNIEXPORT void JNICALL Java_com_rawr_camera_integration_NativePreviewEngine_setTonemapParameters(
    JNIEnv*, jobject, jlong h, jfloat a, jfloat b, jfloat c, jfloat d, jfloat e, jfloat f, jfloat g, jfloat i,
    jfloat j) {
    rawrcam::session::setTonemapParameters(handle(h), a, b, c, d, e, f, g, i, j);
}
extern "C" JNIEXPORT void JNICALL Java_com_rawr_camera_integration_NativePreviewEngine_setAgxParameters(
    JNIEnv* env, jobject, jlong h, jfloatArray controls) {
    constexpr auto kCount = static_cast<jsize>(rawrcam::tonemap_integration::kAgxControlCount);
    if (!controls || env->GetArrayLength(controls) != kCount) return;
    float values[kCount];
    env->GetFloatArrayRegion(controls, 0, kCount, values);
    rawrcam::session::setAgxParameters(handle(h), values);
}
// Film simulation (neutral naming; Experimental placement is UI-only).
// Float array contract (must be FILM_LOOK_FLOAT_COUNT):
// 0 filmExposureEv, 1 printExposureEv, 2 filmPushPullStops, 3 filmGamma,
// 4 printPushPullStops, 5 printGamma, 6 printShadowShape, 7 printHighlightShape,
// 8 negativeBleachBypassAmount, 9 negativeLeucoCyanCoupling, 10 printBleachBypassAmount,
// 11 preflashExposure, 12 preflashMFilterShift, 13 preflashYFilterShift,
// 14 filterC, 15 filterMShift, 16 filterYShift,
// 17 printerLightsR, 18 printerLightsG, 19 printerLightsB,
// 20 enlargerScale, 21 enlargerOffsetXPercent, 22 enlargerOffsetYPercent,
// 23 grainAmount, 24 grainSaturation, 25 grainParticleAreaUm2,
// 26 grainParticleScaleR, 27 grainParticleScaleG, 28 grainParticleScaleB,
// 29 grainParticleScaleLayer0, 30 grainParticleScaleLayer1, 31 grainParticleScaleLayer2,
// 32 grainDensityMinR, 33 grainDensityMinG, 34 grainDensityMinB,
// 35 grainUniformityR, 36 grainUniformityG, 37 grainUniformityB,
// 38 grainFinalBlurUm, 39 grainBlurDyeCloudsUm,
// 40 grainMicroStructureScale, 41 grainMicroStructureSigmaNm,
// 42 cameraUvCutNm, 43 cameraIrCutNm,
// 44 dirCouplersAmount, 45 dirCouplersDiffusionUm, 46 dirCouplersDiffusionTailUm,
// 47 dirCouplersDiffusionTailWeight, 48 dirCouplersInhibitionSameLayer,
// 49 dirCouplersInhibitionInterlayer, 50 dirCouplersGammaSameLayerR,
// 51 dirCouplersGammaSameLayerG, 52 dirCouplersGammaSameLayerB,
// 53 dirCouplersGammaRToG, 54 dirCouplersGammaRToB, 55 dirCouplersGammaGToR,
// 56 dirCouplersGammaGToB, 57 dirCouplersGammaBToR, 58 dirCouplersGammaBToG,
// 59 scannerWhiteLevel, 60 scannerBlackLevel, 61 glarePercent,
// 62 glareRoughness, 63 glareBlur, 64 scannerMtf50LpMm,
// 65 scannerUnsharpRadiusUm, 66 scannerUnsharpAmount,
// 67 scatterAmount, 68 scatterScale, 69 halationAmount, 70 halationScale,
// 71 halationStrengthR, 72 halationStrengthG, 73 halationStrengthB,
// 74 halationFirstSigmaUmR, 75 halationFirstSigmaUmG, 76 halationFirstSigmaUmB,
// 77 halationBoostEv, 78 halationBoostRange, 79 halationProtectEv,
// 80 cameraDiffusionStrength, 81 cameraDiffusionSpatialScale,
// 82 cameraDiffusionHaloWarmth, 83 cameraDiffusionCoreIntensity,
// 84 cameraDiffusionCoreSize, 85 cameraDiffusionHaloIntensity,
// 86 cameraDiffusionHaloSize, 87 cameraDiffusionBloomIntensity,
// 88 cameraDiffusionBloomSize,
// 89 printDiffusionStrength, 90 printDiffusionSpatialScale,
// 91 printDiffusionHaloWarmth, 92 printDiffusionCoreIntensity,
// 93 printDiffusionCoreSize, 94 printDiffusionHaloIntensity,
// 95 printDiffusionHaloSize, 96 printDiffusionBloomIntensity,
// 97 printDiffusionBloomSize.
// Int array contract (must be FILM_LOOK_INT_COUNT):
// 0 film, 1 paper, 2 inputColorSpace, 3 outputColorSpace, 4 rgbToRawMethod,
// 5 filmPushPullMode, 6 printerLightsGang, 7 printerLightCalibration,
// 8 grainEnabled, 9 grainModel, 10 filmFormat, 11 grainSublayersEnabled,
// 12 grainSubLayerCount, 13 grainSeed, 14 grainAnimate,
// 15 cameraUvFilterEnabled, 16 cameraIrFilterEnabled,
// 17 process (0=print, 1=scan), 18 scanNegativeInvert,
// 19 scannerEnabled, 20 scannerWhiteCorrection, 21 scannerBlackCorrection,
// 22 halationEnabled, 23 cameraDiffusionEnabled, 24 cameraDiffusionFamily,
// 25 printDiffusionEnabled, 26 printDiffusionFamily.
extern "C" JNIEXPORT void JNICALL Java_com_rawr_camera_integration_NativePreviewEngine_setAssetManager(
    JNIEnv* env, jobject, jlong h, jobject assetManager) {
    rawrcam::session::setAssetManager(handle(h), AAssetManager_fromJava(env, assetManager));
}
extern "C" JNIEXPORT void JNICALL Java_com_rawr_camera_integration_NativePreviewEngine_setFilmSimEnabled(JNIEnv*,
                                                                                                         jobject,
                                                                                                         jlong h,
                                                                                                         jboolean v) {
    rawrcam::session::setFilmSimEnabled(handle(h), v == JNI_TRUE);
}
extern "C" JNIEXPORT void JNICALL
Java_com_rawr_camera_integration_NativePreviewEngine_setViewfinderDivisor(JNIEnv*, jobject, jlong h, jint v) {
    rawrcam::session::setViewfinderDivisor(handle(h), static_cast<uint32_t>(v < 1 ? 1 : (v > 4 ? 4 : v)));
}
extern "C" JNIEXPORT void JNICALL Java_com_rawr_camera_integration_NativePreviewEngine_setFilmSimLook(
    JNIEnv* env, jobject, jlong h, jfloatArray values, jintArray enums) {
    if (auto look = decodeFilmLook(env, values, enums)) {
        __android_log_print(ANDROID_LOG_INFO, "RawrCamNative",
                            "FILM_SIM_LOOK film=%d paper=%d method=%d process=%d scanInvert=%d", (int)look->film,
                            (int)look->paper, (int)look->rgbToRawMethod, (int)look->process,
                            (int)look->scanNegativeInvert);
        rawrcam::session::setFilmSimLook(handle(h), *look);
    }
}

extern "C" JNIEXPORT void JNICALL Java_com_rawr_camera_integration_InternalTraceNative_clear(JNIEnv*, jobject) {
    rawrcam::diagnostics::RuntimeTraceRecorder::instance().clear();
}
extern "C" JNIEXPORT void JNICALL Java_com_rawr_camera_integration_InternalTraceNative_setRetainedRows(JNIEnv*, jobject,
                                                                                                       jint rows) {
    rawrcam::diagnostics::RuntimeTraceRecorder::instance().setRetainedRows(static_cast<uint32_t>(std::max(rows, 0)));
}
extern "C" JNIEXPORT void JNICALL Java_com_rawr_camera_integration_InternalTraceNative_recordZslArtifactPublished(
    JNIEnv*, jobject, jboolean success, jlong bytes) {
    rawrcam::diagnostics::RuntimeTraceRecorder::instance().record(
        rawrcam::diagnostics::RuntimeTraceStage::ZslArtifactPublished, 0, 0, -1, 0, 0, success == JNI_TRUE ? 1u : 0u,
        static_cast<int64_t>(bytes));
}

extern "C" JNIEXPORT jlong JNICALL Java_com_rawr_camera_integration_NativePreviewEngine_recoverStill(
    JNIEnv* env, jobject, jlong h, jstring name, jboolean multiframe, jint dng, jint merged, jint jpeg) {
    return static_cast<jlong>(
        rawrcam::session::recoverStill(handle(h), jString(env, name), multiframe == JNI_TRUE, dng, merged, jpeg));
}

extern "C" JNIEXPORT jstring JNICALL Java_com_rawr_camera_integration_SystemPropertiesUtil_read(
    JNIEnv* env, jobject, jstring key) {
    char value[PROP_VALUE_MAX]{};
    __system_property_get(jString(env, key).c_str(), value);
    return env->NewStringUTF(value);
}

extern "C" JNIEXPORT void JNICALL
Java_com_rawr_camera_integration_NativePreviewEngine_installCameraTagResolver(JNIEnv* env, jobject, jobject resolver) {
    rawrcam::camera::installCameraTagResolver(env, resolver);
}

extern "C" JNIEXPORT void JNICALL
Java_com_rawr_camera_integration_NativePreviewEngine_installVendorCameraGate(JNIEnv* env, jobject, jobject gate) {
    rawrcam::camera::installVendorCameraGate(env, gate);
}
