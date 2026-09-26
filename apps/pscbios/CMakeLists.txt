# PSC-Bios: the console's hardware configuration tool (WiFi, timezone, gamepad mapping), an extension of the
# launcher since 2026-09-24 - a plugin the launcher loads (Extensions/pscbios/bin/<key>/pscbios.so), run from
# the System menu's Hardware Information item. Its SDK-free logic is pscbios_core.
#
# The plugin must not link the SDK's static libraries - the launcher has them, and a second copy would be a
# second Gui, Env and Lang - so pscbios_core's sources are compiled into the plugin itself, and the
# pscbios_core library (linking ab_core) is what the tests link.

set(PSCBIOS_CORE_SOURCES
        src/core/bluez_bus.h
        src/core/bluez_client.cpp
        src/core/bluez_client.h
        src/core/bt_device_list.cpp
        src/core/bt_device_list.h
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
        src/core/wifi_connect.cpp
        src/core/wifi_connect.h
)
# the native backend (docs/native-backend-plan.md): sysfs, getifaddrs and wpa_supplicant's control socket through
# hostap's vendored client - Unix only; the Windows dev build has the FakeBackend
set(PSCBIOS_WPA_CTRL_DIR ${CMAKE_CURRENT_SOURCE_DIR}/third_party/wpa_ctrl)
if (NOT WIN32)
    list(APPEND PSCBIOS_CORE_SOURCES
            src/core/native_backend.cpp
            src/core/native_backend.h
            src/core/wpa_ctrl_client.cpp
            src/core/wpa_ctrl_client.h
            third_party/wpa_ctrl/wpa_ctrl.c
            third_party/wpa_ctrl/wpa_ctrl.h)
    # the Unix-socket client only (third_party/wpa_ctrl/README.autobleem.txt); vendored, so no warnings
    set_source_files_properties(third_party/wpa_ctrl/wpa_ctrl.c PROPERTIES
            COMPILE_DEFINITIONS "CONFIG_CTRL_IFACE;CONFIG_CTRL_IFACE_UNIX"
            COMPILE_OPTIONS "-w")

    # BlueZ over D-Bus: libdbus-1 (the owner's choice - every console has libdbus-1.so.3, stock included; its
    # headers are in the build image's console sysroot). BluezClient itself is built everywhere and tested with a
    # fake bus; only its libdbus transport needs the library. The console build must have it; a host without the
    # dev package builds the rest, and NativeBackend's Bluetooth then answers "built without libdbus-1".
    # dbus-arch-deps.h is under lib/<multiarch>/dbus-1.0/include, which neither pkg-config (pointed at the
    # sysroot's usr/lib/pkgconfig by the console toolchain) nor find_path's defaults reach - so it is looked for
    # there, relative to the sysroot (find_path re-roots the paths into it).
    file(GLOB _pscbios_dbus_multiarch RELATIVE "${CMAKE_SYSROOT}/" "${CMAKE_SYSROOT}/usr/lib/*/dbus-1.0/include")
    set(_pscbios_dbus_arch_paths /usr/lib/dbus-1.0/include /usr/lib64/dbus-1.0/include)
    set(_pscbios_dbus_lib_suffixes "")
    foreach (_dir ${_pscbios_dbus_multiarch})
        list(APPEND _pscbios_dbus_arch_paths "/${_dir}")
        string(REGEX REPLACE "^usr/lib/([^/]+)/.*" "\\1" _arch "${_dir}")
        list(APPEND _pscbios_dbus_lib_suffixes "${_arch}")
    endforeach ()
    find_path(PSCBIOS_DBUS_INCLUDE_DIR dbus/dbus.h PATH_SUFFIXES dbus-1.0)
    find_path(PSCBIOS_DBUS_ARCH_INCLUDE_DIR dbus/dbus-arch-deps.h PATHS ${_pscbios_dbus_arch_paths})
    find_library(PSCBIOS_DBUS_LIBRARY dbus-1 PATH_SUFFIXES ${_pscbios_dbus_lib_suffixes})
    if (PSCBIOS_DBUS_INCLUDE_DIR AND PSCBIOS_DBUS_ARCH_INCLUDE_DIR AND PSCBIOS_DBUS_LIBRARY)
        message(STATUS "pscbios: libdbus-1 ${PSCBIOS_DBUS_LIBRARY} - Bluetooth through BlueZ")
        set(PSCBIOS_HAVE_DBUS ON)
        list(APPEND PSCBIOS_CORE_SOURCES src/core/bluez_dbus_bus.cpp src/core/bluez_dbus_bus.h)
    elseif (AB_TARGET STREQUAL "psc")
        message(FATAL_ERROR "pscbios: no libdbus-1 in the console sysroot (headers ${PSCBIOS_DBUS_INCLUDE_DIR}, "
                "${PSCBIOS_DBUS_ARCH_INCLUDE_DIR}, library ${PSCBIOS_DBUS_LIBRARY}) - the build image's psc stage has it")
    else ()
        message(STATUS "pscbios: no libdbus-1 dev files - built without the D-Bus transport (Bluetooth unavailable)")
        set(PSCBIOS_HAVE_DBUS OFF)
    endif ()
