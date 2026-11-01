package com.rawr.camera.video

import androidx.core.net.toUri
import android.Manifest
import android.content.Context
import android.content.pm.PackageManager
import android.media.MediaExtractor
import android.media.MediaFormat
import android.net.Uri
import android.os.Build
import android.os.SystemClock
import android.view.Surface
import com.rawr.camera.BuildConfig
import com.rawr.camera.integration.RawPreviewCoordinator
import com.rawr.camera.storage.CaptureFileNames
import com.rawr.camera.storage.CaptureTime
import com.rawr.camera.storage.VideoOutputStore
import org.json.JSONObject
import java.io.File

/** Android lifecycle and storage adapter for the C++ HEVC/AAC/MP4 recorder. */
class NativeVideoRecorder(private val context: Context, private val preview: RawPreviewCoordinator) : VideoRecording {
    companion object {
        /** Compact labels for the recording monitor's DROP cell. */
        private val DROP_REASON_LABELS = mapOf(
            "ingress" to "in", "noSlot" to "gpu", "encoderBusy" to "enc", "presentFail" to "pres",
            "pairer" to "pair", "stale" to "old", "submitFail" to "err", "geometry" to "size")

        /** The UI can enumerate VideoTimestampPolicy.entries and disable unsupported choices. */
        fun supportedTimestampPolicies(): Set<VideoTimestampPolicy> {
            val supportedIds = NativeAvEngine().nativeSupportedTimestampPolicies()
            return VideoTimestampPolicy.entries.filter { it.nativeId in supportedIds }.toSet()
        }

        /**
         * Publishes recordings interrupted before publication. A file that is already
         * playable (stopped, not yet published) is published as is; one cut off mid-take
         * gets its moov rebuilt from the sample index first, keeping everything written.
         */
        fun recoverInterrupted(context: Context) {
            recoverInterrupted(context, { hasVideoAndAudio(context, it) }) { uri, index, start ->
                val fd = requireNotNull(context.contentResolver.openFileDescriptor(uri, "rw")).use { it.detachFd() }
                NativeAvEngine().nativeRecover(fd, index.path, Build.MANUFACTURER.orEmpty(), Build.MODEL.orEmpty(),
                    "RAWR ${BuildConfig.VERSION_NAME}", start.optString("renderProfile"),
                    start.optString("metadataGamut", "Rec.709 / sRGB"), start.optString("metadataTransfer", "BT.709"),
                    start.optInt("bitDepth", 10), start.optString("logProfile").let { it.isNotEmpty() && it != "null" })
            }
        }

        internal fun recoverInterrupted(
            context: Context,
            playable: (Uri) -> Boolean,
            reconstruct: (Uri, File, JSONObject) -> Unit
        ) {
            listOf(File(context.noBackupFilesDir, "video_jobs"), context.filesDir).flatMap { root -> root.listFiles { file ->
                file.name.startsWith("rawr_native_video_") && file.name.endsWith(".jsonl")
            }?.toList().orEmpty() }.forEach { journal ->
                val index = File(journal.path.removeSuffix(".jsonl") + ".idx")
                runCatching {
                    var first: String? = null
                    var last: String? = null
                    journal.bufferedReader().use { reader ->
                        while (true) {
                            val line = reader.readLine() ?: break
                            if (line.isBlank()) continue
                            if (first == null) first = line
                            last = line
                        }
                    }
                    if (first == null || last == null) return@runCatching
                    val lastEvent = JSONObject(last).optString("event")
                    if (lastEvent == "published" || lastEvent == "recovered") {
                        index.delete()
                        return@runCatching
                    }
                    val start = JSONObject(first)
                    val uri = start.getString("uri").toUri()
                    if (!playable(uri)) {
                        check(index.exists()) { "unplayable and no sample index" }
                        reconstruct(uri, index, start)
                        check(playable(uri)) { "recovered file still unplayable" }
                    }
                    VideoOutputStore(context).publish(uri)
                    journal.appendText(JSONObject().put("event", "recovered").toString() + "\n")
                    index.delete()
                }.onFailure { android.util.Log.w("RawrVideo", "Could not recover ${journal.name}", it) }
            }
        }

        private fun hasVideoAndAudio(context: Context, uri: android.net.Uri): Boolean {
            val extractor = MediaExtractor()
            try {
                extractor.setDataSource(context, uri, null)
                var video = false
                var audio = false
                for (index in 0 until extractor.trackCount) {
                    val mime = extractor.getTrackFormat(index).getString(MediaFormat.KEY_MIME).orEmpty()
                    extractor.selectTrack(index)
                    val hasSample = extractor.sampleTime >= 0
                    extractor.unselectTrack(index)
                    if (hasSample && mime.startsWith("video/")) video = true
                    if (hasSample && mime.startsWith("audio/")) audio = true
                }
                return video && audio
            } catch (_: Exception) {
                return false
            } finally {
                extractor.release()
            }
        }
    }

