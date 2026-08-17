# Standalone build for the log-rewrite plugin (Linux/WSL; needs llvm-dev).
# Same pattern as matcher/CMakeLists.txt, and deliberately not wired into the
# root CMakeLists: the library builds on MSVC, the pass needs an LLVM dev
# environment — separate concerns, separate builds.
#
#   CMAKE_PREFIX_PATH=/usr/lib/llvm-21 \
#     cmake -S pass -B <builddir> -DCMAKE_BUILD_TYPE=Release
#   cmake --build <builddir>
#   opt-21 -load-pass-plugin=<builddir>/LogRewrite.so \
#          -passes='loop-simplify,lcssa,log-rewrite<force>' -S module.ll -o rewritten.ll
#
# The two canonicalization passes are required, not optional: ELIGIBILITY.md
# section 0. Without them the pass declines every loop and reports nothing.
cmake_minimum_required(VERSION 3.20)
project(LogRewrite CXX)

find_package(LLVM REQUIRED CONFIG)
message(STATUS "Found LLVM ${LLVM_PACKAGE_VERSION} at ${LLVM_DIR}")

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_library(LogRewrite MODULE LogRewritePass.cpp)
target_include_directories(LogRewrite PRIVATE ${LLVM_INCLUDE_DIRS})
target_compile_definitions(LogRewrite PRIVATE ${LLVM_DEFINITIONS})
# Plugins resolve LLVM symbols from the opt binary; do not link libLLVM.
# -fno-rtti matches LLVM's default build and avoids typeinfo link errors.
target_compile_options(LogRewrite PRIVATE -fno-rtti)
set_target_properties(LogRewrite PROPERTIES PREFIX "")
