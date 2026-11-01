# Internal dependency declarations. Read by CMake and the architecture guard.
rawrcam_module(support
  DIRECTORY support
  DEPENDS
  SOURCES
)

rawrcam_module(geometry
  DIRECTORY geometry
  DEPENDS
  SOURCES
    geometry/DisplayToSensorMapper.cpp
    geometry/LensDistortion.cpp
)

rawrcam_module(metadata
  DIRECTORY metadata
  DEPENDS geometry
  SOURCES
    metadata/CameraMetadataReader.cpp
    metadata/MetadataValidation.cpp
    metadata/MetadataDiagnostics.cpp
)

rawrcam_module(color
  DIRECTORY color
  DEPENDS metadata
  SOURCES
    color/ColorMath.cpp
    color/ColorCalibration.cpp
    color/DcpProfile.cpp
    color/DcpRenderData.cpp
    color/WbDisplayEstimate.cpp
)

rawrcam_module(imaging
  DIRECTORY imaging
  DEPENDS color metadata support
  SOURCES
    imaging/FramePairer.cpp
    imaging/RawFrameIngress.cpp
    imaging/FrameIngressQueue.cpp
    imaging/Raw16CpuSnapshot.cpp
    imaging/RawPixelSource.cpp
)

rawrcam_module(vulkan
  DIRECTORY vulkan
  DEPENDS
  SOURCES
    vulkan/VulkanContext.cpp
    vulkan/VulkanDispatch.cpp
    vulkan/RawAhbImporter.cpp
    vulkan/Synchronization.cpp
    vulkan/CommandResources.cpp
)

rawrcam_module(tonemap
  DIRECTORY tonemap
  DEPENDS
  SOURCES
    tonemap/ColorRenderProfile.cpp
    tonemap/ImportedLutProfileStore.cpp
)

rawrcam_module(diagnostics_logging
  DIRECTORY diagnostics/logging
  DEPENDS color metadata
  SOURCES
    diagnostics/logging/DiagnosticSink.cpp
    diagnostics/logging/RuntimeTraceRecorder.cpp
    diagnostics/logging/FrameAuditWriter.cpp
)

rawrcam_module(diagnostics_timing
  DIRECTORY diagnostics/timing
  DEPENDS imaging
  SOURCES
    diagnostics/timing/GpuTimingTracker.cpp
)

rawrcam_module(diagnostics_probes
  DIRECTORY diagnostics/probes
  DEPENDS imaging vulkan
  SOURCES
    diagnostics/probes/RawCpuCopyProbe.cpp
    diagnostics/probes/RawIntegrityProbe.cpp
    diagnostics/probes/Raw16SourceParityProbe.cpp
    diagnostics/probes/PipelineDiagnostics.cpp
    diagnostics/probes/ZeroCopyImportProbe.cpp
)

rawrcam_module(develop
  DIRECTORY develop
  DEPENDS color geometry imaging metadata tonemap vulkan
  SOURCES
    develop/SharedHighlightRuntime.cpp
    develop/HotPixelConceal.cpp
    develop/DevelopContextBuilder.cpp
    develop/demosaic/AmazeStillProcessor.cpp
    develop/demosaic/RcdStillProcessor.cpp
    develop/demosaic/VngStillProcessor.cpp
    develop/demosaic/DualStillProcessor.cpp
    develop/render/StillImageRenderer.cpp
    develop/render/RenderResources.cpp
    develop/render/DcpRenderStage.cpp
    develop/render/RenderCommandSession.cpp
    develop/render/StillRenderSequence.cpp
    develop/render/FilmRenderStage.cpp
    develop/render/RenderReadback.cpp
)

rawrcam_module(encoding
  DIRECTORY encoding
  DEPENDS color develop diagnostics_logging geometry imaging metadata support tonemap
  SOURCES
    encoding/jpeg/JpegCaptureWriter.cpp
    encoding/jpeg/JpegExif.cpp
    encoding/jpeg/StripJpegEncoder.cpp
    encoding/rzsl/RzslBundleSink.cpp
)

rawrcam_module(camera
  DIRECTORY camera
  DEPENDS color diagnostics_logging geometry metadata
  SOURCES
    camera/CameraControlSurface.cpp
    camera/NativeCameraController.cpp
    camera/CameraDeviceSession.cpp
    camera/CameraCallbacks.cpp
    camera/CameraRequestPipeline.cpp
    camera/CameraResultProcessor.cpp
    camera/CameraMetadataTags.cpp
    camera/VendorCameraGate.cpp
    camera/CameraRouting.cpp
    camera/CameraProfileJson.cpp
    camera/CameraProbe.cpp
    camera/CameraKeyInjection.cpp
    camera/CameraDiscovery.cpp
    camera/CameraVideoFacts.cpp
    camera/CameraVideoCapabilities.cpp
    camera/CameraControlCapabilities.cpp
    camera/CameraRequestControls.cpp
    camera/CameraFocusControls.cpp
    camera/CameraMeteringRegions.cpp
    camera/CameraControlResult.cpp
    camera/CameraResultReader.cpp
    camera/CameraResultState.cpp
    camera/WhiteBalanceMath.cpp
    camera/CameraWhiteBalanceControls.cpp
    camera/CameraControlSerialization.cpp
)

