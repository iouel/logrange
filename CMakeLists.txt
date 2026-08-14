cmake_minimum_required(VERSION 3.20)

# Version identity lives in the header (LOGRANGE_VERSION_* in
# include/logrange/log_math.h) so a vendored copy carries its own version.
# Parsed here rather than duplicated: the previous hardcoded 0.1 had already
# drifted a release behind the header.
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/include/logrange/log_math.h" _lr_header)
foreach(_lr_part MAJOR MINOR PATCH)
  if(NOT _lr_header MATCHES "#define LOGRANGE_VERSION_${_lr_part} +([0-9]+)")
    message(FATAL_ERROR
      "LOGRANGE_VERSION_${_lr_part} not found in include/logrange/log_math.h")
  endif()
  set(_lr_${_lr_part} "${CMAKE_MATCH_1}")
endforeach()
unset(_lr_header)

project(LogRange VERSION ${_lr_MAJOR}.${_lr_MINOR}.${_lr_PATCH} LANGUAGES CXX)
message(STATUS "LogRange version ${PROJECT_VERSION} (from log_math.h)")

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Header-only library target
add_library(logrange INTERFACE)
add_library(LogRange::logrange ALIAS logrange)
target_include_directories(logrange INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/include)

# Determinism: precise FP everywhere. (Predecessor lesson: v0.1 specced
# /fp:fast on a library claiming determinism. Not repeating that.)
if(MSVC)
  add_compile_options(/W4 /WX /fp:precise /permissive-)
else()
  add_compile_options(-Wall -Wextra -Werror -ffp-contract=off)
endif()

enable_testing()

add_executable(test_log_math tests/test_log_math.cpp)
target_link_libraries(test_log_math PRIVATE logrange)
add_test(NAME log_math COMMAND test_log_math)

add_executable(test_rp_accum tests/test_rp_accum.cpp)
target_link_libraries(test_rp_accum PRIVATE logrange)
add_test(NAME rp_accum COMMAND test_rp_accum)

add_executable(test_pos_accum tests/test_pos_accum.cpp)
target_link_libraries(test_pos_accum PRIVATE logrange)
add_test(NAME pos_accum COMMAND test_pos_accum)

add_executable(test_accuracy tests/test_accuracy.cpp)
target_link_libraries(test_accuracy PRIVATE logrange)
add_test(NAME accuracy COMMAND test_accuracy)

# Benchmark harness (intent Deliverable 1 / First Action step 4). Not a test:
# timing runs are manual and only meaningful in the Release config (/O2).
# Inherits the global /W4 /WX /fp:precise flags deliberately — the harness
# measures the library as shipped, not a fast-math variant of it.
add_executable(bench_logrange bench/bench_main.cpp)
target_link_libraries(bench_logrange PRIVATE logrange)
