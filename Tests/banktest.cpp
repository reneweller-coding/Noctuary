/**
 * @file banktest.cpp
 * @brief ambient_banktest: the partial bank's checks, once per vector path.
 *
 * The partial bank's checks on their own (BankChecks.h, which the selftest runs as well), so that
 * every vector path of the bank is run and not only the one the desktop core is built with.
 * Tests/CMakeLists.txt builds this file three ways on x86 -- AVX2, through the NEON path on the
 * x86 shim, and scalar -- and the Android build of it is the real NEON path. Each variant says
 * which path it expects, and a variant that did not get it fails.
 *
 * The bank lives entirely in a header, so unlike the convolver's test there is no .cpp to compile
 * beside it: the variants differ only in what they are allowed to define and include.
 */
#include "ambient/Simd.h"
#include "ambient/GrainRing.h"
#include <cstdio>
#include <cstring>

using namespace ambient;

static int failures = 0;   ///< how many CHECKs failed so far; decides the exit code

/**
 * @brief Records one check: prints a FAIL line with the file and line and counts it when @p cond is false.
 *
 * BankChecks.h is written against this macro and is included right after it is defined.
 * @param cond  the condition that has to hold
 * @param msg   what was measured, as the FAIL line prints it
 */
#define CHECK(cond, msg) do { if (!(cond)) { std::printf("FAIL: %s (%s:%d)\n", msg, __FILE__, __LINE__); ++failures; } } while (0)

#include "BankChecks.h"

/**
 * @brief Names the vector path this build took, checks it is the one CMake asked for (AMBIENT_EXPECT_PATH),
 *        and runs the bank's and the grain ring's checks.
 * @return 0 when every check passed, 1 otherwise
 */
int main()
{
#if AMBIENT_HAS_AVX
    const char* path = "avx2";
#elif AMBIENT_HAS_NEON && defined(AMBIENT_NEON_SHIM)
    const char* path = "neon-shim";
#elif AMBIENT_HAS_NEON
    const char* path = "neon";
#else
    const char* path = "scalar";
#endif
    std::printf("partial bank path: %s, grain ring: %s\n", path, ringGrainPath());
#ifdef AMBIENT_EXPECT_PATH
    CHECK(std::strcmp(path, AMBIENT_EXPECT_PATH) == 0, "the build runs the vector path it was made for");
    // The grain ring says which path IT took as well: its check holds the vector path against the
    // scalar one, and that passes for nothing at all when the two are the same code.
    const char* want = std::strcmp(AMBIENT_EXPECT_PATH, "neon-shim") == 0 ? "neon" : AMBIENT_EXPECT_PATH;
    CHECK(std::strcmp(ringGrainPath(), want) == 0, "and the grain ring runs it too");
#endif
    bankChecks();
    if (failures == 0) std::printf("banktest: all checks passed\n");
    else std::printf("banktest: %d failures\n", failures);
    return failures == 0 ? 0 : 1;
}
