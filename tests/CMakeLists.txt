cmake_minimum_required(VERSION 3.24)
project(rawr_host_tests LANGUAGES C CXX)
include(CTest)
find_package(Vulkan REQUIRED)
find_package(Threads REQUIRED)
find_program(UV_EXECUTABLE uv REQUIRED)
get_filename_component(RAWR_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/.." ABSOLUTE)
set(APP "${RAWR_ROOT}/app/src/main/cpp")
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
option(RAWR_SANITIZERS "Enable address and undefined behavior sanitizers" OFF)
if(RAWR_SANITIZERS)
  add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer -g)
  add_link_options(-fsanitize=address,undefined)
endif()
file(GLOB native_headers "${RAWR_ROOT}/native/*/include" "${RAWR_ROOT}/native/multiframe/*/include")
add_subdirectory("${RAWR_ROOT}/native/tinydng" "${CMAKE_CURRENT_BINARY_DIR}/tinydng")
include("${APP}/cmake/CameraProfiles.cmake")
rawr_embed_camera_profiles("${CMAKE_CURRENT_BINARY_DIR}/generated/camera/BuiltInCameraProfiles.inc")
function(rawr_test name)
  add_executable(${name} ${ARGN})
  target_include_directories(${name} PRIVATE "${APP}" "${RAWR_ROOT}/native/tinydng" "${CMAKE_CURRENT_BINARY_DIR}/generated" ${native_headers} ${Vulkan_INCLUDE_DIRS})
  target_link_libraries(${name} PRIVATE Threads::Threads)
  target_compile_options(${name} PRIVATE -Wall -Wextra -Werror -UNDEBUG)
  if(name STREQUAL "capture_job_test")
    add_test(NAME ${name} COMMAND ${name} "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/capture_job_v1.job")
  else()
    add_test(NAME ${name} COMMAND ${name})
  endif()
  set_tests_properties(${name} PROPERTIES ENVIRONMENT "TMPDIR=${CMAKE_CURRENT_BINARY_DIR}")
endfunction()

rawr_test(gps_metadata_test native/gps_metadata_test.cpp "${APP}/encoding/jpeg/JpegExif.cpp")
target_link_libraries(gps_metadata_test PRIVATE rawr_tinydng)

rawr_test(renderer_bayer_bin_test
  "${RAWR_ROOT}/tests/native/renderer_bayer_bin_test.cpp")
rawr_test(convert_natural_bin_test
  "${RAWR_ROOT}/tests/native/convert_natural_bin_test.cpp")
rawr_test(convert_bayer_resample_test
  "${RAWR_ROOT}/tests/native/convert_bayer_resample_test.cpp")
rawr_test(convert_binned_raw_test
  "${RAWR_ROOT}/tests/native/convert_binned_raw_test.cpp"
  "${APP}/convert/DngConverter.cpp")
target_link_libraries(convert_binned_raw_test PRIVATE rawr_tinydng)
rawr_test(binned_corner_test
  "${RAWR_ROOT}/tests/native/binned_corner_test.cpp")
rawr_test(cfa_remosaic_test
  "${RAWR_ROOT}/tests/native/cfa_remosaic_test.cpp")
rawr_test(renderer_source_exif_test
  "${RAWR_ROOT}/tests/native/renderer_source_exif_test.cpp"
  "${APP}/renderer/SourceCaptureExif.cpp")
target_link_libraries(renderer_source_exif_test PRIVATE rawr_tinydng)
rawr_test(renderer_resample_test
  "${RAWR_ROOT}/tests/native/renderer_resample_test.cpp"
  "${APP}/renderer/LinearResample.cpp")

rawr_test(ae_post_gain_curve_test
  "${RAWR_ROOT}/tests/native/ae_post_gain_curve_test.cpp")
rawr_test(auto_key_exposure_test
  "${RAWR_ROOT}/tests/native/auto_key_exposure_test.cpp")
rawr_test(rawr_auto_exposure_test
  "${RAWR_ROOT}/tests/native/rawr_auto_exposure_test.cpp")
rawr_test(camera_focus_controls_test
  "${RAWR_ROOT}/tests/native/camera_focus_controls_test.cpp"
  "${RAWR_ROOT}/app/src/main/cpp/camera/CameraFocusControls.cpp"
  "${RAWR_ROOT}/app/src/main/cpp/camera/CameraMeteringRegions.cpp"
  "${RAWR_ROOT}/app/src/main/cpp/camera/CameraResultState.cpp"
  "${RAWR_ROOT}/app/src/main/cpp/camera/CameraWhiteBalanceControls.cpp"
  "${RAWR_ROOT}/app/src/main/cpp/camera/WhiteBalanceMath.cpp"
  "${RAWR_ROOT}/app/src/main/cpp/color/WbDisplayEstimate.cpp"
  "${RAWR_ROOT}/app/src/main/cpp/color/ColorMath.cpp")