    data class Settings(val fps: Int = 30, val bitrate: Int = 12_000_000, val intraFrames: Int = 30,
                        val width: Int = 1920, val height: Int = 1080, val mode: String = "1080p",
                        val deviceRotationDegrees: Int = 0,
                        /** -1 lets the codec choose; 0=CQ, 1=VBR, 2=CBR (default). */
                        val bitrateMode: Int = 2,
                        /** -1 lets the codec choose; 0 disables B frames. */
                        val maxBFrames: Int = -1,
                        /** 1=mono, 2=stereo (default). Stereo falls back to mono at 128k if unsupported. */
                        val audioChannels: Int = 2,
                        /** AAC bitrate. Stereo default 192k; mono fallback steps down to 128k. */
                        val audioBitrate: Int = 192_000,
                        /** 0 = AAC in .mp4; 16/24 = PCM in .mov. */
                        val audioPcmBits: Int = 0,
                        /** Source, route, gain and low-cut; [audioChannels] above wins for channels. */
                        val audio: VideoAudioInput = VideoAudioInput(),
                        /** 8 = HEVC Main, 10 = HEVC Main10 (default). */
                        val bitDepth: Int = 10,
                        val renderProfile: String = "RAWR NTRL",
                        val renderProfileId: Int = 0,
                        val logProfile: com.rawr.camera.settings.model.VideoLogProfile? = null,
                        val timestampPolicy: VideoTimestampPolicy = VideoTimestampPolicy.REALTIME,
                        val saveLocationId: String = "storage.dcim_camera",
                        val gps: com.rawr.camera.location.CaptureGps? = null) {
        /** Rec.709 and AgX (video) write BT.709; other display profiles write sRGB. */
        val rec709: Boolean get() = renderProfileId == com.rawr.camera.settings.model.ColorRenderProfile.Rec709.nativeId ||
            renderProfileId == com.rawr.camera.settings.model.ColorRenderProfile.AGX_REC709_NATIVE_ID
    }

    private val engine = NativeAvEngine()
    private val outputStore = VideoOutputStore(context)
    private var handle = 0L
    private var surface: Surface? = null
    private var uri: Uri? = null
    private var journal: File? = null
    private var index: File? = null
    private var metadataGamut = ""
    private var metadataTransfer = ""
    private var settings = Settings()
    override val isRecording: Boolean get() = handle != 0L
    override val outputUri: Uri? get() = uri

