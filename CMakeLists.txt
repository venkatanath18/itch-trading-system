cmake_minimum_required(VERSION 3.25)
project(p6_trading_system LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

set(SANITIZE "" CACHE STRING "address (ASan + UBSan), thread (TSan) or empty")
option(NATIVE_ARCH "Tune the code for this machine's CPU" OFF)
option(BUILD_FUZZER "Build the parser fuzz target with libFuzzer (clang only)" OFF)

if(SANITIZE STREQUAL "address")
  add_compile_options(-fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer)
  add_link_options(-fsanitize=address,undefined)
elseif(SANITIZE STREQUAL "thread")
  add_compile_options(-fsanitize=thread -fno-omit-frame-pointer)
  add_link_options(-fsanitize=thread)
elseif(NOT SANITIZE STREQUAL "")
  message(FATAL_ERROR "SANITIZE must be address, thread or empty, not '${SANITIZE}'")
endif()

if(NATIVE_ARCH)
  if(CMAKE_SYSTEM_PROCESSOR MATCHES "arm64|aarch64")
    add_compile_options(-mcpu=native)
  else()
    add_compile_options(-march=native)
  endif()
endif()

# Warnings are errors for our own code; GoogleTest keeps its own flags.
add_library(project_warnings INTERFACE)
target_compile_options(project_warnings INTERFACE
  -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wnon-virtual-dtor -Werror)

# Compiler, flags and build type, so every result file can name them.
string(TOUPPER "${CMAKE_BUILD_TYPE}" build_type_upper)
get_directory_property(extra_options COMPILE_OPTIONS)
string(JOIN " " BUILD_FLAGS ${CMAKE_CXX_FLAGS} ${CMAKE_CXX_FLAGS_${build_type_upper}} ${extra_options})
configure_file(cmake/build_info.hpp.in ${CMAKE_BINARY_DIR}/generated/build_info.hpp @ONLY)

add_library(core STATIC
  src/common/mapped_file.cpp
  src/common/chunked_reader.cpp
  src/common/machine.cpp
  src/book/sizing.cpp
  src/infra/log.cpp
)
target_include_directories(core PUBLIC include ${CMAKE_BINARY_DIR}/generated)
target_link_libraries(core PUBLIC project_warnings)
find_package(Threads REQUIRED)
target_link_libraries(core PUBLIC Threads::Threads)

add_subdirectory(src)
add_subdirectory(bench)

enable_testing()
add_subdirectory(tests)
