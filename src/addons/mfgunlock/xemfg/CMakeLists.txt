enable_language(C)
enable_language(RC)
set(XEMFG_SDK_ROOT "" CACHE PATH "Intel XeSS SDK headers")
set(XEMFG_INPUTS "${CMAKE_SOURCE_DIR}/../build/xemfg-inputs" CACHE PATH "Verified, local XeMFG data resources")
set(XEMFG_GENERATED "${CMAKE_SOURCE_DIR}/../build/xemfg-generated" CACHE PATH "Verified program resource table")
if(NOT EXISTS "${XEMFG_SDK_ROOT}/inc/xess_fg/xefg_swapchain_d3d12.h")
  message(FATAL_ERROR "Set XEMFG_SDK_ROOT to the Intel SDK checkout")
endif()
foreach(required IN ITEMS embedded_xess_programs.generated.h embedded_xess_programs.rc)
  if(NOT EXISTS "${XEMFG_GENERATED}/${required}")
    message(FATAL_ERROR "Run prepare_xemfg_inputs.py and vendor/generate_xess_programs.py first")
  endif()
endforeach()
configure_file(notices.rc.in "${CMAKE_CURRENT_BINARY_DIR}/notices.rc" @ONLY)
configure_file(native_notices.rc.in "${CMAKE_CURRENT_BINARY_DIR}/native_notices.rc" @ONLY)
set(XEMFG_MODEL_SOURCES backend.cpp
  vendor/host_unlock.cpp vendor/runtime_payload.cpp vendor/sdk_pacing.cpp vendor/cuda/cuda_route.cpp
  vendor/third_party/minhook/src/buffer.c vendor/third_party/minhook/src/hook.c
  vendor/third_party/minhook/src/trampoline.c vendor/third_party/minhook/src/hde/hde64.c
  "${XEMFG_GENERATED}/embedded_xess_programs.rc"
  "${XEMFG_INPUTS}/embedded_xess_shaders.rc"
  "${XEMFG_INPUTS}/embedded_xefg_cuda.rc")
add_library(mfgunlock_xemfg SHARED ${XEMFG_MODEL_SOURCES} "${CMAKE_CURRENT_BINARY_DIR}/notices.rc")
target_compile_definitions(mfgunlock_xemfg PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN UNICODE _UNICODE)
target_compile_options(mfgunlock_xemfg PRIVATE $<$<COMPILE_LANGUAGE:CXX>:/bigobj> /utf-8)
target_compile_options(mfgunlock_xemfg PRIVATE $<$<COMPILE_LANGUAGE:CXX>:/Zi>)
target_link_options(mfgunlock_xemfg PRIVATE /DEBUG:FULL /OPT:REF /OPT:ICF)
target_include_directories(mfgunlock_xemfg PRIVATE vendor
  "${XEMFG_SDK_ROOT}/inc/xess_fg" "${XEMFG_SDK_ROOT}/inc/xell"
  "${XEMFG_INPUTS}" "${XEMFG_GENERATED}")