    @Synchronized fun start(request: Settings = Settings()) {
        check(handle == 0L) { "Video recording already active" }
        check(context.checkSelfPermission(Manifest.permission.RECORD_AUDIO) == PackageManager.PERMISSION_GRANTED) {
            "Microphone permission required"
        }
        require(request.fps == 24 || request.fps == 30)
        require(request.bitrate in VideoEncoderLimits.MIN_BITRATE_MBPS * 1_000_000..
            VideoEncoderLimits.MAX_BITRATE_MBPS * 1_000_000)
        require(request.intraFrames in 1..60)
        require(request.bitrateMode in -1..2)
        require(request.maxBFrames in -1..4)
        require(request.audioChannels == 1 || request.audioChannels == 2)
        require(request.audioBitrate in 64_000..512_000)
        require(request.audioPcmBits in listOf(0, 16, 24))
        require(request.bitDepth == 8 || request.bitDepth == 10)
        require(request.logProfile == null || request.bitDepth == 10) { "LOG requires 10-bit recording" }
        require(request.timestampPolicy in supportedTimestampPolicies()) {
            "Timestamp policy ${request.timestampPolicy.label} is unavailable on this recording path"
        }
        require(request.width > 0 && request.height > 0 && request.width % 2 == 0 && request.height % 2 == 0)
        settings = request
        val rotation = preview.videoRotationDegrees(request.deviceRotationDegrees)
        check(rotation >= 0) { "Camera orientation is not ready for recording" }
        val captureTime = CaptureTime.snapshot()
        val displayName = CaptureFileNames.baseName(captureTime) + if (request.audioPcmBits > 0) ".mov" else ".mp4"
        uri = null
        journal = null
        try {
            check(preview.beginVideoFps(request.fps)) { "RAW camera could not lock ${request.fps} fps" }
            uri = outputStore.create(request.saveLocationId, displayName, captureTime.wallClockUnixMillis,
                if (request.audioPcmBits > 0) "video/quicktime" else "video/mp4")
            val recoveryRoot = File(context.noBackupFilesDir, "video_jobs")
            check(recoveryRoot.isDirectory || recoveryRoot.mkdirs()) { "Could not create video recovery directory" }
            // The detached descriptor is closed by C++, including nativeStart failure paths.
            // Read+write provides the seekable descriptor needed by the muxer and metadata patcher.
            // A provider that cannot supply one fails here; never redirect to another folder.
            val fd = requireNotNull(context.contentResolver.openFileDescriptor(requireNotNull(uri), "rw"))
                .use { it.detachFd() }
            val stem = "rawr_native_video_${System.currentTimeMillis()}"
            index = File(recoveryRoot, "$stem.idx")
            metadataGamut = request.logProfile?.gamut?.label ?: "Rec.709 / sRGB"
            metadataTransfer = request.logProfile?.transfer?.label ?: if (request.rec709) "BT.709" else "sRGB"
            handle = engine.nativeStart(fd, request.width, request.height, request.fps,
                request.bitrate, request.intraFrames, rotation, request.bitrateMode, request.maxBFrames,
                request.audioChannels, request.audioBitrate, request.audio.preset, request.audio.deviceId(context),
                request.audio.gainDb.toFloat(), request.audio.lowCutHz, request.audioPcmBits, request.bitDepth,
                request.timestampPolicy.nativeId, Build.MANUFACTURER.orEmpty(), Build.MODEL.orEmpty(),
                "RAWR ${BuildConfig.VERSION_NAME}", request.renderProfile,
                metadataGamut, metadataTransfer, request.logProfile != null, requireNotNull(index).path,
                request.gps?.nativeValues(), captureTime)
            check(handle != 0L) { "Native recorder did not start" }
            surface = engine.nativeSurface(handle)
            check(preview.setVideoSurface(surface, request.width, request.height, request.bitDepth)) {
                "GPU encoder surface unavailable at ${request.width}×${request.height}"
            }
            journal = File(recoveryRoot, "$stem.jsonl")
            append(JSONObject().put("event", "start").put("uri", uri.toString())
                .put("displayName", displayName).put("saveLocationId", request.saveLocationId)
                .put("bootNs", SystemClock.elapsedRealtimeNanos()).put("camera", preview.cameraControlSnapshot())
                .put("fps", request.fps).put("bitrate", request.bitrate)
                .put("mode", request.mode).put("width", request.width).put("height", request.height)
                .put("intraFrames", request.intraFrames).put("rotationDegrees", rotation)
                .put("bitrateMode", request.bitrateMode).put("maxBFrames", request.maxBFrames)
                .put("audioChannels", request.audioChannels).put("audioBitrate", request.audioBitrate)
                .put("audioPreset", request.audio.preset).put("audioRoute", request.audio.route.name)
                .put("audioGainDb", request.audio.gainDb).put("audioLowCutHz", request.audio.lowCutHz)
                .put("audioPcmBits", request.audioPcmBits)
                .put("bitDepth", request.bitDepth)
                .put("renderProfile", request.renderProfile).put("renderProfileId", request.renderProfileId)
                .put("logProfile", request.logProfile?.name)
                .put("gamut", request.logProfile?.gamut?.name ?: "SRgbRec709")
                .put("transfer", request.logProfile?.transfer?.name ?: if (request.rec709) "Rec709" else "SRgb")
                .put("timestampPolicy", request.timestampPolicy.name)
                .put("metadataGamut", metadataGamut).put("metadataTransfer", metadataTransfer)
                .put("processing", preview.videoImageSettingsSnapshot()?.toJson()))
        } catch (error: Throwable) {
            runCatching { preview.setVideoSurface(null) }
            runCatching { preview.endVideoFps() }
            surface?.release()
            surface = null
            if (handle != 0L) {
                runCatching { engine.nativeStop(handle) }
                handle = 0L
            }
            uri?.let { runCatching { outputStore.delete(it) } }
            uri = null
            index?.delete()
            throw error
        }
    }

