# The SDK's recompiler, built on Linux (or any host with clang/gcc) instead of the Windows toolchain, so a CI runner
# or a Steam Deck can turn the player's default.xbe into generated/<image>/ for the Android build.
#   cmake -S tools/android/xbe_recomp -B out/tools -G Ninja -DCMAKE_BUILD_TYPE=Release && cmake --build out/tools
# then, from the repository root:  out/tools/xbe_recomp config/game.toml --xdk sdk/xdk
cmake_minimum_required(VERSION 3.25)
project(shadow_xbe_tools LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

get_filename_component(REPO ${CMAKE_CURRENT_SOURCE_DIR}/../../.. ABSOLUTE)
set(SDK ${REPO}/sdk)
if(NOT EXISTS ${SDK}/tools/recomp/main.cpp)
    message(FATAL_ERROR "no SDK at ${SDK}")
endif()

include(FetchContent)
set(ZYDIS_BUILD_TOOLS OFF CACHE BOOL "" FORCE)
set(ZYDIS_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(ZYDIS_BUILD_DOXYGEN OFF CACHE BOOL "" FORCE)
FetchContent_Declare(zydis GIT_REPOSITORY https://github.com/zyantific/zydis.git GIT_TAG v4.1.1 GIT_SHALLOW TRUE)
FetchContent_Declare(tomlplusplus GIT_REPOSITORY https://github.com/marzer/tomlplusplus.git GIT_TAG v3.4.0 GIT_SHALLOW TRUE)
FetchContent_MakeAvailable(zydis tomlplusplus)

file(GLOB RECOMP_SOURCES CONFIGURE_DEPENDS ${SDK}/tools/recomp/*.cpp)
add_executable(xbe_recomp ${RECOMP_SOURCES})
target_include_directories(xbe_recomp PRIVATE ${SDK}/runtime ${SDK}/tools/recomp)
target_link_libraries(xbe_recomp PRIVATE Zydis tomlplusplus::tomlplusplus)
target_compile_options(xbe_recomp PRIVATE -w)  # the SDK's own code warns plenty and is not ours to change

add_executable(xbe_symbols ${SDK}/tools/symbols/main.cpp)
target_include_directories(xbe_symbols PRIVATE ${SDK}/runtime)
target_link_libraries(xbe_symbols PRIVATE Zydis tomlplusplus::tomlplusplus)
target_compile_options(xbe_symbols PRIVATE -w)
