package com.rawr.camera.integration

import android.view.Surface
import com.rawr.camera.location.CaptureGps

/**
 * Thin JNI boundary for the application imaging engine.
 *
 * Native C++ owns Camera2 NDK, AImageReader/AHardwareBuffer, Vulkan, raw_preview,
 * TonemapEngine, synchronization and presentation. Kotlin passes only lifecycle,
 * semantic lens-selection and diagnostic intent.
 */
class NativePreviewEngine {
    companion object {
        init {
            System.loadLibrary("rawrcam_native")
        }
    }

    external fun installCameraTagResolver(resolver: CameraTagResolver)

    /** Installed once per process, before any camera opens. */
    external fun installVendorCameraGate(gate: VendorCameraGate)

    external fun create(filesDir: String, nativeLibraryDir: String, customDriverPath: String): Long

    external fun destroy(handle: Long)

    external fun setSurface(handle: Long, surface: Surface?, displayRotationDegrees: Int): Boolean
    external fun setVideoSurface(handle: Long, surface: Surface?, width: Int, height: Int, bitDepth: Int): Boolean
    external fun setVideoPreviewCrop(handle: Long, width: Int, height: Int)
    external fun prewarmVideo(handle: Long, width: Int, height: Int, bitDepth: Int)
    external fun releaseVideoProcessing(handle: Long)
    external fun videoStats(handle: Long): String
    external fun setVideoImageSettings(handle: Long, lensShadingEnabled: Boolean,
        highlightEnabled: Boolean, highlightMethod: Int, highlightThreshold: Float,
        highlightCompression: Float, fccSteps: Int, defringeStrength: Float,
        waveletDenoiseStrength: Float, waveletDenoiseDetail: Float,
        waveletDenoiseLuma: Float, waveletDenoiseScales: Int, distortionEnabled: Boolean)
    external fun setRecordingFps(handle: Long, fps: Int): Boolean

    external fun setScopeDeviceRotationDegrees(handle: Long, deviceRotationDegrees: Int)

    external fun setCameraActive(handle: Long, active: Boolean)
    external fun setPreviewForeground(handle: Long, foreground: Boolean)
    external fun setInitialConfigReady(handle: Long)
    external fun setStillCaptureInFlight(handle: Long, begin: Boolean)
    external fun tickCameraSession(handle: Long)
    external fun canRecoverStills(handle: Long): Boolean
    external fun setVideoMode(handle: Long, video: Boolean, fps: Int)

    external fun setOisEnabled(handle: Long, enabled: Boolean)


    external fun setAntibandingMode(handle: Long, mode: Int)

    external fun selectLens(handle: Long, lensId: String)

    /** Lens profile JSON (native CameraProfileJson); false when malformed or pinned by a debug setprop. */
    external fun setCameraProfile(handle: Long, json: String): Boolean

    /** This device's built-in lens profile JSON (matched natively from system properties). */
    external fun builtInCameraProfile(): String

    /** A lens camera profile (.dcp) file: [error, name, summary]; error is empty when the app can use it. */
    external fun inspectCameraProfile(path: String): Array<String>

    external fun setPreferredCameraId(handle: Long, cameraId: String?)

    external fun setPreferredAccessRoute(handle: Long, route: String?)

    external fun setPreferredColorMode(handle: Long, mode: String?)


    external fun setPipelineDiagnostic(handle: Long, mode: Int)

    external fun setExperimentalZeroCopy(handle: Long, enabled: Boolean)

    external fun setExperimentalMultiframeEnabled(handle: Long, enabled: Boolean)
    external fun setHdrPlusHeadroomMaxEv(handle: Long, maxEv: Float)

    external fun setPersistentEngineEnabled(handle: Long, enabled: Boolean)

    external fun setAssetManager(handle: Long, assetManager: android.content.res.AssetManager)

    external fun setFilmSimEnabled(handle: Long, enabled: Boolean)

    external fun setViewfinderDivisor(handle: Long, divisor: Int)

    external fun setFilmSimLook(handle: Long, values: FloatArray, enums: IntArray)

    external fun setLensShadingCorrectionEnabled(handle: Long, enabled: Boolean)

    external fun setHighlightReconstructionEnabled(handle: Long, enabled: Boolean)

    /** Undistorts the photo viewfinder with the lens profile calibration (photo Distortion correction). */
    external fun setDistortionCorrectionEnabled(handle: Long, enabled: Boolean)