target_link_libraries(mfgunlock_xemfg PRIVATE d3d12 dxgi bcrypt version psapi user32)
set_target_properties(mfgunlock_xemfg PROPERTIES PREFIX "" SUFFIX ".asi" OUTPUT_NAME "MFGAdaUnlock.XeMFG"
  MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

add_library(mfgunlock_dual_backend SHARED ../addon.cpp)
target_compile_definitions(mfgunlock_dual_backend PRIVATE
  $<TARGET_PROPERTY:mfgunlock_release_1_4_5,COMPILE_DEFINITIONS> MFGUNLOCK_DUAL_BACKEND)
target_compile_options(mfgunlock_dual_backend PRIVATE /bigobj /utf-8)
target_include_directories(mfgunlock_dual_backend SYSTEM PRIVATE
  $<TARGET_PROPERTY:mfgunlock_addon_compile,INCLUDE_DIRECTORIES>)
target_link_libraries(mfgunlock_dual_backend PRIVATE ${MFGUNLOCK_NATIVE_LIBRARIES})
set_target_properties(mfgunlock_dual_backend PROPERTIES PREFIX "" SUFFIX ".addon64" OUTPUT_NAME "renodx-mfgunlock-dual"
  MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

add_library(mfgunlock_native_xemfg SHARED ../addon.cpp ${XEMFG_MODEL_SOURCES}
  native_bridge.cpp native_streamline.cpp native_swapchain.cpp native_latency.cpp native_ui.cpp native_diagnostics.cpp native_overlay.cpp native_panel.cpp native_controls.cpp native_window.cpp "${CMAKE_CURRENT_BINARY_DIR}/native_notices.rc")
target_compile_definitions(mfgunlock_native_xemfg PRIVATE
  $<TARGET_PROPERTY:mfgunlock_release_1_4_5,COMPILE_DEFINITIONS>
  MFGUNLOCK_DUAL_BACKEND MFGUNLOCK_NATIVE_XEMFG UNICODE _UNICODE)
target_compile_options(mfgunlock_native_xemfg PRIVATE /bigobj /utf-8 $<$<COMPILE_LANGUAGE:CXX>:/Zi>)
target_include_directories(mfgunlock_native_xemfg PRIVATE vendor
  "${XEMFG_SDK_ROOT}/inc/xess_fg" "${XEMFG_SDK_ROOT}/inc/xell"
  "${XEMFG_INPUTS}" "${XEMFG_GENERATED}"
  "${RENODX_SOURCE_DIR}/external/reshade/include")
target_include_directories(mfgunlock_native_xemfg SYSTEM PRIVATE
  $<TARGET_PROPERTY:mfgunlock_addon_compile,INCLUDE_DIRECTORIES>)
target_link_libraries(mfgunlock_native_xemfg PRIVATE ${MFGUNLOCK_NATIVE_LIBRARIES} d3d12 dxgi bcrypt psapi)
target_link_options(mfgunlock_native_xemfg PRIVATE /DEBUG:FULL /OPT:REF /OPT:ICF)
set_target_properties(mfgunlock_native_xemfg PROPERTIES PREFIX "" SUFFIX ".addon64"
  OUTPUT_NAME "renodx-mfgunlock-native" MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

add_library(xemfg_streamline_fixture SHARED "${CMAKE_SOURCE_DIR}/xemfg_streamline_fixture.cpp")
target_compile_definitions(xemfg_streamline_fixture PRIVATE SL_INTERPOSER)
target_include_directories(xemfg_streamline_fixture PRIVATE "${RENODX_SOURCE_DIR}/external/Streamline/include")
target_link_options(xemfg_streamline_fixture PRIVATE /OPT:NOICF)
set_target_properties(xemfg_streamline_fixture PROPERTIES OUTPUT_NAME "sl.interposer")
add_executable(xemfg_native_smoke "${CMAKE_SOURCE_DIR}/xemfg_native_smoke.cpp")
target_compile_definitions(xemfg_native_smoke PRIVATE NOMINMAX UNICODE _UNICODE)
target_include_directories(xemfg_native_smoke PRIVATE "${RENODX_SOURCE_DIR}/external/Streamline/include"
  "${RENODX_SOURCE_DIR}/external/reshade"
  "${RENODX_SOURCE_DIR}/external/reshade/include")
target_link_libraries(xemfg_native_smoke PRIVATE d3d12 dxgi user32)
add_executable(xemfg_window_tests "${CMAKE_SOURCE_DIR}/xemfg_window_tests.cpp" native_window.cpp)
target_compile_definitions(xemfg_window_tests PRIVATE NOMINMAX UNICODE _UNICODE)
target_link_libraries(xemfg_window_tests PRIVATE user32)
add_test(NAME xemfg_window_controls COMMAND xemfg_window_tests)
add_executable(xemfg_overlay_shortcut_tests "${CMAKE_SOURCE_DIR}/xemfg_overlay_shortcut_tests.cpp")
target_compile_definitions(xemfg_overlay_shortcut_tests PRIVATE NOMINMAX)
add_test(NAME xemfg_overlay_shortcuts COMMAND xemfg_overlay_shortcut_tests)
set_tests_properties(xemfg_window_controls PROPERTIES TIMEOUT 20)
add_executable(xemfg_latency_tests "${CMAKE_SOURCE_DIR}/xemfg_latency_tests.cpp" native_latency.cpp)
target_compile_definitions(xemfg_latency_tests PRIVATE NOMINMAX UNICODE _UNICODE)
target_include_directories(xemfg_latency_tests PRIVATE "${XEMFG_SDK_ROOT}/inc/xess_fg" "${XEMFG_SDK_ROOT}/inc/xell"
  "${RENODX_SOURCE_DIR}/external/Streamline/include")
target_link_libraries(xemfg_latency_tests PRIVATE d3d12 dxgi)
add_test(NAME xemfg_latency_translation COMMAND xemfg_latency_tests)
set_tests_properties(xemfg_latency_translation PROPERTIES TIMEOUT 20)
add_executable(xemfg_ui_tests "${CMAKE_SOURCE_DIR}/xemfg_ui_tests.cpp" native_ui.cpp)
target_compile_definitions(xemfg_ui_tests PRIVATE NOMINMAX UNICODE _UNICODE)
target_include_directories(xemfg_ui_tests PRIVATE "${XEMFG_SDK_ROOT}/inc/xess_fg" "${XEMFG_SDK_ROOT}/inc/xell"
  "${RENODX_SOURCE_DIR}/external/Streamline/include")
target_link_libraries(xemfg_ui_tests PRIVATE d3d12 dxgi)
add_test(NAME xemfg_ui_composition COMMAND xemfg_ui_tests)
add_executable(xemfg_cadence_tests "${CMAKE_SOURCE_DIR}/xemfg_cadence_tests.cpp")
add_test(NAME xemfg_cadence COMMAND xemfg_cadence_tests)
add_executable(xemfg_hud_input_tests "${CMAKE_SOURCE_DIR}/xemfg_hud_input_tests.cpp")
target_compile_definitions(xemfg_hud_input_tests PRIVATE NOMINMAX)
add_test(NAME xemfg_hud_inputs COMMAND xemfg_hud_input_tests)
add_executable(xemfg_frame_store_tests "${CMAKE_SOURCE_DIR}/xemfg_frame_store_tests.cpp")
target_compile_definitions(xemfg_frame_store_tests PRIVATE NOMINMAX UNICODE _UNICODE)
target_include_directories(xemfg_frame_store_tests PRIVATE "${XEMFG_SDK_ROOT}/inc/xess_fg" "${XEMFG_SDK_ROOT}/inc/xell"
  "${RENODX_SOURCE_DIR}/external/Streamline/include")
add_test(NAME xemfg_frame_store COMMAND xemfg_frame_store_tests)
add_executable(xemfg_async_log_tests "${CMAKE_SOURCE_DIR}/xemfg_async_log_tests.cpp")
add_test(NAME xemfg_async_log COMMAND xemfg_async_log_tests)
add_executable(xemfg_diagnostics_tests "${CMAKE_SOURCE_DIR}/xemfg_diagnostics_tests.cpp" native_diagnostics.cpp)
add_test(NAME xemfg_cpu_diagnostics COMMAND xemfg_diagnostics_tests)
add_executable(xemfg_panel_preview "${CMAKE_SOURCE_DIR}/xemfg_panel_preview.cpp" native_panel.cpp native_controls.cpp native_diagnostics.cpp
  "${RENODX_SOURCE_DIR}/external/reshade/deps/imgui/imgui.cpp"
  "${RENODX_SOURCE_DIR}/external/reshade/deps/imgui/imgui_draw.cpp"
  "${RENODX_SOURCE_DIR}/external/reshade/deps/imgui/imgui_tables.cpp"
  "${RENODX_SOURCE_DIR}/external/reshade/deps/imgui/imgui_widgets.cpp")
target_compile_definitions(xemfg_panel_preview PRIVATE NOMINMAX UNICODE _UNICODE MFGUNLOCK_NATIVE_XEMFG MFGUNLOCK_UI_PREVIEW)
target_include_directories(xemfg_panel_preview PRIVATE "${XEMFG_SDK_ROOT}/inc/xess_fg" "${XEMFG_SDK_ROOT}/inc/xell"
  "${RENODX_SOURCE_DIR}/external/Streamline/include" "${RENODX_SOURCE_DIR}/external/reshade")
target_link_libraries(xemfg_panel_preview PRIVATE d3d12 dxgi user32 ole32 windowscodecs)
foreach(page RANGE 0 3)
  add_test(NAME xemfg_panel_page_${page} COMMAND xemfg_panel_preview "${CMAKE_BINARY_DIR}/ui-preview/page-${page}.png" ${page} 800)
  set_tests_properties(xemfg_panel_page_${page} PROPERTIES RUN_SERIAL TRUE)
endforeach()
add_test(NAME xemfg_panel_large_font COMMAND xemfg_panel_preview "${CMAKE_BINARY_DIR}/ui-preview/large-font.png" 0 460 22)
set_tests_properties(xemfg_panel_large_font PROPERTIES RUN_SERIAL TRUE)
add_test(NAME xemfg_panel_keyboard_navigation COMMAND xemfg_panel_preview "${CMAKE_BINARY_DIR}/ui-preview/navigation.png" 0 930 20 navigation)
add_test(NAME xemfg_panel_keyboard_navigation_compact COMMAND xemfg_panel_preview "${CMAKE_BINARY_DIR}/ui-preview/navigation-compact.png" 0 460 22 navigation)
set_tests_properties(xemfg_panel_keyboard_navigation xemfg_panel_keyboard_navigation_compact PROPERTIES RUN_SERIAL TRUE)
foreach(scenario IN ITEMS expanded performance pacing tooltip multiplier)
  set(page 0)
  if(scenario STREQUAL "expanded")
    set(page 2)
  elseif(scenario STREQUAL "performance")
    set(page 3)
  endif()
  add_test(NAME xemfg_panel_${scenario} COMMAND xemfg_panel_preview "${CMAKE_BINARY_DIR}/ui-preview/${scenario}.png" ${page} 800 16 ${scenario})
  set_tests_properties(xemfg_panel_${scenario} PROPERTIES RUN_SERIAL TRUE)
endforeach()
set(XEMFG_OFFLINE_RESHADE "" CACHE FILEPATH "Real ReShade addon-enabled DXGI loader for native bridge test")
if(XEMFG_OFFLINE_RESHADE AND XEMFG_OFFLINE_FG AND XEMFG_OFFLINE_LL)
  find_package(Python3 COMPONENTS Interpreter REQUIRED)
  foreach(layout IN ITEMS storage-only preloaded-storage mutated-storage missing)
    add_test(NAME xemfg_native_runtime_${layout} COMMAND "${Python3_EXECUTABLE}"
      "${CMAKE_SOURCE_DIR}/xemfg_native_tests.py" --directory "${CMAKE_BINARY_DIR}/native-checks"
      --addon $<TARGET_FILE:mfgunlock_native_xemfg> --exe $<TARGET_FILE:xemfg_native_smoke>
      --fixture $<TARGET_FILE:xemfg_streamline_fixture> --reshade "${XEMFG_OFFLINE_RESHADE}"
      --sdk-fg "${XEMFG_OFFLINE_FG}" --sdk-ll "${XEMFG_OFFLINE_LL}" --runtime-layout ${layout})
    set_tests_properties(xemfg_native_runtime_${layout} PROPERTIES RUN_SERIAL TRUE TIMEOUT 150)
  endforeach()
  foreach(multiplier IN ITEMS 2 3 4 6)
  add_test(NAME xemfg_native_reshade_${multiplier}x COMMAND "${Python3_EXECUTABLE}"
    "${CMAKE_SOURCE_DIR}/xemfg_native_tests.py" --directory "${CMAKE_BINARY_DIR}/native-checks"
    --addon $<TARGET_FILE:mfgunlock_native_xemfg> --exe $<TARGET_FILE:xemfg_native_smoke>
    --fixture $<TARGET_FILE:xemfg_streamline_fixture> --reshade "${XEMFG_OFFLINE_RESHADE}"
    --sdk-fg "${XEMFG_OFFLINE_FG}" --sdk-ll "${XEMFG_OFFLINE_LL}" --multiplier ${multiplier} --overlay-keys)
  set_tests_properties(xemfg_native_reshade_${multiplier}x PROPERTIES TIMEOUT 150 RUN_SERIAL TRUE)
  endforeach()
  foreach(multiplier IN ITEMS 2 3 4 6)
    add_test(NAME xemfg_native_reflex_${multiplier}x COMMAND "${Python3_EXECUTABLE}"
      "${CMAKE_SOURCE_DIR}/xemfg_native_tests.py" --directory "${CMAKE_BINARY_DIR}/native-checks"
      --addon $<TARGET_FILE:mfgunlock_native_xemfg> --exe $<TARGET_FILE:xemfg_native_smoke>
      --fixture $<TARGET_FILE:xemfg_streamline_fixture> --reshade "${XEMFG_OFFLINE_RESHADE}"
      --sdk-fg "${XEMFG_OFFLINE_FG}" --sdk-ll "${XEMFG_OFFLINE_LL}" --multiplier ${multiplier} --latency-output Reflex)
    set_tests_properties(xemfg_native_reflex_${multiplier}x PROPERTIES TIMEOUT 150 RUN_SERIAL TRUE)
  endforeach()
endif()

add_executable(xemfg_provider_probe "${CMAKE_SOURCE_DIR}/xemfg_provider_probe.cpp"
  vendor/host_unlock.cpp vendor/runtime_payload.cpp
  "${XEMFG_GENERATED}/embedded_xess_programs.rc" "${XEMFG_INPUTS}/embedded_xess_shaders.rc")
target_compile_definitions(xemfg_provider_probe PRIVATE NOMINMAX UNICODE _UNICODE)
target_include_directories(xemfg_provider_probe PRIVATE vendor "${XEMFG_INPUTS}" "${XEMFG_GENERATED}")
target_link_libraries(xemfg_provider_probe PRIVATE bcrypt)
set(XEMFG_OFFLINE_FG "" CACHE FILEPATH "Explicit local XeSS-FG provider to probe")
set(XEMFG_OFFLINE_LL "" CACHE FILEPATH "Explicit local XeLL provider to probe")
if(XEMFG_OFFLINE_FG AND XEMFG_OFFLINE_LL)
  add_test(NAME xemfg_provider_patch_restore COMMAND xemfg_provider_probe "${XEMFG_OFFLINE_FG}" "${XEMFG_OFFLINE_LL}")
endif()
add_library(xemfg_host_fixture SHARED "${CMAKE_SOURCE_DIR}/xemfg_host_fixture.cpp" "${CMAKE_SOURCE_DIR}/xemfg_host_fixture.rc"
  vendor/third_party/minhook/src/buffer.c vendor/third_party/minhook/src/hook.c
  vendor/third_party/minhook/src/trampoline.c vendor/third_party/minhook/src/hde/hde64.c)
target_compile_definitions(xemfg_host_fixture PRIVATE UNICODE _UNICODE NOMINMAX)
target_include_directories(xemfg_host_fixture PRIVATE "${XEMFG_SDK_ROOT}/inc/xess_fg" "${XEMFG_SDK_ROOT}/inc/xell")
target_link_libraries(xemfg_host_fixture PRIVATE d3d12 dxgi user32)
add_executable(xemfg_runtime_smoke "${CMAKE_SOURCE_DIR}/xemfg_runtime_smoke.cpp")
add_executable(xemfg_loader_smoke "${CMAKE_SOURCE_DIR}/xemfg_loader_smoke.cpp")
target_compile_definitions(xemfg_loader_smoke PRIVATE NOMINMAX UNICODE _UNICODE)
target_include_directories(xemfg_loader_smoke PRIVATE "${XEMFG_SDK_ROOT}/inc/xess_fg")
target_link_libraries(xemfg_loader_smoke PRIVATE "${XEMFG_SDK_ROOT}/lib/libxess_fg.lib")
add_executable(xemfg_optiscaler_present_smoke "${CMAKE_SOURCE_DIR}/xemfg_optiscaler_present_smoke.cpp")
target_compile_definitions(xemfg_optiscaler_present_smoke PRIVATE NOMINMAX UNICODE _UNICODE)
target_include_directories(xemfg_optiscaler_present_smoke PRIVATE "${XEMFG_SDK_ROOT}/inc/xess_fg")
target_link_libraries(xemfg_optiscaler_present_smoke PRIVATE "${XEMFG_SDK_ROOT}/lib/libxess_fg.lib" d3d12 dxgi user32)
add_library(xemfg_foreign_fixture SHARED "${CMAKE_SOURCE_DIR}/xemfg_foreign_fixture.cpp")
target_include_directories(xemfg_foreign_fixture PRIVATE "${XEMFG_SDK_ROOT}/inc/xess_fg")
target_link_libraries(xemfg_foreign_fixture PRIVATE "${XEMFG_SDK_ROOT}/lib/libxess_fg.lib")
set(XEMFG_OFFLINE_HOST "" CACHE FILEPATH "Explicit OptiScaler 10 loader for the cold-start regression test")
if(XEMFG_OFFLINE_HOST AND XEMFG_OFFLINE_FG AND XEMFG_OFFLINE_LL)
  find_package(Python3 COMPONENTS Interpreter REQUIRED)
  add_test(NAME xemfg_loader_startup COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/xemfg_startup_tests.py"
    --mode loader --directory "${CMAKE_BINARY_DIR}/xemfg-checks"
    --plugin $<TARGET_FILE:mfgunlock_xemfg> --executable $<TARGET_FILE:xemfg_loader_smoke>
    --sdk-fg "${XEMFG_OFFLINE_FG}" --sdk-ll "${XEMFG_OFFLINE_LL}" --host "${XEMFG_OFFLINE_HOST}")
  add_test(NAME xemfg_early_sdk_lifecycle COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/xemfg_startup_tests.py"
    --mode early-lifecycle --directory "${CMAKE_BINARY_DIR}/xemfg-checks"
    --plugin $<TARGET_FILE:mfgunlock_xemfg> --executable $<TARGET_FILE:xemfg_runtime_smoke>
    --sdk-fg "${XEMFG_OFFLINE_FG}" --sdk-ll "${XEMFG_OFFLINE_LL}" --host $<TARGET_FILE:xemfg_host_fixture>)
  add_test(NAME xemfg_host_redirect_lifecycle COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/xemfg_startup_tests.py"
    --mode redirect-lifecycle --directory "${CMAKE_BINARY_DIR}/xemfg-checks"
    --plugin $<TARGET_FILE:mfgunlock_xemfg> --executable $<TARGET_FILE:xemfg_runtime_smoke>
    --sdk-fg "${XEMFG_OFFLINE_FG}" --sdk-ll "${XEMFG_OFFLINE_LL}" --host $<TARGET_FILE:xemfg_host_fixture>)
  add_test(NAME xemfg_optiscaler_present COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/xemfg_startup_tests.py"
    --mode optiscaler-present --directory "${CMAKE_BINARY_DIR}/xemfg-checks"
    --plugin $<TARGET_FILE:mfgunlock_xemfg> --executable $<TARGET_FILE:xemfg_optiscaler_present_smoke>
    --sdk-fg "${XEMFG_OFFLINE_FG}" --sdk-ll "${XEMFG_OFFLINE_LL}" --host "${XEMFG_OFFLINE_HOST}")
  add_test(NAME xemfg_native_context_publication_guard COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/xemfg_startup_tests.py"
    --mode optiscaler-live-native --directory "${CMAKE_BINARY_DIR}/xemfg-checks"
    --plugin $<TARGET_FILE:mfgunlock_xemfg> --executable $<TARGET_FILE:xemfg_optiscaler_present_smoke>
    --sdk-fg "${XEMFG_OFFLINE_FG}" --sdk-ll "${XEMFG_OFFLINE_LL}" --host "${XEMFG_OFFLINE_HOST}"
    --foreign-fixture $<TARGET_FILE:xemfg_foreign_fixture>)
  add_test(NAME xemfg_game_first_shared_model COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/xemfg_startup_tests.py"
    --mode optiscaler-game-first --directory "${CMAKE_BINARY_DIR}/xemfg-checks"
    --plugin $<TARGET_FILE:mfgunlock_xemfg> --executable $<TARGET_FILE:xemfg_optiscaler_present_smoke>
    --sdk-fg "${XEMFG_OFFLINE_FG}" --sdk-ll "${XEMFG_OFFLINE_LL}" --host "${XEMFG_OFFLINE_HOST}")
  set_tests_properties(xemfg_loader_startup xemfg_early_sdk_lifecycle xemfg_host_redirect_lifecycle xemfg_optiscaler_present xemfg_native_context_publication_guard xemfg_game_first_shared_model PROPERTIES TIMEOUT 45)
endif()

add_executable(xemfg_reshade_panel_input "${CMAKE_SOURCE_DIR}/xemfg_reshade_panel_input.cpp")
target_compile_definitions(xemfg_reshade_panel_input PRIVATE NOMINMAX UNICODE _UNICODE)
target_include_directories(xemfg_reshade_panel_input PRIVATE "${RENODX_SOURCE_DIR}/external/reshade" "${RENODX_SOURCE_DIR}/external/reshade/include")
target_link_libraries(xemfg_reshade_panel_input PRIVATE d3d12 dxgi user32)

add_subdirectory("${CMAKE_SOURCE_DIR}/xemfg_cuda" "${CMAKE_BINARY_DIR}/xemfg-cuda")
