package com.rawr.camera.integration

import android.app.Application
import android.content.Intent
import android.net.Uri
import android.os.Build
import android.os.ParcelFileDescriptor
import android.util.Log
import androidx.core.content.ContextCompat
import com.rawr.camera.model.CaptureOutputFormat
import com.rawr.camera.settings.model.SettingsValues
import com.rawr.camera.settings.model.StillPath
import com.rawr.camera.settings.model.describeFilm
import com.rawr.camera.settings.model.resolve
import com.rawr.camera.settings.model.toModesArray
import com.rawr.camera.settings.model.toStrengthsArray
import com.rawr.camera.storage.ArtifactDiagnosticExporter
import com.rawr.camera.storage.CaptureTime
import com.rawr.camera.storage.StillProcessingService
import com.rawr.camera.storage.StillOutputStore
import com.rawr.camera.storage.ZslBundleExportService
import java.io.File
import java.util.concurrent.ConcurrentHashMap

class StillCaptureCoordinator(
    private val application: Application,
    private val preview: RawPreviewCoordinator,
    private val settings: () -> SettingsValues,
    private val onJpegPublished: (Uri) -> Unit = {},
    private val onDngPublished: () -> Unit = {},
    private val gpsSnapshot: () -> com.rawr.camera.location.CaptureGps?
) {
    data class CompletionEvent(val requestId: Long, val artifact: Artifact, val success: Boolean, val jpegRequested: Boolean = false,
                                val filmFallbackMemory: Boolean = false, val dngFailed: Boolean = false) {
        enum class Artifact { Queued, Dng, Jpeg, Finished }
    }

    private data class RequestState(
        val jpegRequested: Boolean,
        var remainingDngs: Int = 1,
        var allDngsSuccessful: Boolean = true,
        var dngDone: Boolean = remainingDngs == 0,
        var fallback: Boolean = false,
        var jpegSuccessful: Boolean = !jpegRequested,
        val completedDngNames: MutableSet<String> = mutableSetOf(),
        var jpegDone: Boolean = !jpegRequested
    )

    private val outputs = StillOutputStore(application)
    private val artifactExporter = ArtifactDiagnosticExporter(application)

    private data class ArtifactRequest(
        val baseName: String,
        val scratchJpeg: File?,
        val waitForZsl: Boolean,
        val saveLocationId: String
    )

    private val artifactRequests = ConcurrentHashMap<Long, ArtifactRequest>()
    private val requestStates = ConcurrentHashMap<Long, RequestState>()
    private val fallbackAliases = ConcurrentHashMap<Long, Long>()
    private val finishedEvents = mutableListOf<CompletionEvent>()

    private var closeWhenDrained: (() -> Unit)? = null
    private var closing = false
    private data class CaptureInput(val settings: SettingsValues, val captureTime: CaptureTime, val releaseAssets: () -> Unit,
                                    var frozenAssetId: String? = null,
                                    val gps: com.rawr.camera.location.CaptureGps? = null)
    private val queuedInputs = java.util.concurrent.ConcurrentLinkedQueue<CaptureInput>()
    private val activeCaptures = java.util.concurrent.atomic.AtomicInteger(0)

    // Called directly by the foreground shutter event, before dispatching provider
    // IO to the capture worker. Holds camera/process ownership across immediate Home.
    @Synchronized
    fun enqueueCapture(format: CaptureOutputFormat) {
        check(!closing) { "Capture coordinator is closing" }
        StillProcessingService.acquire(application, this)
        val snapshot = settings().copy(dngEnabled = format.dng, jpegEnabled = format.jpeg)
        val releaseAssets = com.rawr.camera.settings.preferences.CaptureLutPins.acquire(
            snapshot.userLutProfiles.firstOrNull { it.id == snapshot.selectedUserLutProfileId })
        queuedInputs.add(CaptureInput(snapshot, CaptureTime.snapshot(), releaseAssets,
            gps = if (snapshot.locationTagging) gpsSnapshot() else null))
        activeCaptures.incrementAndGet()
        preview.beginStillCapture()
    }

    @Synchronized
    fun closeWhenIdle(close: () -> Unit) {
        closing = true
        closeWhenDrained = close
        closeIfDrained()
    }

    @Synchronized
    private fun releaseCapture() {
        activeCaptures.decrementAndGet()
        preview.endStillCapture()
        StillProcessingService.release(this)
        closeIfDrained()
    }

    private fun closeIfDrained() {
        if (activeCaptures.get() != 0) return
        closeWhenDrained?.also { closeWhenDrained = null }?.invoke()
    }

    fun capture(format: CaptureOutputFormat): Long {
        val input = checkNotNull(queuedInputs.poll()) { "Capture requires foreground admission" }
        var accepted = false
        try {
            val id = captureInternal(format.jpeg, input)
            accepted = id != 0L
            return id
        } finally {
            input.releaseAssets()
            if (!accepted) {
                preview.cancelPreparedMultiframeCapture()
                input.frozenAssetId?.let { com.rawr.camera.storage.releaseCaptureAssets(application.filesDir, it) }
                releaseCapture()
            }
        }
    }

    private fun captureInternal(jpegEnabled: Boolean, input: CaptureInput): Long {
        val now = input.captureTime.wallClockUnixMillis
        val current = try { CaptureRecipe.freezeAssets(application, input.settings) }
            finally { input.releaseAssets() }
        input.frozenAssetId = current.selectedUserLutProfileId
        val filmDescription =
            if (current.filmSimEnabled) current.filmSimLook.describeFilm() else null
        val jpegQuality = current.imageTone.jpegQuality.toInt().coerceIn(95, 100)
        val multiframeFrames =
            if (current.experimentalMultiframeEnabled) {
                preview.prepareExperimentalMultiframe(
                    current.multiframeTuning.zslFrames(), current.multiframeTuning.postShutterFrames()
                ).also { frames ->
                    check(frames >= 0) { "Full-resolution acquisition failed; no replacement capture was submitted" }
                }.coerceAtMost(30)
            } else {
                0
            }
        val multiframe = multiframeFrames >= 2
        // File names count every merged frame, including HDR+ Bracketed's post-shutter dark frames.
        // A burst longer than requested already holds the long frames' slots (skipped).
        val postShutterFrames =
            if (multiframeFrames > current.multiframeTuning.zslFrames()) 0 else current.multiframeTuning.postShutterFrames()
        val namedFrames = multiframeFrames + postShutterFrames
        if (current.experimentalMultiframeEnabled && !multiframe) {
            Log.i(TAG, "Multiframe ring unavailable; capturing a fresh single frame")
        }
        val output =
            outputs.prepare(
                current.saveLocationId,
                now,
                jpegEnabled,
                if (multiframe) namedFrames else 0,
                dngEnabled = current.dngEnabled,
                saveBaseDng = current.saveBaseDng,
                filmFallback = current.filmSimEnabled,
                recipe = CaptureRecipe.encode(current, application.filesDir)
            ) ?: run {
                if (multiframe) preview.cancelPreparedMultiframeCapture()
                com.rawr.camera.storage.releaseCaptureAssets(application.filesDir, current.selectedUserLutProfileId.orEmpty())
                return 0L
            }
        val diagnosticsEnabled = current.pipelineDiagnosticsEnabled
        val zslPersistRequested = current.experimentalMultiframeEnabled && current.persistZslRingOnShutterEnabled
        val artifactExportRequested = diagnosticsEnabled
        if (artifactExportRequested || zslPersistRequested) artifactExporter.clearPrivateLatest()
        val scratchJpeg =
            if (diagnosticsEnabled && !jpegEnabled) {
                File(application.cacheDir, "rawrcam_artifact_scratch_${now}_${System.nanoTime()}.jpg").also { it.delete() }
            } else {
                null
            }
        val diagnosticFd =
            scratchJpeg?.let { file ->
                runCatching {
                    ParcelFileDescriptor
                        .open(
                            file,
                            ParcelFileDescriptor.MODE_CREATE or ParcelFileDescriptor.MODE_TRUNCATE or
                                ParcelFileDescriptor.MODE_WRITE_ONLY
                        ).detachFd()
                }.getOrElse {
                    outputs.abandon(output)
                    file.delete()
                    return 0L
                }
            } ?: -1
        val jpegFd = output.jpeg?.fd ?: diagnosticFd
        val jpegDisplayName =
            output.jpeg?.displayName ?: output.jobName.removeSuffix(".dng") + ".diagnostic.jpg"
        val rendererDisplayName = current.captureRendererDisplayName()
        val offsetMinutes = input.captureTime.utcOffsetMinutes
        // Single path arbitration point: the sealed config resolves to
        // exactly what the still pipeline may run (RAW only here; wavelet
        // only here). Multiframe resolves separately below. Native keeps its
        // device gates (float16, geometry) as backstop with SKIP emits.
        val singleDenoise = current.photoDenoise.resolve(StillPath.SINGLE)
        val requestId =
            if (multiframe) {
                val mfDenoise = current.photoDenoise.resolve(StillPath.MULTIFRAME)
                mfDenoise.skipped.forEach { Log.i(TAG, "Denoise resolve path=multiframe skip=$it") }
                preview.startPreparedMultiframeCapture(
                    baseDngOutputFd = (if (output.conditionalDng) -1 else output.dng?.fd ?: -1),
                    mergedDngOutputFd = (if (output.conditionalDng) -1 else output.mergedDng?.fd ?: -1),
                    jpegOutputFd = jpegFd,
                    jpegQuality = jpegQuality,
                    jpegChromaSubsamplingId = current.imageTone.jpegChromaSubsamplingId,
                    dngCompressionId = current.dngCompressionId,
                    pipelineDiagnosticsEnabled = diagnosticsEnabled,
                    colorRenderProfile = current.colorRenderProfile,
                    importedLutProfileId = current.selectedUserLutProfileId.orEmpty(),
                    rendererDisplayName = rendererDisplayName,
                    demosaicAlgorithm = current.demosaicAlgorithm,
                    dualAutoContrast = current.dualAutoContrast,
                    dualContrastPercent = current.dualContrastPercent,
                    fccSteps = current.photoFccSteps,
                    lensShadingCorrectionEnabled = current.photoLensShadingEnabled,
                    highlightReconstructionEnabled = current.photoHighlightEnabled,
                    distortionCorrectionEnabled = current.distortionCorrectionEnabled,
                    filmDescription = filmDescription,
                    defringeEnabled = current.photoDefringeEnabled,
                    defringeStrength = current.photoDefringeStrength,
                    defringeEdgeThreshold = current.photoDefringeEdgeThreshold,
                    defringeLumaFloor = current.photoDefringeLumaFloor,
                    // Resolved multiframe intent as the collapsed JNI spec:
                    // RAW never survives merge, wavelet is unsupported on
                    // merged input. master/method derive inside toModesArray
                    // so native gating stays consistent.
                    denoiseModes = mfDenoise.toModesArray(),
                    denoiseStrengths = mfDenoise.toStrengthsArray(),
                    captureRecipe = output.recipe,
                    captureTone = CaptureRecipe.nativeTone(current),
                    captureFilmValues = current.filmSimLook.toFloatArray(),
                    captureFilmEnums = current.filmSimLook.toIntArray(),
                    captureFilmEnabled = current.filmSimEnabled,
                    ultraHdrEnabled = current.ultraHdrEnabled,
                    wallClockMillis = now,
                    gps = input.gps?.nativeValues(),
                    utcOffsetMinutes = offsetMinutes,
                    deviceMake = Build.MANUFACTURER,
                    deviceModel = DeviceNames.marketingModel(),
                    baseDngDisplayName = output.jobName,
                    mergedDngDisplayName = output.mergedDng?.displayName ?: "${output.baseName}_MF$namedFrames.dng",
                    jpegDisplayName = jpegDisplayName,
                    dumpRzslRequested = zslPersistRequested,
                    multiframeTuning = current.multiframeTuning,
                    multiframeBaseFrameMode = current.multiframeBaseFrameMode,
                    multiframeChromaDenoise = current.multiframeChromaDenoise
                )
            } else {
                preview.requestRawStillCapture(
                    dngOutputFd = (if (output.conditionalDng) -1 else output.dng?.fd ?: -1),
                    jpegOutputFd = jpegFd,
                    jpegQuality = jpegQuality,
                    jpegChromaSubsamplingId = current.imageTone.jpegChromaSubsamplingId,
                    dngCompressionId = current.dngCompressionId,
                    pipelineDiagnosticsEnabled = diagnosticsEnabled,
                    colorRenderProfile = current.colorRenderProfile,
                    importedLutProfileId = current.selectedUserLutProfileId.orEmpty(),
                    rendererDisplayName = rendererDisplayName,
                    demosaicAlgorithm = current.demosaicAlgorithm,
                    dualAutoContrast = current.dualAutoContrast,
                    dualContrastPercent = current.dualContrastPercent,
                    fccSteps = current.photoFccSteps,
                    lensShadingCorrectionEnabled = current.photoLensShadingEnabled,
                    highlightReconstructionEnabled = current.photoHighlightEnabled,
                    distortionCorrectionEnabled = current.distortionCorrectionEnabled,
                    filmDescription = filmDescription,
                    defringeEnabled = current.photoDefringeEnabled,
                    defringeStrength = current.photoDefringeStrength,
                    defringeEdgeThreshold = current.photoDefringeEdgeThreshold,
                    defringeLumaFloor = current.photoDefringeLumaFloor,
                    denoiseModes = singleDenoise.toModesArray(),
                    denoiseStrengths = singleDenoise.toStrengthsArray(),
                    captureRecipe = output.recipe,
                    captureTone = CaptureRecipe.nativeTone(current),
                    captureFilmValues = current.filmSimLook.toFloatArray(),
                    captureFilmEnums = current.filmSimLook.toIntArray(),
                    captureFilmEnabled = current.filmSimEnabled,
                    ultraHdrEnabled = current.ultraHdrEnabled,
                    wallClockMillis = now,
                    gps = input.gps?.nativeValues(),
                    utcOffsetMinutes = offsetMinutes,
                    deviceMake = Build.MANUFACTURER,
                    deviceModel = DeviceNames.marketingModel(),
                    dngDisplayName = output.jobName,
                    jpegDisplayName = jpegDisplayName
                )
            }
        // Native accepts ownership of both detached FDs even on rejection, so only remove rows here.
        if (requestId == 0L) {
            scratchJpeg?.delete()
            // FDs have already been closed by native; remove provider targets without re-closing.
            outputs.discardPublishedTargets(output)
        } else {
            outputs.register(requestId, output)
            requestStates[requestId] =
                RequestState(
                    jpegRequested = output.jpeg != null,
                    remainingDngs = output.requestedDngCount,
                    jpegDone = jpegFd < 0
                )
            if (zslPersistRequested) {
                val baseName = output.jobName.removeSuffix(".dng")
                ContextCompat.startForegroundService(
                    application,
                    Intent(application, ZslBundleExportService::class.java)
                        .putExtra(ZslBundleExportService.EXTRA_BASE_NAME, baseName)
                        .putExtra(ZslBundleExportService.EXTRA_SAVE_LOCATION_ID, current.saveLocationId)
                        .putExtra(ZslBundleExportService.EXTRA_DUMP_STATUS, 0)
                )
            }
            if (artifactExportRequested) {
                artifactRequests[requestId] =
                    ArtifactRequest(
                        output.jobName.removeSuffix(".dng"),
                        scratchJpeg,
                        zslPersistRequested,
                        current.saveLocationId
                    )
            }
        }
        return requestId
    }

    private val recoveredEvents = mutableListOf<CompletionEvent>()
    private var nextRecoveryScan = 0L
    private fun recoverPending() {
        if (!preview.canRecoverStills() || android.os.SystemClock.elapsedRealtime() < nextRecoveryScan) return
        nextRecoveryScan = android.os.SystemClock.elapsedRealtime() + 2000
        while (true) {
            val output = outputs.nextRecovery() ?: return
            try {
                StillProcessingService.acquire(application, this)
            } catch (error: RuntimeException) {
                listOfNotNull(output.dng, output.mergedDng, output.jpeg).forEach {
                    runCatching { ParcelFileDescriptor.adoptFd(it.fd).close() }
                }
                outputs.releaseRecovery(output, closeConditional = false)
                Log.e(TAG, "Recovery service unavailable", error)
                return
            }
            activeCaptures.incrementAndGet()
            preview.beginStillCapture()
            val id = preview.recoverStill(output.jobName, output.multiframe,
                if (output.conditionalDng) -1 else output.dng?.fd ?: -1,
                if (output.conditionalDng) -1 else output.mergedDng?.fd ?: -1, output.jpeg?.fd ?: -1)
            if (id == 0L) { outputs.releaseRecovery(output); releaseCapture(); return }
            outputs.register(id, output)
            requestStates[id] = RequestState(output.jpeg != null, remainingDngs = output.requestedDngCount, fallback = output.fallback)
            recoveredEvents += CompletionEvent(id, CompletionEvent.Artifact.Queued, true, output.jpeg != null)
            Log.i(TAG, "Resuming persisted capture: ${output.jobName} requestId=$id")
        }
    }

    fun pollCompletion(): List<CompletionEvent> {
        runCatching { recoverPending() }.onFailure { Log.e(TAG, "Capture recovery unavailable", it) }
        val events = recoveredEvents.toMutableList()
        recoveredEvents.clear()
        preview.pollDngWriteCompletion().takeIf { it.isNotBlank() }?.let { raw ->
            val parts = raw.split('\t', limit = 4)
            val nativeId = parts.getOrNull(0)?.toLongOrNull() ?: return@let
            val requestId = fallbackAliases[nativeId] ?: nativeId
            val success = parts.getOrNull(1) == "1"
            val displayName = parts.getOrNull(2).orEmpty()
            if (!success) {
                Log.e(TAG, "DNG capture failed for $displayName: ${parts.getOrNull(3).orEmpty()}")
                runCatching { outputs.recordFailure(requestId, parts.getOrNull(3).orEmpty()) }
            }
            val state = requestStates[requestId] ?: return@let
            if (state.remainingDngs == 0 || !state.completedDngNames.add(displayName)) return@let
            val publication = outputs.completeDng(requestId, displayName, success)
            if (publication.success) onDngPublished()
            state.let { state ->
                state.allDngsSuccessful = state.allDngsSuccessful && publication.success
                if (state.remainingDngs > 0) state.remainingDngs -= 1
                if (publication.allDone || state.remainingDngs == 0) {
                    state.dngDone = true
                    events +=
                        CompletionEvent(
                            requestId,
                            CompletionEvent.Artifact.Dng,
                            state.allDngsSuccessful
                        )
                    finishRequestIfDone(requestId)
                }
            }
        }
        preview.pollJpegWriteCompletion().takeIf { it.isNotBlank() }?.let { raw ->
            val parts = raw.split('\t', limit = 5)
            val nativeId = parts.getOrNull(0)?.toLongOrNull() ?: return@let
            val requestId = fallbackAliases[nativeId] ?: nativeId
            val success = parts.getOrNull(1) == "1"
            if (!success && parts.getOrNull(4) != "1") {
                Log.e(TAG, "JPEG capture failed: ${parts.getOrNull(3).orEmpty()}")
                runCatching { outputs.recordFailure(requestId, parts.getOrNull(3).orEmpty()) }
            }
            val filmFallbackMemory = parts.getOrNull(4) == "1"
            val state = requestStates[requestId] ?: return@let
            if (filmFallbackMemory && state.jpegRequested) {
                state.fallback = true
                val replacement = try { outputs.useDngFallback(requestId) }
                    catch (error: Exception) {
                        Log.e(TAG, "DNG substitution deferred; retaining capture input", error)
                        outputs.deferFallback(requestId, error.message ?: "fallback_preparation_failed")
                        state.allDngsSuccessful = false
                        null
                    }
                state.jpegDone = true
                if (replacement != null) {
                    state.dngDone = false
                    state.remainingDngs = 1
                    val id = preview.recoverStill(replacement.jobName, replacement.multiframe,
                        replacement.dng?.fd ?: -1, replacement.mergedDng?.fd ?: -1, -1)
                    if (id != 0L) fallbackAliases[id] = requestId
                    else {
                        outputs.completeDng(requestId, requireNotNull(replacement.primaryDng).displayName, false)
                        state.dngDone = true
                        state.allDngsSuccessful = false
                    }
                }
                outputs.completeJpeg(requestId, false) // Skipped JPEG is terminal, not retryable.
            } else {
                val publication = outputs.completeJpeg(requestId, success)
                publication.uri?.let(onJpegPublished)
                state.jpegDone = true
                state.jpegSuccessful = !state.jpegRequested || publication.success
                events += CompletionEvent(requestId, CompletionEvent.Artifact.Jpeg, publication.success)
            }
            finishRequestIfDone(requestId)
        }
        events += finishedEvents
        finishedEvents.clear()
        return events
    }

    private fun finishRequestIfDone(requestId: Long) {
        val state = requestStates[requestId] ?: return
        if (!state.dngDone || !state.jpegDone) return
        if (requestStates.remove(requestId, state)) {
            fallbackAliases.entries.removeAll { it.value == requestId }
            val success = state.allDngsSuccessful && (state.fallback || state.jpegSuccessful)
            finishedEvents += CompletionEvent(requestId, CompletionEvent.Artifact.Finished, success,
                filmFallbackMemory = state.fallback, dngFailed = !state.allDngsSuccessful)
            if (state.fallback && success) android.os.Handler(android.os.Looper.getMainLooper()).post {
                android.widget.Toast.makeText(application,
                    "Not enough memory for the film look — saved DNG", android.widget.Toast.LENGTH_LONG).show()
            }
            val artifact = artifactRequests.remove(requestId)
            artifact?.scratchJpeg?.delete()
            if (artifact != null) {
                Thread({
                    // Diagnostic export is best-effort: a storage error must never kill the camera.
                    runCatching {
                        artifactExporter.exportLatest(artifact.baseName, artifact.waitForZsl, artifact.saveLocationId)
                    }.onFailure { android.util.Log.e(TAG, "Artifact export failed", it) }
                }, "rawr-artifact-export").start()
            }
            releaseCapture()
        }
    }

    private companion object {
        const val TAG = "RawrCamCapture"
    }
}