    external fun setMaxAePostGain(handle: Long, gain: Float)

    external fun setPostGainKneeWidthEv(handle: Long, widthEv: Float)

    external fun setAutoKeyMode(handle: Long, mode: Int)
    external fun setLocalTone(handle: Long, shadowsEv: Float, highlightsEv: Float, sigma: Float, coarsestMip: Int, boostLocalContrast: Boolean)

    external fun setExpectedMergeFrames(handle: Long, frames: Int)

    external fun setAutoMinFps(handle: Long, fps: Int)
    external fun setAePriorityDisabled(handle: Long, disabled: Boolean)
    external fun setRawrAutoExposure(
        handle: Long,
        enabled: Boolean,
        nightLook: Int,
        isoMin: Int,
        isoMax: Int,
        shutterStartNs: Long,
        shutterMaxNs: Long,
        balanceEv: Float,
        maxDigitalGainEv: Float,
        speed: Int,
        metering: Int
    )

    external fun setPersistentDiagnosticsEnabled(handle: Long, enabled: Boolean)

    external fun setPersistZslRingEnabled(handle: Long, enabled: Boolean)



    external fun prepareExperimentalMultiframe(handle: Long, maxFrames: Int, longFrames: Int): Int

    external fun startPreparedMultiframeCapture(
        handle: Long,
        baseDngOutputFd: Int,
        mergedDngOutputFd: Int,
        jpegOutputFd: Int,
        jpegQuality: Int,
        jpegChromaSubsampling: Int,
        pipelineDiagnosticsEnabled: Boolean,
        colorRenderProfile: Int,
        importedLutProfileId: String,
        rendererDisplayName: String,
        demosaicAlgorithm: Int,
        dualAutoContrast: Boolean,
        dualContrastPercent: Float,
        fccSteps: Int,
        lensShadingCorrectionEnabled: Boolean,
        highlightReconstructionEnabled: Boolean,
        deviceRotationDegrees: Int,
        wallClockMillis: Long,
        utcOffsetMinutes: Int,
        deviceMake: String,
        deviceModel: String,
        baseDngDisplayName: String,
        mergedDngDisplayName: String,
        jpegDisplayName: String,
        dumpRzslRequested: Boolean,
        tuningValues: FloatArray,
        baseFrameMode: Int,
        distortionCorrectionEnabled: Boolean,
        filmDescription: String?,
        defringeEnabled: Boolean,
        defringeStrength: Float,
        defringeEdgeThreshold: Float,
        defringeLumaFloor: Float,
        // Collapsed denoise spec (see DenoiseConfig.toModesArray /
        // toStrengthsArray): modes[master, waveletScales],
        // strengths[waveletStrength, waveletDetail, waveletLuma].
        denoiseModes: IntArray,
        denoiseStrengths: FloatArray,
        captureRecipe: String = "",
        captureTone: FloatArray? = null,
        captureFilmValues: FloatArray? = null,
        captureFilmEnums: IntArray? = null,
        captureFilmEnabled: Boolean = false,
        ultraHdrEnabled: Boolean = false,
        // Fixed at 95: no UI setting exists yet, StillCaptureCoordinator never
        // forwards a quality. q95 keeps JPEG contours down in sky gradients.
        ultraHdrGainmapQuality: Int = 95,
        dngCompression: Int = 0,
        gps: CaptureGps? = null
    ): Long

    external fun cancelPreparedMultiframeCapture(handle: Long)

    external fun setInternalTraceCaptureEnabled(handle: Long, enabled: Boolean)

    external fun setCpuRawCopyProbeFrames(handle: Long, frames: Int)

    external fun setMonitoringOverlay(handle: Long, mode: Int, focusSensitivity: Float)

    external fun setScopePresentationState(
        handle: Long,
        types: IntArray,
        modes: IntArray,
        rotations: IntArray,
        rects: FloatArray
    )

