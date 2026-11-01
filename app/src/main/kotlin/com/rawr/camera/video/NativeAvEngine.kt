package com.rawr.camera.video

import android.view.Surface
import com.rawr.camera.storage.CaptureTime

/** JNI handle for the C++ recording engine. The output fd transfers ownership on nativeStart. */
internal class NativeAvEngine {
    companion object {
        init { System.loadLibrary("rawrcam_native") }
    }

    external fun nativeStart(fd: Int, width: Int, height: Int, fps: Int, bitrate: Int,
                             intraFrames: Int, rotationDegrees: Int, bitrateMode: Int,
                             maxBFrames: Int, audioChannels: Int, audioBitrate: Int,
                             audioPreset: Int, audioDeviceId: Int, audioGainDb: Float, audioLowCutHz: Int, audioPcmBits: Int,
                             bitDepth: Int, timestampPolicy: Int, deviceMake: String,
                             deviceModel: String, software: String, renderProfile: String,
                             gamut: String, transfer: String, log: Boolean, indexPath: String, gps: DoubleArray?,
                             captureTime: CaptureTime): Long
    /** Finishes an interrupted recording from its sample index; closes [fd]. Throws on failure. */
    external fun nativeRecover(fd: Int, indexPath: String, deviceMake: String, deviceModel: String,
                               software: String, renderProfile: String, gamut: String, transfer: String,
                               bitDepth: Int, log: Boolean)
    external fun nativeSupportedTimestampPolicies(): IntArray
    external fun nativeSurface(handle: Long): Surface
    external fun nativeStats(handle: Long): String
    external fun nativeStop(handle: Long): String
}
