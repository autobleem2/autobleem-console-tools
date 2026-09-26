# autobleem-console-tools: PSC-Bios (an extension of the launcher, Extensions/pscbios) and ABFlashKit (an App,
# Apps/abflashkit), the console's own tools. They draw with the classic UI and
# use the AutoBleem SDK (ab_core + ab_classic) from the autobleem-core submodule - ABFlashKit links it, PSC-Bios
# is built with its headers and binds to the launcher's copy when it is loaded.
cmake_minimum_required(VERSION 3.12)
project(autobleem-console-tools)

set(CMAKE_CXX_STANDARD 14)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# AB_TARGET: psc (cross-compiled for the console) or dev (a host build, the default) - these tools build
# for nothing else (CLAUDE.md: "the console tools build for psc/dev"). Read as a CACHE variable so it is
# genuinely shared with the autobleem-core submodule below: CMake directory scope does not flow sideways
# (a sibling add_subdirectory does not inherit a plain set() from another sibling), so autobleem-core's own
# CMakeLists.txt independently reads this SAME cache variable for its own AB_PLATFORM_<X> macro - see the
# comment there. Passed as -DAB_TARGET=psc alongside -DCMAKE_TOOLCHAIN_FILE=toolchains/psc/PSCtoolchainV8.cmake.
set(AB_TARGET "" CACHE STRING "psc | dev (empty: psc on ARM, dev elsewhere)")
if (AB_TARGET STREQUAL "")
    if (CMAKE_SYSTEM_PROCESSOR MATCHES "^arm" OR CMAKE_SYSTEM_PROCESSOR STREQUAL "aarch64")
        set(AB_TARGET "psc")
    else()
        set(AB_TARGET "dev")
    endif()
endif()
if (NOT AB_TARGET STREQUAL "psc" AND NOT AB_TARGET STREQUAL "dev")
    message(FATAL_ERROR "AB_TARGET must be psc or dev for the console tools (got '${AB_TARGET}')")
endif()
string(TOUPPER "${AB_TARGET}" _ab_target_upper)
message(STATUS "autobleem-console-tools: building for target ${AB_TARGET} (AB_PLATFORM_${_ab_target_upper})")
add_compile_definitions(AB_PLATFORM_${_ab_target_upper})

if (AB_TARGET STREQUAL "psc")
    # the console CPU flags (root CMakeLists.txt's own psc branch, ported here 2026-09-23): set at THIS
    # top level, before either add_subdirectory below, so both the autobleem-core submodule's own internal
    # libraries (lib_ableem/ab_core/ab_classic, descendants of THIS scope) and apps/pscbios+abflashkit
    # (siblings of the submodule, also descendants of THIS scope) inherit the SAME flags - putting this
    # inside the submodule instead would only reach its own internals, not these apps. The toolchain file
    # deliberately does not set CPU flags itself, expecting the top-level project to (its own comment).
    # The Cortex-A35 (ARMv8-A/AArch32, NEON); GCC 8+ spells that armv8-a+simd, the image's Stretch gcc-6
    # (the newest whose libstdc++ matches the console's glibc 2.24) takes the -mfpu spelling instead.
    if (CMAKE_CXX_COMPILER_VERSION VERSION_GREATER 8.0)
        set(CMAKE_C_FLAGS "-mfloat-abi=hard -march=armv8-a+simd -Os -s")
        set(CMAKE_CXX_FLAGS "-mfloat-abi=hard -march=armv8-a+simd -Os -s")
    else()
        set(CMAKE_C_FLAGS "-mfloat-abi=hard -march=armv8-a -mfpu=neon-vfpv4 -Os -s")
        set(CMAKE_CXX_FLAGS "-mfloat-abi=hard -march=armv8-a -mfpu=neon-vfpv4 -Os -s")
    endif()
    # 64-bit file offsets on the 32-bit console: without them stat() fails (EOVERFLOW) on a file over 2 GB,
    # so a big Store download or disc image reads as size 0. The PC stick's toolchain does the same.
    add_definitions(-D_FILE_OFFSET_BITS=64)
    if (CMAKE_SYSROOT)
        include_directories("${CMAKE_SYSROOT}/usr/include")
        link_directories("${CMAKE_SYSROOT}/usr/lib")
    endif()
endif()

find_package(Threads REQUIRED)

# the SDK submodule defines ableem_engine, ableem, ab_core, ab_classic (with src/code as a PUBLIC include).
# No tests or theme_convert here - those are the core repo's; AB_TARGET passed through explicitly (forced
# into the cache) so both sides of the split are reading the exact same decision, not two independent
# guesses that happen to agree.
set(AB_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(AB_TARGET "${AB_TARGET}" CACHE STRING "" FORCE)
add_subdirectory(autobleem-core)

add_subdirectory(apps/pscbios)
add_subdirectory(apps/abflashkit)

# the tools' own suites (tests/), on the core's harness - host builds only
if (AB_TARGET STREQUAL "dev")
    option(AB_TOOL_TESTS "Build the console tools' unit tests" ON)
else()
    set(AB_TOOL_TESTS OFF)
endif()
if (AB_TOOL_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
