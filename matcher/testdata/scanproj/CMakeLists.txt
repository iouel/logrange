# Fixture project for test_scan.sh. Its only job is to make cmake emit a real
# compile_commands.json, so the front door is tested against a generator's
# output rather than a hand-written file.
#
# The source is the matcher's own labeled ground truth, so case 1 asserts the
# same 7-loops/5-hits/2-HIGH table that run_study.sh selftest asserts on the
# raw records. Whatever compiler cmake picks here is irrelevant: the scan
# recompiles every unit with clang at the study's flags (METHODOLOGY.md).
cmake_minimum_required(VERSION 3.20)
project(logrange_scan_fixture C)
add_library(scanfixture STATIC ${CMAKE_CURRENT_SOURCE_DIR}/../../selftest.c)
