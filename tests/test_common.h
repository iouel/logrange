#pragma once
#include <cstdio>
#include <cstdlib>

#define NC_CHECK(cond) do { \
  if (!(cond)) { \
    std::fprintf(stderr, "check failed: %s (%s:%d)\n", #cond, __FILE__, __LINE__); \
    std::exit(1); \
  } \
} while (0)