rawr_test(camera_interaction_policy_test
  "${RAWR_ROOT}/tests/native/camera_interaction_policy_test.cpp"
  "${RAWR_ROOT}/app/src/main/cpp/camera/CameraControlSerialization.cpp"
  "${RAWR_ROOT}/app/src/main/cpp/camera/CameraProfileJson.cpp"
  "${RAWR_ROOT}/app/src/main/cpp/color/DcpProfile.cpp"
  "${RAWR_ROOT}/app/src/main/cpp/color/ColorMath.cpp"
  "${RAWR_ROOT}/app/src/main/cpp/geometry/LensDistortion.cpp"
  "${RAWR_ROOT}/app/src/main/cpp/camera/CameraRouting.cpp")
add_test(NAME camera_interaction_policy_test_snapshot COMMAND "${UV_EXECUTABLE}" run --no-project --cache-dir "${RAWR_ROOT}/tmp/uv-cache" "${CMAKE_CURRENT_SOURCE_DIR}/native/camera_interaction_policy_test_snapshot.py" "$<TARGET_FILE:camera_interaction_policy_test>")
rawr_test(camera_result_state_test
  "${RAWR_ROOT}/tests/native/camera_result_state_test.cpp"
  "${RAWR_ROOT}/app/src/main/cpp/camera/CameraControlResult.cpp"
  "${RAWR_ROOT}/app/src/main/cpp/camera/CameraResultState.cpp"
  "${RAWR_ROOT}/app/src/main/cpp/camera/CameraWhiteBalanceControls.cpp"
  "${RAWR_ROOT}/app/src/main/cpp/camera/WhiteBalanceMath.cpp"
  "${RAWR_ROOT}/app/src/main/cpp/color/WbDisplayEstimate.cpp"
  "${RAWR_ROOT}/app/src/main/cpp/color/ColorMath.cpp")
rawr_test(sensitivity_probe_startup_test
  "${RAWR_ROOT}/tests/native/sensitivity_probe_startup_test.cpp")

rawr_test(camera_session_policy_test
  "${RAWR_ROOT}/tests/native/camera_session_policy_test.cpp")
rawr_test(raw_reader_usage_ladder_test
  "${RAWR_ROOT}/tests/native/raw_reader_usage_ladder_test.cpp")
rawr_test(camera_video_facts_test
  "${RAWR_ROOT}/tests/native/camera_video_facts_test.cpp"
  "${RAWR_ROOT}/app/src/main/cpp/camera/CameraVideoFacts.cpp")
add_test(NAME camera_video_facts_test_snapshot COMMAND "${UV_EXECUTABLE}" run --no-project --cache-dir "${RAWR_ROOT}/tmp/uv-cache" "${CMAKE_CURRENT_SOURCE_DIR}/native/camera_video_facts_test_snapshot.py" "$<TARGET_FILE:camera_video_facts_test>")
rawr_test(camera_white_balance_test
  "${RAWR_ROOT}/tests/native/camera_white_balance_test.cpp"
  "${RAWR_ROOT}/app/src/main/cpp/camera/WhiteBalanceMath.cpp"
  "${RAWR_ROOT}/app/src/main/cpp/camera/CameraWhiteBalanceControls.cpp"
  "${RAWR_ROOT}/app/src/main/cpp/camera/CameraControlSerialization.cpp"
  "${RAWR_ROOT}/app/src/main/cpp/camera/CameraProfileJson.cpp"
  "${RAWR_ROOT}/app/src/main/cpp/color/DcpProfile.cpp"
  "${RAWR_ROOT}/app/src/main/cpp/geometry/LensDistortion.cpp"
  "${RAWR_ROOT}/app/src/main/cpp/camera/CameraRouting.cpp"
  "${RAWR_ROOT}/app/src/main/cpp/color/WbDisplayEstimate.cpp"
  "${RAWR_ROOT}/app/src/main/cpp/color/ColorMath.cpp")
add_test(NAME camera_white_balance_test_snapshot COMMAND "${UV_EXECUTABLE}" run --no-project --cache-dir "${RAWR_ROOT}/tmp/uv-cache" "${CMAKE_CURRENT_SOURCE_DIR}/native/camera_white_balance_test_snapshot.py" "$<TARGET_FILE:camera_white_balance_test>")
rawr_test(capture_job_test
  "${RAWR_ROOT}/tests/native/capture_job_test.cpp"
  "${RAWR_ROOT}/app/src/main/cpp/capture/persistence/CaptureJob.cpp"
  "${RAWR_ROOT}/app/src/main/cpp/color/DcpProfile.cpp"
  "${RAWR_ROOT}/app/src/main/cpp/color/ColorMath.cpp"
  "${RAWR_ROOT}/app/src/main/cpp/geometry/LensDistortion.cpp")

