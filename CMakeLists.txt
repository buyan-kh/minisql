cmake_minimum_required(VERSION 3.20)
project(keel VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(KEEL_BUILD_TESTS "Build unit and SQL logic tests" ON)
option(KEEL_BUILD_BENCH "Build the SQLite comparison benchmark" ON)
option(KEEL_ASAN "Enable address and undefined-behavior sanitizers" OFF)
option(KEEL_WASM "Build the WebAssembly bindings" OFF)

if(KEEL_ASAN AND NOT KEEL_WASM)
  add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer)
  add_link_options(-fsanitize=address,undefined)
endif()

if(KEEL_WASM)
  add_compile_options(-fexceptions)
  add_link_options(-fexceptions)
endif()

set(KEEL_SOURCES
  src/crc32.cpp
  src/types.cpp
  src/disk_manager.cpp
  src/buffer_pool.cpp
  src/slotted.cpp
  src/store.cpp
  src/heap.cpp
  src/btree.cpp
  src/wal.cpp
  src/catalog.cpp
  src/parser.cpp
  src/expr.cpp
  src/planner.cpp
  src/executor.cpp
  src/database.cpp
)

add_library(keel STATIC ${KEEL_SOURCES})
target_include_directories(keel PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_options(keel PRIVATE -Wall -Wextra -Werror)

if(KEEL_BUILD_TESTS AND NOT KEEL_WASM)
  include(FetchContent)
  FetchContent_Declare(
    googletest
    URL https://github.com/google/googletest/archive/refs/tags/v1.15.2.zip
  )
  set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
  FetchContent_MakeAvailable(googletest)

  enable_testing()
  add_executable(keel_tests
    tests/storage_test.cpp
    tests/index_test.cpp
    tests/wal_test.cpp
    tests/sql_test.cpp
    tests/recovery_test.cpp
  )
  target_link_libraries(keel_tests PRIVATE keel GTest::gtest_main)
  target_compile_options(keel_tests PRIVATE -Wall -Wextra -Werror)
  target_compile_definitions(keel_tests PRIVATE KEEL_SQL_DIR="${CMAKE_SOURCE_DIR}/tests/sql")
  include(GoogleTest)
  gtest_discover_tests(keel_tests)
endif()

if(NOT KEEL_WASM)
  add_executable(keel_cli src/cli.cpp)
  target_link_libraries(keel_cli PRIVATE keel)
  target_compile_options(keel_cli PRIVATE -Wall -Wextra -Werror)
endif()

if(KEEL_WASM)
  add_executable(keel_wasm src/wasm.cpp)
  target_link_libraries(keel_wasm PRIVATE keel)
  target_compile_options(keel_wasm PRIVATE -Wall -Wextra -Werror)
  target_link_options(keel_wasm PRIVATE
    "-fexceptions"
    "SHELL:-s DISABLE_EXCEPTION_CATCHING=0"
    "SHELL:-s ALLOW_MEMORY_GROWTH=1"
    "SHELL:-s MODULARIZE=1"
    "SHELL:-s EXPORT_ES6=1"
    "SHELL:-s EXPORT_NAME=KeelModule"
    "SHELL:-s ENVIRONMENT=web,node"
    "SHELL:-s FILESYSTEM=1"
    "--bind"
  )
  set_target_properties(keel_wasm PROPERTIES OUTPUT_NAME "keel" SUFFIX ".js")
endif()

if(KEEL_BUILD_BENCH AND NOT KEEL_WASM)
  find_package(SQLite3)
  if(SQLite3_FOUND)
    add_executable(keel_bench bench/bench.cpp)
    target_link_libraries(keel_bench PRIVATE keel SQLite::SQLite3)
    target_compile_options(keel_bench PRIVATE -Wall -Wextra -Werror)
  else()
    message(STATUS "SQLite3 not found; keel_bench will not be built")
  endif()
endif()
