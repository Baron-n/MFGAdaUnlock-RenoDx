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
add_library(mfgunlock_xemfg SHARED backend.cpp
  vendor/host_unlock.cpp vendor/runtime_payload.cpp vendor/sdk_pacing.cpp vendor/cuda/cuda_route.cpp
  vendor/third_party/minhook/src/buffer.c vendor/third_party/minhook/src/hook.c
  vendor/third_party/minhook/src/trampoline.c vendor/third_party/minhook/src/hde/hde64.c
  "${XEMFG_GENERATED}/embedded_xess_programs.rc"
  "${XEMFG_INPUTS}/embedded_xess_shaders.rc"
  "${XEMFG_INPUTS}/embedded_xefg_cuda.rc" "${CMAKE_CURRENT_BINARY_DIR}/notices.rc")
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
    --sdk-fg "${XEMFG_OFFLINE_FG}" --sdk-ll "${XEMFG_OFFLINE_LL}" --host "${XEMFG_OFFLINE_HOST}")
  set_tests_properties(xemfg_loader_startup xemfg_early_sdk_lifecycle xemfg_host_redirect_lifecycle xemfg_optiscaler_present xemfg_native_context_publication_guard PROPERTIES TIMEOUT 45)
endif()
