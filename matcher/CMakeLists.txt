# Standalone build for the matcher plugin (Linux/WSL; needs llvm-dev).
# Not wired into the root CMakeLists on purpose: the library builds on MSVC,
# the matcher needs an LLVM dev environment — separate concerns, separate
# builds.
#
#   cmake -S matcher -B matcher/build -DCMAKE_BUILD_TYPE=Release
#   cmake --build matcher/build
#   opt -load-pass-plugin=matcher/build/SopMatcher.so \
#       -passes=loop-simplify,lcssa,sop-matcher -disable-output module.bc
#
# The two canonicalization passes are required, not optional: see the header
# of SumOfProductsMatcher.cpp. Without them every loop is skipped with a WARN.
cmake_minimum_required(VERSION 3.20)
project(SopMatcher CXX)

find_package(LLVM REQUIRED CONFIG)
message(STATUS "Found LLVM ${LLVM_PACKAGE_VERSION} at ${LLVM_DIR}")

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_library(SopMatcher MODULE SumOfProductsMatcher.cpp)
target_include_directories(SopMatcher PRIVATE ${LLVM_INCLUDE_DIRS})
target_compile_definitions(SopMatcher PRIVATE ${LLVM_DEFINITIONS})
# Plugins resolve LLVM symbols from the opt binary; do not link libLLVM.
# -fno-rtti matches LLVM's default build and avoids typeinfo link errors.
target_compile_options(SopMatcher PRIVATE -fno-rtti)
set_target_properties(SopMatcher PROPERTIES PREFIX "")
