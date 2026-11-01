include(FetchContent)
FetchContent_Declare(googletest
  URL https://github.com/google/googletest/releases/download/v1.17.0/googletest-1.17.0.tar.gz
  URL_HASH SHA256=65fab701d9829d38cb77c14acdc431d2108bfdbf8979e40eb8ae567edf10b27c
  DOWNLOAD_EXTRACT_TIMESTAMP ON)
set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
set(BUILD_GMOCK OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)

add_executable(unit_tests
  test_smoke.cpp
  test_parser.cpp
  test_book.cpp
  test_book_random.cpp
  test_fast_book.cpp
  test_signals.cpp
  test_strategy_formulas.cpp
  test_risk.cpp
  test_exchange.cpp
  test_system.cpp
  test_spsc.cpp
)
target_link_libraries(unit_tests PRIVATE core GTest::gtest_main)

include(GoogleTest)
gtest_discover_tests(unit_tests DISCOVERY_TIMEOUT 60)

# Parser fuzzing: libFuzzer when asked for (clang on Linux), otherwise the
# standalone mutation driver, which also runs as a quick test in every build.
add_executable(make_seeds fuzz/make_seeds.cpp)
target_link_libraries(make_seeds PRIVATE core)

if(BUILD_FUZZER)
  add_executable(fuzz_parser fuzz/fuzz_parser.cpp)
  target_compile_options(fuzz_parser PRIVATE -fsanitize=fuzzer)
  target_link_options(fuzz_parser PRIVATE -fsanitize=fuzzer)
  target_link_libraries(fuzz_parser PRIVATE core)
else()
  add_executable(fuzz_parser fuzz/fuzz_parser.cpp fuzz/standalone_main.cpp)
  target_link_libraries(fuzz_parser PRIVATE core)
  add_test(NAME fuzz_parser_smoke
           COMMAND fuzz_parser --random 200000 ${CMAKE_CURRENT_SOURCE_DIR}/fuzz/seeds)
endif()

# The hot path must not allocate. Not in sanitizer builds (they replace operator new).
if(NOT SANITIZE)
  add_executable(no_allocation_test test_no_allocation.cpp ../src/apps/alloc_counter.cpp)
  target_link_libraries(no_allocation_test PRIVATE core GTest::gtest)
  add_test(NAME no_allocation_on_the_hot_path COMMAND no_allocation_test)
endif()
