cmake_minimum_required(VERSION 3.20)
project(stratadb VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(LSM_SANITIZER "" CACHE STRING "Sanitizer: empty, address, or thread")
set_property(CACHE LSM_SANITIZER PROPERTY STRINGS "" address thread)
option(LSM_BUILD_BENCHMARKS "Fetch Google Benchmark for benchmark targets" OFF)

if(NOT LSM_SANITIZER MATCHES "^(|address|thread)$")
  message(FATAL_ERROR "Unknown LSM_SANITIZER: ${LSM_SANITIZER}")
endif()

if(MSVC)
  # /RTC1 and incremental linking are incompatible with AddressSanitizer.
  if(LSM_SANITIZER STREQUAL "thread")
    message(FATAL_ERROR "ThreadSanitizer requires the Linux toolchain")
  elseif(LSM_SANITIZER STREQUAL "address")
    set(CMAKE_CXX_FLAGS_DEBUG "/O1 /Zi")
    add_compile_options(/fsanitize=address)
    add_link_options(/INCREMENTAL:NO /DEBUG)
  else()
    set(CMAKE_CXX_FLAGS_DEBUG "/Od /Zi /RTC1")
  endif()
else()
  set(CMAKE_CXX_FLAGS_DEBUG "-O0 -g")
  set(CMAKE_CXX_FLAGS_RELEASE "-O3 -march=native -DNDEBUG")
  if(LSM_SANITIZER)
    set(CMAKE_CXX_FLAGS_DEBUG "-O1 -g")
    add_compile_options(-fno-omit-frame-pointer)
    if(LSM_SANITIZER STREQUAL "address")
      add_compile_options(-fsanitize=address,undefined)
      add_link_options(-fsanitize=address,undefined)
    else()
      add_compile_options(-fsanitize=thread)
      add_link_options(-fsanitize=thread)
    endif()
  endif()
endif()

function(lsm_strict_warnings target)
  if(MSVC)
    target_compile_options(${target} PRIVATE /W4 /WX /permissive- /utf-8)
  else()
    target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic -Werror)
  endif()
endfunction()

add_library(lsm src/status.cpp src/util/crc32c.cpp src/util/hash.cpp src/util/varint.cpp)
target_sources(lsm PRIVATE src/write_batch.cpp)
target_sources(lsm PRIVATE src/memtable.cpp)
target_sources(lsm PRIVATE src/wal.cpp)
target_sources(lsm PRIVATE src/db.cpp)
target_sources(lsm PRIVATE src/table/block.cpp)
target_sources(lsm PRIVATE src/table/block_iterator.cpp)
target_sources(lsm PRIVATE src/table/table_iterator.cpp)
target_sources(lsm PRIVATE src/table/bloom.cpp)
target_sources(lsm PRIVATE src/table/block_cache.cpp)
target_sources(lsm PRIVATE src/table/table.cpp)
target_sources(lsm PRIVATE src/version_set.cpp)
target_sources(lsm PRIVATE src/file_cleanup.cpp)
target_sources(lsm PRIVATE src/compaction.cpp)
target_sources(lsm PRIVATE src/iterator.cpp)
target_sources(lsm PRIVATE src/merging_iterator.cpp)
find_package(Threads REQUIRED)
target_link_libraries(lsm PUBLIC Threads::Threads)
if(WIN32)
  target_sources(lsm PRIVATE src/platform/windows.cpp src/platform/mapping_windows.cpp)
else()
  target_sources(lsm PRIVATE src/platform/posix.cpp src/platform/mapping_posix.cpp)
endif()
add_library(lsm::lsm ALIAS lsm)
target_include_directories(lsm PUBLIC include PRIVATE src)
lsm_strict_warnings(lsm)

include(FetchContent)
include(CTest)
if(BUILD_TESTING)
  set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
  set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
  FetchContent_Declare(googletest
    GIT_REPOSITORY https://github.com/google/googletest.git
    GIT_TAG 52eb8108c5bdec04579160ae17225d66034bd723 # v1.17.0
  )
  FetchContent_MakeAvailable(googletest)
  add_executable(lsm_tests
    tests/test_main.cpp
    tests/status_test.cpp
    tests/slice_test.cpp
    tests/crc32c_test.cpp
    tests/hash_test.cpp
    tests/varint_test.cpp
    tests/write_batch_test.cpp
    tests/memtable_test.cpp
    tests/platform_test.cpp
    tests/wal_test.cpp
    tests/db_test.cpp
    tests/block_test.cpp
    tests/block_iterator_test.cpp
    tests/bloom_test.cpp
    tests/mapping_test.cpp
    tests/table_test.cpp
    tests/version_set_test.cpp
    tests/flush_test.cpp
    tests/cleanup_test.cpp
    tests/compaction_test.cpp
    tests/iterator_test.cpp
    tests/merging_iterator_test.cpp
    tests/snapshot_test.cpp
    tests/block_cache_test.cpp
  )
  target_link_libraries(lsm_tests PRIVATE lsm::lsm GTest::gtest)
  target_include_directories(lsm_tests PRIVATE src tests)
  lsm_strict_warnings(lsm_tests)
  # Child process for python/crash_test.py.
  add_executable(lsm_crash_writer tests/crash_writer.cpp)
  target_link_libraries(lsm_crash_writer PRIVATE lsm::lsm)
  lsm_strict_warnings(lsm_crash_writer)
  add_test(NAME lsm_tests COMMAND lsm_tests)
  set_tests_properties(lsm_tests PROPERTIES TIMEOUT 1800)
  add_test(NAME seed_maximum
    COMMAND lsm_tests --seed=18446744073709551615 --gtest_filter=StatusTest.DefaultIsSuccess)
  foreach(bad_seed IN ITEMS empty text overflow)
    if(bad_seed STREQUAL "empty")
      set(seed_argument "--seed=")
    elseif(bad_seed STREQUAL "text")
      set(seed_argument "--seed=invalid")
    else()
      set(seed_argument "--seed=18446744073709551616")
    endif()
    add_test(NAME seed_reject_${bad_seed} COMMAND lsm_tests "${seed_argument}")
    # Match the parser's message so a crash or unrelated failure does not count as a pass.
    set_tests_properties(seed_reject_${bad_seed} PROPERTIES
      PASS_REGULAR_EXPRESSION "Invalid --seed" TIMEOUT 60)
  endforeach()
endif()

if(LSM_BUILD_BENCHMARKS)
  set(BENCHMARK_ENABLE_TESTING OFF CACHE BOOL "" FORCE)
  set(BENCHMARK_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
  FetchContent_Declare(googlebenchmark
    GIT_REPOSITORY https://github.com/google/benchmark.git
    GIT_TAG 31c66f47f60d06790a4885a261d682ba339bd100 # v1.9.4
  )
  FetchContent_MakeAvailable(googlebenchmark)
endif()
