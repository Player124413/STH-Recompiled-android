# Shadow the Hedgehog (original Xbox), statically recompiled, as an Android library: libmain.so, loaded by SDL's
# activity. The x86 code is recompiled on a PC first (tools/xbe_recomp or the Windows build): this takes
# generated/game as it is, with the SDK's portable runtime, the game's sources in src and the Android host in native.

cmake_minimum_required(VERSION 3.22)
project(shadow_android LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

get_filename_component(REPO ${CMAKE_CURRENT_SOURCE_DIR}/.. ABSOLUTE)
set(NATIVE ${REPO}/native)
set(GAME ${REPO}/src)     # the game's hooks and the PC parts of the port that are plain C++ (render_ext, pc_text)
set(SDK ${REPO}/sdk)      # this repository carries the SDK it is built with

# the recompiled code and the xbe it came from
set(XBE_GENERATED ${REPO}/generated/game CACHE PATH "the recompiled code of default.xbe")
set(XBE_IMAGE ${REPO}/assets/default.xbe CACHE FILEPATH "the xbe it was recompiled from")
set(GEN ${XBE_GENERATED})
set(XBE ${XBE_IMAGE})
if(NOT EXISTS ${GEN}/x86_func_mapping.cpp OR NOT EXISTS ${GEN}/image_symbols.h)
    message(FATAL_ERROR "No recompiled code: put the game in assets/ and recompile it first (see ANDROID.md), "
                        "it writes ${GEN}. For a build without it (a shell APK, to prove the port builds), run\n"
                        "  python3 tools/android/make_stub_game.py\nwhich writes a stand-in there.")
endif()
# tools/android/make_stub_game.py writes a folder of the recompiler's shape with no game code in it: that APK builds,
# installs and stops at "game not found", which is what tests the toolchain and the port itself
set(SHADOW_STUB OFF)
if(EXISTS ${GEN}/stub.txt)
    set(SHADOW_STUB ON)
    message(STATUS "android: no game code, building a shell APK from ${GEN}")
endif()
set(GEN_SOURCES ${GEN}/x86_func_mapping.cpp)
if(SHADOW_STUB)
    if(NOT EXISTS ${XBE})
        set(XBE "")
    endif()
else()
    if(NOT EXISTS ${XBE})
        message(FATAL_ERROR "No game image: put the USA disc's default.xbe in assets/default.xbe")
    endif()
    file(STRINGS ${REPO}/config/game.toml GAME_PARTITIONS REGEX "^partitions *=")
    string(REGEX MATCH "[0-9]+" GAME_PARTITIONS "${GAME_PARTITIONS}")
    math(EXPR last "${GAME_PARTITIONS} - 1")
    foreach(i RANGE ${last})
        list(APPEND GEN_SOURCES ${GEN}/x86_recomp.${i}.cpp)
    endforeach()
endif()
if(XBE)
    file(SHA1 ${XBE} xbe_sha1)
else()
    set(xbe_sha1 "0000000000000000000000000000000000000000")  # no image to check a game against
endif()
file(CONFIGURE OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/image_sha1.cpp
     CONTENT "extern const char kImageSha1[] = \"${xbe_sha1}\";\n")

# dependencies: SDL3 from the checkout Gradle makes (its Java side goes into the APK), the rest fetched
set(SDL_DIR ${REPO}/android/third_party/SDL)
if(NOT EXISTS ${SDL_DIR}/CMakeLists.txt)
    message(FATAL_ERROR "SDL3 is missing from ${SDL_DIR}: the Gradle build clones it")
endif()
set(SDL_SHARED ON CACHE BOOL "" FORCE)
set(SDL_STATIC OFF CACHE BOOL "" FORCE)
set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
set(SDL_RENDER OFF CACHE BOOL "" FORCE)
set(SDL_GPU OFF CACHE BOOL "" FORCE)
set(SDL_CAMERA OFF CACHE BOOL "" FORCE)
add_subdirectory(${SDL_DIR} ${CMAKE_CURRENT_BINARY_DIR}/SDL EXCLUDE_FROM_ALL)

set(ENABLE_OPT OFF CACHE BOOL "" FORCE)
set(ENABLE_HLSL ON CACHE BOOL "" FORCE)
set(ENABLE_GLSLANG_BINARIES OFF CACHE BOOL "" FORCE)
set(ENABLE_SPVREMAPPER OFF CACHE BOOL "" FORCE)
set(GLSLANG_TESTS OFF CACHE BOOL "" FORCE)
set(GLSLANG_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
set(BUILD_EXTERNAL OFF CACHE BOOL "" FORCE)
include(FetchContent)
FetchContent_Declare(glslang GIT_REPOSITORY https://github.com/KhronosGroup/glslang.git GIT_TAG 16.0.0 GIT_SHALLOW TRUE)
FetchContent_Declare(tomlplusplus GIT_REPOSITORY https://github.com/marzer/tomlplusplus.git GIT_TAG v3.4.0 GIT_SHALLOW TRUE)
FetchContent_Declare(imgui GIT_REPOSITORY https://github.com/ocornut/imgui.git GIT_TAG v1.91.8 GIT_SHALLOW TRUE)
# the GL ES renderer takes the same SPIR-V the Vulkan one runs, and writes it out as GLSL ES 3.00
set(SPIRV_CROSS_CLI OFF CACHE BOOL "" FORCE)
set(SPIRV_CROSS_ENABLE_TESTS OFF CACHE BOOL "" FORCE)
set(SPIRV_CROSS_ENABLE_CPP OFF CACHE BOOL "" FORCE)
set(SPIRV_CROSS_ENABLE_MSL OFF CACHE BOOL "" FORCE)
set(SPIRV_CROSS_ENABLE_HLSL OFF CACHE BOOL "" FORCE)
set(SPIRV_CROSS_ENABLE_REFLECT OFF CACHE BOOL "" FORCE)
set(SPIRV_CROSS_ENABLE_C_API OFF CACHE BOOL "" FORCE)
set(SPIRV_CROSS_ENABLE_UTIL OFF CACHE BOOL "" FORCE)
set(SPIRV_CROSS_SKIP_INSTALL ON CACHE BOOL "" FORCE)
FetchContent_Declare(spirv_cross GIT_REPOSITORY https://github.com/KhronosGroup/SPIRV-Cross.git GIT_TAG vulkan-sdk-1.3.296.0 GIT_SHALLOW TRUE)
FetchContent_MakeAvailable(glslang tomlplusplus imgui spirv_cross)

# the SDK's runtime but its Windows parts (D3D12, XAudio2, the texture dumper's WIC) and the host side this port
# writes itself (native/app.cpp and friends stand in for app/)
file(GLOB RUNTIME_SOURCES ${SDK}/runtime/*/*.cpp)
list(FILTER RUNTIME_SOURCES EXCLUDE REGEX
     "/runtime/app/|/core/input\\.cpp$|/gpu/(d3d12|texturedumper)\\.cpp$|/audio/xaudio2\\.cpp$")
# the game's own sources: its hooks, plus the port's render extras and HD text, which are plain C++
set(GAME_HOST_SOURCES ${GAME}/game/hooks.cpp ${GAME}/common/render_ext.cpp ${GAME}/common/pc_text.cpp)
file(GLOB ANDROID_SOURCES CONFIGURE_DEPENDS ${NATIVE}/*.cpp)
# the Spider-Man 2 renderer: 3200 lines of OpenGL in its own folder, listed one by one so nothing else in there is built
list(APPEND ANDROID_SOURCES ${NATIVE}/gl43/gl_backend.cpp ${NATIVE}/gl43/glsl.cpp ${NATIVE}/gl43/gl_api.cpp
     ${NATIVE}/gl43/gl_host_sdl.cpp)
file(GLOB COMPAT_SOURCES CONFIGURE_DEPENDS ${NATIVE}/compat/*.cpp)
# ImGui draws through whichever renderer the port chose, so both backends are in the APK
set(IMGUI_SOURCES ${imgui_SOURCE_DIR}/imgui.cpp ${imgui_SOURCE_DIR}/imgui_draw.cpp ${imgui_SOURCE_DIR}/imgui_tables.cpp
    ${imgui_SOURCE_DIR}/imgui_widgets.cpp ${imgui_SOURCE_DIR}/backends/imgui_impl_vulkan.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_opengl3.cpp)

add_library(main SHARED ${RUNTIME_SOURCES} ${GAME_HOST_SOURCES} ${ANDROID_SOURCES} ${COMPAT_SOURCES} ${GEN_SOURCES}
            ${IMGUI_SOURCES} ${CMAKE_CURRENT_BINARY_DIR}/image_sha1.cpp)
# compat/ for the SDK's x86 header, the SDK harness's for the Winsock and DXGI names its portable runtime uses
# ${SDK}/harness: the Spider-Man 2 renderer includes the harness's reference header for the box a small draw gets
target_include_directories(main PRIVATE ${NATIVE}/gl43 ${NATIVE}/compat ${SDK}/harness/compat ${SDK}/harness ${NATIVE} ${GAME}
                           ${SDK}/runtime ${GEN}
                           ${CMAKE_CURRENT_BINARY_DIR} ${glslang_SOURCE_DIR} ${spirv_cross_SOURCE_DIR})
target_include_directories(main SYSTEM PRIVATE ${imgui_SOURCE_DIR})
# x86 chars are signed, as the game's code and the runtime were written for
target_compile_options(main PRIVATE -fno-strict-aliasing -fsigned-char)
set(HOST_SOURCES ${RUNTIME_SOURCES} ${GAME_HOST_SOURCES} ${ANDROID_SOURCES})
set_source_files_properties(${HOST_SOURCES} PROPERTIES COMPILE_OPTIONS "-include;${NATIVE}/compat/crt.h")
set_source_files_properties(${GEN_SOURCES} PROPERTIES
    COMPILE_OPTIONS "-O2;-w;-fno-strict-aliasing;-fsigned-char")
set_source_files_properties(${NATIVE}/vk_shader.cpp PROPERTIES COMPILE_DEFINITIONS ENABLE_HLSL)
# ImGui's GL ES 3 backend, and the port's own GL ES renderer: GLES2/gl2.h and EGL from the NDK
set_source_files_properties(${imgui_SOURCE_DIR}/backends/imgui_impl_opengl3.cpp PROPERTIES COMPILE_DEFINITIONS IMGUI_IMPL_OPENGL_ES3)
target_link_libraries(main PRIVATE SDL3::SDL3 tomlplusplus::tomlplusplus glslang glslang-default-resource-limits
                      spirv-cross-glsl spirv-cross-core vulkan EGL GLESv3 android log)
# 16 KB pages, as Android 15 devices may have
target_link_options(main PRIVATE -Wl,-z,max-page-size=16384)
if(SHADOW_STUB)
    # the shell build has no game code, so the functions the SDK's XDK layer and the game's hooks call into the image
    # (sub_002DD1C0 and the like) are not there to link against. Leave them unresolved instead of failing the link:
    # libmain.so still loads, because nothing calls them, and the app stops at "the game is not here". A build with
    # the game keeps the linker strict.
    target_link_options(main PRIVATE -Wl,--unresolved-symbols=ignore-all)
    message(STATUS "android: shell build, the game's functions are left unresolved")
endif()
