// Catch2 v2 — single-file main for all tests
// Workaround: glibc ≥ 2.34 makes MINSIGSTKSZ a sysconf() call,
// which breaks Catch2 v2's constexpr sigStackSize declaration.
#define CATCH_CONFIG_NO_POSIX_SIGNALS
#define CATCH_CONFIG_MAIN
#include <catch2/catch.hpp>