    @Synchronized override fun stats(): JSONObject {
        val gpu = runCatching { JSONObject(preview.videoStats()) }.getOrDefault(JSONObject())
        val native = if (handle != 0L) JSONObject(engine.nativeStats(handle)) else JSONObject()
        val keys = native.keys()
        while (keys.hasNext()) {
            val key = keys.next()
            gpu.put(key, native.get(key))
        }
        // "dropped" counts camera frames lost on the way to the encoder. A slow
        // sensor cadence is a target shortfall instead; encodedFrameGaps stays
        // in the journal as the file-side cross-check.
        gpu.optJSONObject("dropReasons")?.let { r ->
            val top = r.keys().asSequence().maxByOrNull { r.optLong(it) }
            if (top != null && r.optLong(top) > 0) gpu.put("dropReason", DROP_REASON_LABELS[top] ?: top)
        }
        gpu.optJSONObject("stageMs")?.let { stage ->
            stage.optDouble("total").takeIf { it > 0 }?.let { gpu.put("videoGpuMs", it) }
            stage.optDouble("totalPeak").takeIf { it > 0 }?.let { gpu.put("videoGpuPeakMs", it) }
        }
        gpu.put("mode", settings.mode)
        gpu.put("timestampPolicy", settings.timestampPolicy.name)
        return gpu
    }

    @Synchronized override fun journalSnapshot() {
        if (isRecording) append(JSONObject().put("event", "sample")
            .put("bootNs", SystemClock.elapsedRealtimeNanos()).put("stats", stats())
            .put("camera", preview.cameraControlSnapshot())
            .put("processing", preview.videoImageSettingsSnapshot()?.toJson()))
    }

    @Synchronized override fun close() {
        val active = handle
        if (active == 0L) return
        var failure: Throwable? = null
        try {
            val detached = preview.setVideoSurface(null)
            // nativeStop frees the recorder whatever happens next: never hand this handle out again.
            handle = 0L
            val result = JSONObject(engine.nativeStop(active))
            append(JSONObject().put("event", "stop").put("result", result))
            result.optJSONObject("stats")?.let {
                runCatching { EncoderObservations.record(context, settings.maxBFrames, settings.intraFrames, it) }
            }
            check(detached) { "Could not detach video surface" }
            check(result.optBoolean("ok")) { "Native recording failed: ${result.optJSONObject("stats")?.optString("failure")}" }
            uri?.let(outputStore::publish)
            append(JSONObject().put("event", "published"))
            index?.delete()
        } catch (error: Throwable) {
            failure = error
            append(JSONObject().put("event", "error").put("error", error.toString()))
        } finally {
            if (handle != 0L) {
                runCatching { engine.nativeStop(handle) }
                handle = 0L
            }
            runCatching { preview.endVideoFps() }
            surface?.release()
            surface = null
        }
        if (failure != null) throw requireNotNull(failure)
    }

    private fun append(event: JSONObject) { journal?.appendText(event.toString() + "\n") }
}
