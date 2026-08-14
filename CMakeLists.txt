cmake_minimum_required(VERSION 3.20)
project(LogRange VERSION 0.1 LANGUAGES CXX)

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