rawr_test(film_exposure_test
  "${RAWR_ROOT}/tests/native/film_exposure_test.cpp")
rawr_test(bracket_exposure_test
  "${RAWR_ROOT}/tests/native/bracket_exposure_test.cpp")
rawr_test(strip_merge_test
  "${RAWR_ROOT}/tests/native/strip_merge_test.cpp")
rawr_test(hdr_headroom_meter_test
  "${RAWR_ROOT}/tests/native/hdr_headroom_meter_test.cpp")
rawr_test(exposure_normalizer_test
  "${RAWR_ROOT}/tests/native/exposure_normalizer_test.cpp")
rawr_test(film_scratch_estimate_test
  "${RAWR_ROOT}/tests/native/film_scratch_estimate_test.cpp")
rawr_test(public_description_test
  "${RAWR_ROOT}/tests/native/public_description_test.cpp"
  "${RAWR_ROOT}/app/src/main/cpp/tonemap/ColorRenderProfile.cpp")
rawr_test(multiframe_exif_test
  "${RAWR_ROOT}/tests/native/multiframe_exif_test.cpp"
  "${RAWR_ROOT}/app/src/main/cpp/capture/multiframe/MultiframeDescription.cpp")
rawr_test(native_ownership_test
  "${RAWR_ROOT}/tests/native/native_ownership_test.cpp"
  "${RAWR_ROOT}/app/src/main/cpp/imaging/FrameIngressQueue.cpp"
  "${RAWR_ROOT}/app/src/main/cpp/imaging/FramePairer.cpp")