rawrcam_module(monitoring
  DIRECTORY monitoring
  DEPENDS imaging vulkan
  SOURCES
    monitoring/MonitoringOverlayProcessor.cpp
    monitoring/ImageScopesProcessor.cpp
    monitoring/MonitoringCoordinator.cpp
)

rawrcam_module(presentation
  DIRECTORY presentation
  DEPENDS geometry imaging monitoring vulkan
  SOURCES
    presentation/PresentationSurface.cpp
    presentation/PresentRecorder.cpp
    presentation/SwapchainRenderer.cpp
)

rawrcam_module(video
  DIRECTORY video
  DEPENDS color develop diagnostics_logging diagnostics_timing imaging vulkan
  SOURCES
    video/VideoOutput.cpp
    video/VideoProcessingResources.cpp
    video/VideoRecorder.cpp
    video/VideoSession.cpp
    video/AudioInput.cpp
    video/MovMuxer.cpp
    video/NativeAvRecorder.cpp
    video/Mp4MetadataPatcher.cpp
    video/HevcLogMetadata.cpp
)

rawrcam_module(renderer
  DIRECTORY renderer
  DEPENDS color develop imaging support vulkan
  SOURCES
    renderer/LinearResample.cpp
    renderer/DngSource.cpp
    renderer/SourceCaptureExif.cpp
    renderer/SceneKeyMeter.cpp
    renderer/RendererEngine.cpp
)

rawrcam_module(capture
  DIRECTORY capture
  DEPENDS color develop diagnostics_logging diagnostics_probes encoding geometry imaging metadata support tonemap vulkan
  SOURCES
    capture/single/SingleShotSlot.cpp
    capture/persistence/CaptureJob.cpp
    capture/persistence/BurstJob.cpp
    capture/single/SingleFrameCoordinator.cpp
    capture/single/SingleFrameCaptureJob.cpp
    capture/single/SingleFrameCaptureAcquisition.cpp
    capture/single/SingleFrameCaptureJournal.cpp
    capture/single/SingleFrameCaptureDevelop.cpp
    capture/single/SingleFrameCaptureOutputs.cpp
    capture/single/SingleFrameCaptureContextBuilder.cpp
    capture/multiframe/MultiframeDescription.cpp
    capture/multiframe/mfsr/MfsrCaptureService.cpp
    capture/multiframe/mfsr/MfsrCaptureJob.cpp
    capture/multiframe/mfsr/MergeProcessor.cpp
    capture/multiframe/RzslCaptureWriter.cpp
    capture/multiframe/MultiframeFrameRing.cpp
    capture/multiframe/MultiframeCaptureCoordinator.cpp
)

rawrcam_module(pipeline
  DIRECTORY pipeline
  DEPENDS color develop diagnostics_logging diagnostics_probes diagnostics_timing geometry imaging metadata monitoring presentation tonemap video vulkan
  SOURCES
    pipeline/FrameSubmitCoordinator.cpp
    pipeline/RawCpuUploadPool.cpp
    pipeline/RealtimePipeline.cpp
    pipeline/FrameSlotPool.cpp
    pipeline/RawDevelopRecorder.cpp
    pipeline/MonitorRecorder.cpp
    pipeline/PreviewLookController.cpp
)

rawrcam_module(diagnostics_replay
  DIRECTORY diagnostics/replay
  DEPENDS develop encoding tonemap vulkan
  SOURCES
    diagnostics/replay/HighlightReplayRunner.cpp
)

rawrcam_module(session
  DIRECTORY session
  DEPENDS encoding camera capture color diagnostics_logging diagnostics_probes diagnostics_replay diagnostics_timing geometry imaging metadata monitoring pipeline presentation video vulkan
  SOURCES
    session/SessionEngine.cpp
    session/SessionExposureControls.cpp
    session/SessionWhiteBalanceControls.cpp
    session/SessionToneControls.cpp
    session/SessionStillCapture.cpp
    session/SessionMonitoringControls.cpp
    session/SessionHighlightReplay.cpp
    session/SessionFrameCallbacks.cpp
    session/SessionDiagnostics.cpp
    session/SessionSurfaces.cpp
)
