cmake_minimum_required(VERSION 3.20)
project(depth_recorder LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Portable static MSVC runtime (/MT) so no VC++ Redistributable is required
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

add_compile_definitions(WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)

include(FetchContent)
FetchContent_Declare(
    minhook
    GIT_REPOSITORY https://github.com/TsudaKageyu/minhook.git
    GIT_TAG        v1.3.3
)
FetchContent_GetProperties(minhook)
if(NOT minhook_POPULATED)
    FetchContent_Populate(minhook)
endif()

add_library(minhook STATIC
    "${minhook_SOURCE_DIR}/src/buffer.c"
    "${minhook_SOURCE_DIR}/src/hook.c"
    "${minhook_SOURCE_DIR}/src/trampoline.c"
    "${minhook_SOURCE_DIR}/src/hde/hde64.c"
)
target_include_directories(minhook PUBLIC "${minhook_SOURCE_DIR}/include")

# Locate fxc.exe from Windows SDK for build-time HLSL -> C++ header compilation
find_program(FXC_EXE fxc
    PATHS
        "C:/Program Files (x86)/Windows Kits/10/bin/10.0.26100.0/x64"
        "C:/Program Files (x86)/Windows Kits/10/bin/10.0.22621.0/x64"
        "C:/Program Files (x86)/Windows Kits/10/bin/x64"
    REQUIRED
)

set(GENERATED_SHADER_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated_shaders")
file(MAKE_DIRECTORY "${GENERATED_SHADER_DIR}")

function(compile_hlsl_header HLSL_FILE ENTRY TARGET_PROFILE VAR_NAME OUT_HEADER)
    add_custom_command(
        OUTPUT "${OUT_HEADER}"
        COMMAND "${FXC_EXE}" /nologo /T ${TARGET_PROFILE} /E ${ENTRY} /Vn ${VAR_NAME} /Fh "${OUT_HEADER}" "${HLSL_FILE}"
        DEPENDS "${HLSL_FILE}"
        COMMENT "Compiling HLSL ${ENTRY} (${TARGET_PROFILE}) -> ${OUT_HEADER}"
        VERBATIM
    )
endfunction()

compile_hlsl_header("${CMAKE_CURRENT_SOURCE_DIR}/src/hook/shaders/depth_convert.hlsl" "CSMain" "cs_5_0" "g_depth_convert_cs" "${GENERATED_SHADER_DIR}/depth_convert_cs.h")
compile_hlsl_header("${CMAKE_CURRENT_SOURCE_DIR}/src/hook/shaders/fps_overlay.hlsl"   "VSMain" "vs_5_0" "g_fps_overlay_vs"   "${GENERATED_SHADER_DIR}/fps_overlay_vs.h")
compile_hlsl_header("${CMAKE_CURRENT_SOURCE_DIR}/src/hook/shaders/fps_overlay.hlsl"   "PSMain" "ps_5_0" "g_fps_overlay_ps"   "${GENERATED_SHADER_DIR}/fps_overlay_ps.h")

find_program(GLSLANG_VALIDATOR glslangValidator
    PATHS
        "$ENV{VULKAN_SDK}/Bin"
        "C:/VulkanSDK/1.4.350.0/Bin"
    REQUIRED
)
find_path(VULKAN_INCLUDE_DIR vulkan/vulkan.h
    PATHS
        "$ENV{VULKAN_SDK}/Include"
        "C:/VulkanSDK/1.4.350.0/Include"
    REQUIRED
)
find_library(VULKAN_LIB vulkan-1
    PATHS
        "$ENV{VULKAN_SDK}/Lib"
        "C:/VulkanSDK/1.4.350.0/Lib"
    REQUIRED
)

add_custom_command(
    OUTPUT "${GENERATED_SHADER_DIR}/depth_convert_spv.h"
    COMMAND "${GLSLANG_VALIDATOR}" -V --vn g_depth_convert_spv -o "${GENERATED_SHADER_DIR}/depth_convert_spv.h" "${CMAKE_CURRENT_SOURCE_DIR}/src/hook/shaders/depth_convert.comp"
    DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/hook/shaders/depth_convert.comp"
    COMMENT "Compiling GLSL depth_convert.comp -> depth_convert_spv.h"
    VERBATIM
)

add_custom_target(compile_shaders DEPENDS
    "${GENERATED_SHADER_DIR}/depth_convert_cs.h"
    "${GENERATED_SHADER_DIR}/fps_overlay_vs.h"
    "${GENERATED_SHADER_DIR}/fps_overlay_ps.h"
    "${GENERATED_SHADER_DIR}/depth_convert_spv.h"
)

# 1. In-game hook DLL
add_library(depth_hook SHARED
    src/hook/dllmain.cpp
    src/hook/dx11_hook.cpp
    src/hook/dx12_hook.cpp
    src/hook/gl_hook.cpp
)
add_dependencies(depth_hook compile_shaders)
target_include_directories(depth_hook PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
    "${GENERATED_SHADER_DIR}"
    "${minhook_SOURCE_DIR}/include"
)
target_link_libraries(depth_hook PRIVATE minhook d3d11 d3d12 dxgi opengl32)

# 2. Portable Win32 + CLI recorder & launcher
add_executable(depth_recorder WIN32
    src/recorder/main.cpp
    src/recorder/ui_win32.cpp
    src/recorder/encoder.cpp
    src/recorder/injector.cpp
)
target_include_directories(depth_recorder PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_link_libraries(depth_recorder PRIVATE comctl32 comdlg32 shell32 ole32)

# 3. Automated tests & D3D11 synthetic cube app
enable_testing()

add_executable(test_ipc_ffmpeg
    tests/test_ipc_ffmpeg.cpp
    src/recorder/encoder.cpp
)
target_include_directories(test_ipc_ffmpeg PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
add_test(NAME test_ipc_ffmpeg COMMAND test_ipc_ffmpeg)

add_executable(sample_dx11_depth WIN32
    tests/sample_dx11_depth.cpp
)
target_link_libraries(sample_dx11_depth PRIVATE d3d11 dxgi d3dcompiler)

add_executable(sample_gl_depth WIN32
    tests/sample_gl_depth.cpp
)
target_link_libraries(sample_gl_depth PRIVATE opengl32)

add_executable(sample_dx12_depth WIN32
    tests/sample_dx12_depth.cpp
)
target_link_libraries(sample_dx12_depth PRIVATE d3d12 dxgi d3dcompiler)

# 4. Vulkan Implicit Layer & sample app
add_library(VkLayer_depth_recorder SHARED
    src/hook/vk_layer.cpp
)
add_dependencies(VkLayer_depth_recorder compile_shaders)
target_include_directories(VkLayer_depth_recorder PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
    "${GENERATED_SHADER_DIR}"
    "${VULKAN_INCLUDE_DIR}"
)
add_custom_command(TARGET VkLayer_depth_recorder POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_CURRENT_SOURCE_DIR}/src/hook/VkLayer_depth_recorder.json"
        "$<TARGET_FILE_DIR:depth_recorder>/VkLayer_depth_recorder.json"
)

add_executable(sample_vk_depth WIN32
    tests/sample_vk_depth.cpp
)
target_include_directories(sample_vk_depth PRIVATE "${VULKAN_INCLUDE_DIR}")
target_link_libraries(sample_vk_depth PRIVATE "${VULKAN_LIB}")
