# The console tools' unit tests, on autobleem-core's harness (doctest, EnvFixture, TempDir, ab_add_test).
# Host builds only: nothing here ships to the console.
include(${CMAKE_SOURCE_DIR}/autobleem-core/tests/harness.cmake)

ab_add_test(test_pscbios_core apps/test_pscbios_core.cpp LIBS pscbios_core)
ab_add_test(test_abflashkit_core apps/test_abflashkit_core.cpp LIBS abflashkit_core)
