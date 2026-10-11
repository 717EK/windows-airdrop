cmake_minimum_required(VERSION 3.20)
project(windows_airdrop C CXX)

# GCC/Clang only for now: OWL relies on __attribute__((packed)) and
# __builtin_bswap*. MinGW-w64 GCC and clang/clang-cl both accept them; MSVC cl doesn't.
if (MSVC AND NOT CMAKE_C_COMPILER_ID MATCHES "Clang")
    message(FATAL_ERROR "Use MinGW-w64 GCC or clang-cl; MSVC cl can't compile OWL's packed structs")
endif ()

include(FetchContent)

# Pins, from omdrop-owl's ./pins (docs/section16-audit.md has the full map).
FetchContent_Declare(omdrop_owl
    GIT_REPOSITORY https://github.com/t4t5/omdrop-owl.git
    GIT_TAG ca3de11584370325647b63f8d2ecf571bb10e9d2
    GIT_CONFIG core.autocrlf=false)
FetchContent_MakeAvailable(omdrop_owl)
file(GLOB OWL_PATCHES ${omdrop_owl_SOURCE_DIR}/patches/owl-*.patch)
list(SORT OWL_PATCHES)
# Our fixes to OWL, applied after omdrop-owl's (patches/README.md says why each exists).
file(GLOB WIN_PATCHES ${CMAKE_CURRENT_SOURCE_DIR}/patches/win-*.patch)
list(SORT WIN_PATCHES)

FetchContent_Declare(owl
    GIT_REPOSITORY https://github.com/jedbillyb/owl.git
    GIT_TAG 832d70f815c3d4a06a02117bf0fc5e868daa1ff0
    GIT_SUBMODULES radiotap   # OWL's bundled googletest (2018) doesn't build with current compilers
    GIT_CONFIG core.autocrlf=false
    PATCH_COMMAND git apply ${OWL_PATCHES} ${WIN_PATCHES}
    UPDATE_DISCONNECTED ON)
FetchContent_GetProperties(owl)
if (NOT owl_POPULATED)
    FetchContent_Populate(owl)   # sources only; OWL's own CMakeLists needs libpcap/libev/libnl
endif ()

FetchContent_Declare(googletest
    URL https://github.com/google/googletest/archive/refs/tags/v1.15.2.tar.gz
    URL_HASH SHA256=7b42b4d6ed48810c5362c265a17faebe90dc2373c885e5216439d37927f02926
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)

set(OWL ${owl_SOURCE_DIR})

add_library(radiotap STATIC ${OWL}/radiotap/radiotap.c)
target_include_directories(radiotap PUBLIC ${OWL}/radiotap compat)
target_compile_definitions(radiotap PRIVATE RADIOTAP_SUPPORT_OVERRIDES)
# It casts pointers to 32-bit `unsigned long` (LLP64), but only to subtract two
# of them, which wraps to the right offset for headers far under 4 GB.
target_compile_options(radiotap PRIVATE -Wno-pointer-to-int-cast -Wno-address-of-packed-member)

# OWL's portable core: frames, TLVs, sync, election, channel sequence, peers.
add_library(awdl STATIC
    ${OWL}/src/log.c ${OWL}/src/state.c ${OWL}/src/election.c ${OWL}/src/sync.c
    ${OWL}/src/channel.c ${OWL}/src/schedule.c ${OWL}/src/tx.c ${OWL}/src/rx.c
    ${OWL}/src/frame.c ${OWL}/src/crc32.c ${OWL}/src/wire.c ${OWL}/src/peers.c
    ${OWL}/src/version.c ${OWL}/src/hashmap.c ${OWL}/src/siphash24.c
    ${OWL}/src/circular_buffer.c
    compat/compat.c compat/clock.c)
set_source_files_properties(${OWL}/src/state.c PROPERTIES
    COMPILE_DEFINITIONS clock_time_us=owl_clock_monotonic_us)
target_include_directories(awdl PUBLIC ${OWL}/src compat)
target_link_libraries(awdl PUBLIC radiotap)
# radiotap.h assumes <stdint.h> arrives via glibc headers; MinGW's localtime_r is behind this macro.
target_compile_options(awdl PRIVATE -include stdint.h)
target_compile_definitions(awdl PRIVATE _POSIX_THREAD_SAFE_FUNCTIONS)

add_library(radio_mock STATIC radio/mock_radio.c)
target_include_directories(radio_mock PUBLIC radio)

# Replaces OWL's daemon/core.c timing loop; no libev/libpcap/TAP.
add_library(awdl_sched STATIC sched/awdl_sched.c)
target_include_directories(awdl_sched PUBLIC sched radio)
target_link_libraries(awdl_sched PUBLIC awdl)

enable_testing()
add_executable(owl_tests
    ${OWL}/tests/test_wire.cpp ${OWL}/tests/test_awdl_sync.cpp
    ${OWL}/tests/test_awdl_peers.cpp ${OWL}/tests/test_awdl_election.cpp
    ${OWL}/tests/test_awdl_channel.cpp ${OWL}/tests/test_awdl_txgate.cpp
    tests/test_mock_radio.cpp tests/test_sched.cpp)
target_link_libraries(owl_tests awdl_sched radio_mock GTest::gtest_main)
include(GoogleTest)
gtest_discover_tests(owl_tests)