target_include_directories(native_ownership_test BEFORE PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/native/ownership_stubs")
rawr_test(raw_still_capture_manager_test
  "${RAWR_ROOT}/tests/native/raw_still_capture_manager_test.cpp"
  "${RAWR_ROOT}/app/src/main/cpp/imaging/Raw16CpuSnapshot.cpp"
  "${RAWR_ROOT}/app/src/main/cpp/imaging/RawPixelSource.cpp"
  "${RAWR_ROOT}/app/src/main/cpp/capture/single/SingleShotSlot.cpp")
target_compile_options(raw_still_capture_manager_test PRIVATE -DNDEBUG)
target_include_directories(raw_still_capture_manager_test BEFORE PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/native/capture_stubs")
rawr_test(exif_transpose_test
  "${RAWR_ROOT}/tests/native/exif_transpose_test.cpp")
rawr_test(ultrahdr_map_fit_test
  "${RAWR_ROOT}/tests/native/ultrahdr_map_fit_test.cpp")
rawr_test(raw_image_ring_queue_test
  "${RAWR_ROOT}/tests/native/raw_image_ring_queue_test.cpp"
  "${RAWR_ROOT}/native/zsl_ring/src/RawImageRing.cpp")
rawr_test(dng_capture_test native/dng_capture_test.cpp
  "${APP}/encoding/dng/DngExif.cpp"
  "${APP}/encoding/dng/DngGainMapOpcode.cpp"
  "${APP}/geometry/LensDistortion.cpp"
  "${APP}/encoding/dng/DngMetadataAdapter.cpp"
  "${APP}/encoding/dng/DngPixelPayload.cpp"
  "${APP}/encoding/dng/DngProvenance.cpp"
  "${APP}/color/DcpProfile.cpp"
  "${APP}/color/ColorMath.cpp")
target_link_libraries(dng_capture_test PRIVATE rawr_tinydng)
add_executable(hevc_metadata_helper native/hevc_log_metadata_test.cpp "${APP}/video/HevcLogMetadata.cpp")
add_executable(mp4_metadata_helper native/mp4_metadata_patcher_test.cpp "${APP}/video/Mp4MetadataPatcher.cpp" "${APP}/video/HevcLogMetadata.cpp")
add_executable(hevc_frame_type_helper native/hevc_frame_type_test.cpp)
add_executable(mov_muxer_helper native/mov_muxer_test.cpp "${APP}/video/MovMuxer.cpp")
target_link_libraries(mov_muxer_helper PRIVATE Threads::Threads)
target_include_directories(mov_muxer_helper PRIVATE "${APP}")
target_compile_options(mov_muxer_helper PRIVATE -Wall -Wextra -Werror)
target_include_directories(hevc_metadata_helper PRIVATE "${APP}")
target_include_directories(hevc_frame_type_helper PRIVATE "${APP}")
target_include_directories(mp4_metadata_helper PRIVATE "${APP}")
find_program(FFMPEG ffmpeg REQUIRED)
find_program(FFPROBE ffprobe REQUIRED)
add_test(NAME hevc_frame_types COMMAND "${UV_EXECUTABLE}" run --no-project --cache-dir "${RAWR_ROOT}/tmp/uv-cache" "${CMAKE_CURRENT_SOURCE_DIR}/hevc_frame_types.py" "$<TARGET_FILE:hevc_frame_type_helper>" "${FFMPEG}" "${FFPROBE}" "${CMAKE_CURRENT_BINARY_DIR}")
add_test(NAME mov_muxer COMMAND "${UV_EXECUTABLE}" run --no-project --cache-dir "${RAWR_ROOT}/tmp/uv-cache" "${CMAKE_CURRENT_SOURCE_DIR}/mov_muxer.py" "$<TARGET_FILE:mov_muxer_helper>" "$<TARGET_FILE:mp4_metadata_helper>" "${FFMPEG}" "${FFPROBE}" "${CMAKE_CURRENT_BINARY_DIR}")
add_test(NAME video_containers COMMAND "${UV_EXECUTABLE}" run --no-project --cache-dir "${RAWR_ROOT}/tmp/uv-cache" "${CMAKE_CURRENT_SOURCE_DIR}/video_containers.py" "$<TARGET_FILE:hevc_metadata_helper>" "$<TARGET_FILE:mp4_metadata_helper>" "${FFMPEG}" "${FFPROBE}" "${CMAKE_CURRENT_BINARY_DIR}")

rawr_test(raw_pixel_source_test
  "${RAWR_ROOT}/tests/native/raw_pixel_source_test.cpp"
  "${APP}/imaging/RawPixelSource.cpp")
rawr_test(full_size_sequence_test
  "${RAWR_ROOT}/tests/native/full_size_sequence_test.cpp")
rawr_test(dcp_profile_test
  "${RAWR_ROOT}/tests/native/dcp_profile_test.cpp"
  "${APP}/color/DcpProfile.cpp"
  "${APP}/color/ColorMath.cpp")
rawr_test(camera_profile_test
  "${RAWR_ROOT}/tests/native/camera_profile_test.cpp"
  "${APP}/camera/CameraRouting.cpp"
  "${APP}/camera/CameraProfileJson.cpp"
  "${APP}/color/DcpProfile.cpp"
  "${APP}/color/ColorMath.cpp"
  "${APP}/geometry/LensDistortion.cpp")
# GPU RAW10 ingress vs the CPU fallback unpack (needs a Vulkan device).
add_subdirectory("${RAWR_ROOT}/native/raw_ingress" "${CMAKE_CURRENT_BINARY_DIR}/raw_ingress")
rawr_test(raw10_unpack_vulkan_test
  "${RAWR_ROOT}/tests/native/raw10_unpack_vulkan_test.cpp"
  "${APP}/imaging/RawPixelSource.cpp")
target_include_directories(raw10_unpack_vulkan_test PRIVATE "${RAWR_ROOT}/native/vk_common/testing")
target_link_libraries(raw10_unpack_vulkan_test PRIVATE rawr::raw_ingress)
# GPU block-CFA remosaic vs the CPU reference.
rawr_test(raw_remosaic_vulkan_test
  "${RAWR_ROOT}/tests/native/raw_remosaic_vulkan_test.cpp"
  "${APP}/imaging/RawPixelSource.cpp")
target_include_directories(raw_remosaic_vulkan_test PRIVATE "${RAWR_ROOT}/native/vk_common/testing")
target_link_libraries(raw_remosaic_vulkan_test PRIVATE rawr::raw_ingress)

# The production DCP shader, exercised on a host Vulkan device.
find_program(GLSLC glslc REQUIRED)
include("${RAWR_ROOT}/native/vk_common/cmake/EmbedSpirv.cmake")
embed_spirv(dcp "${APP}/develop/render/shaders/dcp.comp" dcp_spv
  OUTPUT_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated")
rawr_test(dcp_render_test native/dcp_render_test.cpp
  "${APP}/develop/render/DcpRenderStage.cpp"
  "${APP}/color/DcpRenderData.cpp" "${APP}/color/ColorMath.cpp" ${dcp_HDR})
target_include_directories(dcp_render_test PRIVATE "${RAWR_ROOT}/native/vk_common/testing")
target_link_libraries(dcp_render_test PRIVATE Vulkan::Vulkan)

rawr_test(raw_reader_drain_test
  "${RAWR_ROOT}/tests/native/raw_reader_drain_test.cpp"
  "${APP}/imaging/RawFrameIngress.cpp")
target_include_directories(raw_reader_drain_test BEFORE PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/native/reader_stubs")