    external fun requestRawStillCapture(
        handle: Long,
        dngOutputFd: Int,
        jpegOutputFd: Int,
        jpegQuality: Int,
        jpegChromaSubsampling: Int,
        pipelineDiagnosticsEnabled: Boolean,
        colorRenderProfile: Int,
        importedLutProfileId: String,
        rendererDisplayName: String,
        demosaicAlgorithm: Int,
        dualAutoContrast: Boolean,
        dualContrastPercent: Float,
        fccSteps: Int,
        lensShadingCorrectionEnabled: Boolean,
        highlightReconstructionEnabled: Boolean,
        deviceRotationDegrees: Int,
        wallClockMillis: Long,
        utcOffsetMinutes: Int,
        deviceMake: String,
        deviceModel: String,
        dngDisplayName: String,
        jpegDisplayName: String,
        distortionCorrectionEnabled: Boolean,
        filmDescription: String?,
        defringeEnabled: Boolean,
        defringeStrength: Float,
        defringeEdgeThreshold: Float,
        defringeLumaFloor: Float,
        // Collapsed denoise spec (see DenoiseConfig.toModesArray /
        // toStrengthsArray): modes[master, waveletScales],
        // strengths[waveletStrength, waveletDetail, waveletLuma].
        denoiseModes: IntArray,
        denoiseStrengths: FloatArray,
        captureRecipe: String = "",
        captureTone: FloatArray? = null,
        captureFilmValues: FloatArray? = null,
        captureFilmEnums: IntArray? = null,
        captureFilmEnabled: Boolean = false,
        ultraHdrEnabled: Boolean = false,
        // Fixed at 95: no UI setting exists yet, StillCaptureCoordinator never
        // forwards a quality. q95 keeps JPEG contours down in sky gradients.
        ultraHdrGainmapQuality: Int = 95,
        dngCompression: Int = 0,
        gps: CaptureGps? = null
    ): Long

    external fun pollDngWriteCompletion(handle: Long): String

    external fun pollJpegWriteCompletion(handle: Long): String

    external fun recoverStill(handle: Long, name: String, multiframe: Boolean, dng: Int, merged: Int, jpeg: Int): Long
    external fun runHighlightReplay(handle: Long, inputPath: String): String

    external fun cameraControlSnapshot(handle: Long): String

    /** Read-only camera discovery; no preview handle or open camera is required. */
    external fun cameraVideoCapabilitiesSnapshot(): String

    /** Camera inventory for lens settings (native CameraProbe); does not open any camera. */
    external fun probeCameras(): String

    /** Opens [cameraId] briefly to read default request keys and resolve [names] to tag ids. */
    external fun probeCameraKeys(cameraId: String, names: Array<String>): String

    /** Container orientation from the selected native camera context; -1 means unavailable. */
    external fun videoRotationDegrees(handle: Long, deviceRotationDegrees: Int): Int

    /** Face detections in display-normalized coords as JSON [[x,y,w,h,score],...]. */
    external fun faceDetectionsSnapshot(handle: Long): String

    external fun setExposureMode(handle: Long, mode: Int)

    external fun setManualExposureTimeNs(handle: Long, exposureTimeNs: Long)
    external fun setShutterAngleDegrees(handle: Long, degrees: Double)

    external fun setManualSensitivity(handle: Long, sensitivity: Int)

    external fun setExposureCompensationSteps(handle: Long, steps: Int)

    external fun setWhiteBalanceMode(handle: Long, mode: Int, requestId: Long)

    external fun setWhiteBalanceTempTint(handle: Long, temperatureK: Int, tint: Int, editedAxes: Int, requestId: Long)

    external fun setWhiteBalanceLocked(handle: Long, temperatureK: Int, tint: Int, requestId: Long)

    external fun setSpotAeTarget(handle: Long, active: Boolean, x: Float, y: Float)

    external fun setFocusMode(handle: Long, mode: Int, x: Float, y: Float, requestId: Long)

    external fun focusAt(handle: Long, x: Float, y: Float, requestId: Long)

    external fun clearTapAf(handle: Long, requestId: Long)

    external fun setManualFocusNormalized(handle: Long, normalized: Float, requestId: Long)

    external fun setQuickToneNativeValue(handle: Long, target: Int, value: Float)

    external fun setColorRenderProfile(handle: Long, profile: Int, importedLutProfileId: String)

    /** All AgX controls in AgxParameter (native AgxControl) order. */
    external fun setAgxParameters(handle: Long, controls: FloatArray)

    external fun setTonemapParameters(
        handle: Long,
        exposureEV: Float,
        blackPointEV: Float,
        shadowLiftEV: Float,
        midtoneLiftEV: Float,
        contrast: Float,
        whitePointEV: Float,
        highlightBiasEV: Float,
        saturation: Float,
        vibrance: Float
    )

}
