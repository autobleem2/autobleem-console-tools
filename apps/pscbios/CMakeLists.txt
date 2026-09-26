# PSC-Bios: the console's hardware configuration tool (WiFi, timezone, gamepad mapping), an extension of the
# launcher since 2026-09-24 - a plugin the launcher loads (Extensions/pscbios/bin/<key>/pscbios.so), run from
# the System menu's Hardware Information item. Its SDK-free logic is pscbios_core.
#
# The plugin must not link the SDK's static libraries - the launcher has them, and a second copy would be a
# second Gui, Env and Lang - so pscbios_core's sources are compiled into the plugin itself, and the
# pscbios_core library (linking ab_core) is what the tests link.

set(PSCBIOS_CORE_SOURCES
        src/core/console_backend.cpp
        src/core/console_backend.h
        src/core/game_controller_db.cpp
        src/core/game_controller_db.h
        src/core/network_status.cpp
        src/core/network_status.h
        src/core/nm_backend.cpp
        src/core/nm_backend.h
        src/core/pad_mapping.cpp
        src/core/pad_mapping.h
        src/core/ssid_config.cpp
        src/core/ssid_config.h
)
add_library(pscbios_core STATIC ${PSCBIOS_CORE_SOURCES})
target_include_directories(pscbios_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(pscbios_core ab_core)

if (NOT COMMAND ab_add_extension)
    include(${CMAKE_SOURCE_DIR}/autobleem-core/cmake/ab_extension.cmake)
endif ()
# built with the launcher (its AB_EXTENSION_DIRS, a dev host) the host is the launcher's executable; in this
# repository there is none, which only Windows needs (for its import library)
if (DEFINED AB_EXTENSION_HOST)
    set(pscbios_host HOST ${AB_EXTENSION_HOST})
elseif (WIN32)
    message(STATUS "pscbios: an extension is built on Windows with the launcher (AB_EXTENSION_DIRS) - skipped")
    return()
endif ()

ab_add_extension(pscbios ${pscbios_host}
        SOURCES ${PSCBIOS_CORE_SOURCES}
        src/pscbios.cpp
        src/pscbios.h
        src/pscbios_extension.cpp
        src/screens/gui_bt_pairing.cpp
        src/screens/gui_bt_pairing.h
        src/screens/gui_gamepad_menu.cpp
        src/screens/gui_gamepad_menu.h
        src/screens/gui_network_menu.cpp
        src/screens/gui_network_menu.h
        src/screens/gui_pad_config.cpp
        src/screens/gui_pad_config.h
        src/screens/gui_pscbios_main.cpp
        src/screens/gui_pscbios_main.h
        src/screens/gui_ssid_scan_menu.cpp
        src/screens/gui_ssid_scan_menu.h
        src/screens/pscbios_pages.cpp
        src/screens/pscbios_pages.h
        INI ${CMAKE_CURRENT_SOURCE_DIR}/resources/extension.ini
        ICON ${CMAKE_CURRENT_SOURCE_DIR}/resources/icon.png
        LANG ${CMAKE_CURRENT_SOURCE_DIR}/resources/lang)
target_include_directories(pscbios PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)

# the tool's other files next to the library, found through Env::getAppDir() (the extension's folder while it runs)
set(pscbios_stage "${CMAKE_BINARY_DIR}/extensions/pscbios")
add_custom_command(TARGET pscbios POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different ${CMAKE_CURRENT_SOURCE_DIR}/resources/DS3.png
                ${CMAKE_CURRENT_SOURCE_DIR}/resources/bt ${CMAKE_CURRENT_SOURCE_DIR}/resources/gamecontrollerdb.txt
                ${CMAKE_CURRENT_SOURCE_DIR}/resources/readme.txt ${pscbios_stage})
