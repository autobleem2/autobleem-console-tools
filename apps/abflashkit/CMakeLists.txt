# ABFlashKit: the console's kernel installer, an App the launcher starts from Apps/abflashkit. Two targets:
# abflashkit_core is everything that is not a screen and links ab_core only (so the tests can link it),
# abflashkit is the program on ab_classic.

add_library(abflashkit_core STATIC
        src/core/factory_restore.cpp
        src/core/factory_restore.h
        src/core/flash_actions.cpp
        src/core/flash_actions.h
        src/core/flasher.cpp
        src/core/flasher.h
        src/core/games_backup.cpp
        src/core/games_backup.h
        src/core/lboot_backup.cpp
        src/core/lboot_backup.h
        src/core/lboot_signature.h
        src/core/led.cpp
        src/core/led.h
)
target_include_directories(abflashkit_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/src/code)
# the manifest and the Eris data are JSON
target_include_directories(abflashkit_core PRIVATE ${CMAKE_SOURCE_DIR}/autobleem-core/lib_ableem/third_party/nlohmann)
target_link_libraries(abflashkit_core ab_core)

add_executable(abflashkit
        src/abflashkit_app.cpp
        src/abflashkit_app.h
        src/main.cpp
        src/screens/gui_flashkit_main.cpp
        src/screens/gui_flashkit_main.h
)
target_link_libraries(abflashkit abflashkit_core ab_classic Threads::Threads ${CMAKE_DL_LIBS})

# the tool's own files next to the binary (what ships in Apps/abflashkit; the kernel/ payload is fetched
# from autobleem2/psc-kernel-payload's releases and staged into Apps/abflashkit/kernel at package time -
# release artefacts, not sources)
add_custom_command(TARGET abflashkit PRE_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory
        ${CMAKE_CURRENT_SOURCE_DIR}/resources $<TARGET_FILE_DIR:abflashkit>/abflashkit_resources)
# Restore to factory's stock kernel image and data files, next to the binary like the package lays them
add_custom_command(TARGET abflashkit PRE_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory
        ${CMAKE_CURRENT_SOURCE_DIR}/stock $<TARGET_FILE_DIR:abflashkit>/abflashkit_stock
        COMMAND ${CMAKE_COMMAND} -E copy_directory
        ${CMAKE_CURRENT_SOURCE_DIR}/data $<TARGET_FILE_DIR:abflashkit>/abflashkit_data)
