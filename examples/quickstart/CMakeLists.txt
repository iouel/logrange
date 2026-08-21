# A consumer of the INSTALLED package, built standalone against a prefix.
#
# This is not part of the main build. It exists to prove the packaging works
# the way the README says it does: find_package, one target, no include-path
# archaeology. CI installs the library to a temp prefix and builds this.
cmake_minimum_required(VERSION 3.20)
project(logrange_quickstart LANGUAGES CXX)

# CONFIG mode only: there is no FindLogRange.cmake and there should not be.
# The version request is deliberately exact-minor — pre-1.0 the error
# contract can move between minor versions, and the package's
# SameMinorVersion compatibility rule will reject a mismatch here.
find_package(LogRange 1.0 CONFIG REQUIRED)

add_executable(quickstart quickstart.cpp)
target_link_libraries(quickstart PRIVATE LogRange::logrange)

# Registered as a test so CI can run it the same way on every platform,
# without knowing where the generator puts the executable.
enable_testing()
add_test(NAME quickstart COMMAND quickstart)
