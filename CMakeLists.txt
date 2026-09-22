# autobleem-console-tools: PSC-Bios and ABFlashKit, the on-console apps. They draw with the classic UI and
# link the AutoBleem SDK (ab_core + ab_classic) from the autobleem-core submodule.
cmake_minimum_required(VERSION 3.12)
project(autobleem-console-tools)

set(CMAKE_CXX_STANDARD 14)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
add_compile_definitions(AB_PLATFORM_DEV)   # host/dev build; the console build sets AB_TARGET=psc
find_package(Threads REQUIRED)

# the SDK submodule defines ableem_engine, ableem, ab_core, ab_classic (with src/code as a PUBLIC include).
# No tests or theme_convert here - those are the core repo's.
set(AB_BUILD_TESTS OFF CACHE BOOL "" FORCE)
add_subdirectory(autobleem-core)

add_subdirectory(apps/pscbios)
add_subdirectory(apps/abflashkit)
