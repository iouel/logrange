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

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

# PROJECT_IS_TOP_LEVEL is 3.21; this project targets 3.20.
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  set(_lr_top_level ON)
else()
  set(_lr_top_level OFF)
endif()

option(LOGRANGE_BUILD_TESTING "Build LogRange's tests and benchmark"
       ${_lr_top_level})
option(LOGRANGE_PROPAGATE_FP_FLAGS
       "Give consumers the FP flag the error contract depends on" ON)

# ---------------------------------------------------------------------------
# The library. Header-only, so everything is INTERFACE.
# ---------------------------------------------------------------------------
add_library(logrange INTERFACE)
add_library(LogRange::logrange ALIAS logrange)

target_include_directories(logrange INTERFACE
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)

# Consumers get C++17 from the target rather than having to know to set it.
target_compile_features(logrange INTERFACE cxx_std_17)

# The FP flag is part of the contract, not a style preference. rp_accum's
# Neumaier compensation is written statement-per-step so the rounding error
# it recovers is not fused away; with FMA contraction enabled the compiler
# may contract those steps and the compensation degrades. Measured on the
# heavy-cancellation scenario: 2.665e-09 with contraction off, 3.707e-08
# with -ffp-contract=fast -mfma. Still inside the stated bound, but 14x
# worse — and silently, which is the problem. x86-64 without FMA in the
# baseline hides this; -march=native or any ARM target does not.
#
# Set LOGRANGE_PROPAGATE_FP_FLAGS=OFF to opt out; then the error contract is
# yours to re-establish.
if(LOGRANGE_PROPAGATE_FP_FLAGS)
  target_compile_options(logrange INTERFACE
    $<$<AND:$<COMPILE_LANGUAGE:CXX>,$<OR:$<CXX_COMPILER_ID:GNU>,$<CXX_COMPILER_ID:Clang>,$<CXX_COMPILER_ID:AppleClang>>>:-ffp-contract=off>
    $<$<AND:$<COMPILE_LANGUAGE:CXX>,$<CXX_COMPILER_ID:MSVC>>:/fp:precise>)
endif()

# ---------------------------------------------------------------------------
# Install / package. Header-only, so this is headers + a config package.
# ---------------------------------------------------------------------------
install(TARGETS logrange EXPORT LogRangeTargets)
install(DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/include/"
        DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}")
install(EXPORT LogRangeTargets
        FILE LogRangeTargets.cmake
        NAMESPACE LogRange::
        DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/LogRange")

configure_package_config_file(
  "${CMAKE_CURRENT_SOURCE_DIR}/cmake/LogRangeConfig.cmake.in"
  "${CMAKE_CURRENT_BINARY_DIR}/LogRangeConfig.cmake"
  INSTALL_DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/LogRange")

# SameMinorVersion, not SameMajorVersion: pre-1.0 the error contract can move
# between minor versions and has (CHANGELOG.md). Treating 0.2 and 0.9 as
# interchangeable would be exactly the wrong promise.
write_basic_package_version_file(
  "${CMAKE_CURRENT_BINARY_DIR}/LogRangeConfigVersion.cmake"
  VERSION ${PROJECT_VERSION}
  COMPATIBILITY SameMinorVersion)

install(FILES
  "${CMAKE_CURRENT_BINARY_DIR}/LogRangeConfig.cmake"
  "${CMAKE_CURRENT_BINARY_DIR}/LogRangeConfigVersion.cmake"
  DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/LogRange")

if(NOT LOGRANGE_BUILD_TESTING)
  return()
endif()

# ---------------------------------------------------------------------------
# Our own build flags. Deliberately NOT on the `logrange` target: a consumer
# who does add_subdirectory() here must not inherit -Werror. Only the FP flag
# above is contractual and therefore public.
# ---------------------------------------------------------------------------
add_library(logrange_build_flags INTERFACE)
if(MSVC)
  target_compile_options(logrange_build_flags INTERFACE /W4 /WX /permissive-)
else()
  target_compile_options(logrange_build_flags INTERFACE -Wall -Wextra -Werror)
endif()

enable_testing()

foreach(_lr_test log_math rp_accum pos_accum accuracy)
  add_executable(test_${_lr_test} tests/test_${_lr_test}.cpp)
  target_link_libraries(test_${_lr_test} PRIVATE logrange logrange_build_flags)
  add_test(NAME ${_lr_test} COMMAND test_${_lr_test})
endforeach()

# Adversarial search against the error contract. test_accuracy checks fixed
# scenarios, which can only fail to refute a universal claim; this one hunts
# for a counterexample. Kept as a test so a bound change cannot land quietly.
add_executable(bound_search tests/bound_search.cpp)
target_link_libraries(bound_search PRIVATE logrange logrange_build_flags)
add_test(NAME bound_search COMMAND bound_search)

# Benchmark harness (intent Deliverable 1 / First Action step 4). Not a test:
# timing runs are manual and only meaningful in the Release config (/O2).
# Inherits the same flags deliberately — the harness measures the library as
# shipped, not a fast-math variant of it.
add_executable(bench_logrange bench/bench_main.cpp)
target_link_libraries(bench_logrange PRIVATE logrange logrange_build_flags)
