include(FetchContent)
FetchContent_Declare(googletest
  GIT_REPOSITORY https://github.com/google/googletest.git
  GIT_TAG        063de7e9578f82b369302001269680b4b1553359  # v1.18.0
)
set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
set(BUILD_GMOCK   OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)
include(GoogleTest)

add_executable(oems_tests
  quickfix_smoke_test.cpp
)
target_link_libraries(oems_tests PRIVATE oems_options quickfix GTest::gtest_main)

# PRE_TEST: list the tests when ctest runs, not at build time.
gtest_discover_tests(oems_tests DISCOVERY_MODE PRE_TEST)