endif ()
# PSCBIOS_HAVE_DBUS: the definition, include dirs and library, for pscbios_core and the plugin alike (the
# plugin links with the keyword signature, as ab_add_extension does)
function(pscbios_use_dbus target)
    if (PSCBIOS_HAVE_DBUS)
        target_compile_definitions(${target} PRIVATE PSCBIOS_HAVE_DBUS)
        target_include_directories(${target} PRIVATE ${PSCBIOS_DBUS_INCLUDE_DIR} ${PSCBIOS_DBUS_ARCH_INCLUDE_DIR})
        target_link_libraries(${target} ${ARGN} ${PSCBIOS_DBUS_LIBRARY}) # ARGN: PRIVATE for the plugin
    endif ()
endfunction()
add_library(pscbios_core STATIC ${PSCBIOS_CORE_SOURCES})
target_include_directories(pscbios_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src PRIVATE ${PSCBIOS_WPA_CTRL_DIR})
target_link_libraries(pscbios_core ab_core)
pscbios_use_dbus(pscbios_core)

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
        src/screens/gui_network_hub.cpp
        src/screens/gui_network_hub.h
        src/screens/gui_network_menu.cpp
        src/screens/gui_network_menu.h
        src/screens/gui_pad_config.cpp
        src/screens/gui_pad_config.h
        src/screens/gui_pscbios_main.cpp
        src/screens/gui_pscbios_main.h
        src/screens/gui_ssid_scan_menu.cpp
        src/screens/gui_ssid_scan_menu.h
        src/screens/pscbios_busy.cpp
        src/screens/pscbios_busy.h
        src/screens/pscbios_pages.cpp
        src/screens/pscbios_pages.h
        INI ${CMAKE_CURRENT_SOURCE_DIR}/resources/extension.ini
        ICON ${CMAKE_CURRENT_SOURCE_DIR}/resources/icon.png
        LANG ${CMAKE_CURRENT_SOURCE_DIR}/resources/lang)
target_include_directories(pscbios PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src ${PSCBIOS_WPA_CTRL_DIR})
pscbios_use_dbus(pscbios PRIVATE)
# the console (and a Linux host) gets NativeBackend; Windows only has the FakeBackend (AB_DEBUG_HOST uses it anyway)
if (NOT WIN32)
    target_compile_definitions(pscbios PRIVATE PSCBIOS_NATIVE_BACKEND)
endif ()

# the tool's other files next to the library, found through Env::getAppDir() (the extension's folder while it runs)
set(pscbios_stage "${CMAKE_BINARY_DIR}/extensions/pscbios")
add_custom_command(TARGET pscbios POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different ${CMAKE_CURRENT_SOURCE_DIR}/resources/DS3.png
                ${CMAKE_CURRENT_SOURCE_DIR}/resources/gamecontrollerdb.txt
                ${CMAKE_CURRENT_SOURCE_DIR}/resources/readme.txt ${pscbios_stage})
