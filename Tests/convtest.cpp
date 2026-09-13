// The convolution room's checks on their own (ConvolverChecks.h, which the selftest runs as well),
// so that every vector path of the convolver is run and not only the one the desktop core is built
// with. Tests/CMakeLists.txt builds this file three ways on x86 -- with NoctuaryCore (AVX2), through
// the NEON path on the x86 shim, and scalar -- and the Android build of it is the real NEON path.
// Each variant says which path it expects, and a variant that did not get it fails.
#include "ambient/Convolution.h"
#include "ambient/Dsp.h"
#include "ambient/Simd.h"
#include <cstdio>
#include <cstring>

using namespace ambient;

static int failures = 0;
#define CHECK(cond, msg) do { if (!(cond)) { std::printf("FAIL: %s (%s:%d)\n", msg, __FILE__, __LINE__); ++failures; } } while (0)

#include "ConvolverChecks.h"

int main()
{
#if AMBIENT_HAS_AVX
    const char* path = "avx2";
#elif AMBIENT_HAS_NEON && defined(AMBIENT_NEON_SHIM) && defined(AMBIENT_NEON_SHIM_NOFMA)
    const char* path = "neon-shim-nofma";
#elif AMBIENT_HAS_NEON && defined(AMBIENT_NEON_SHIM)
    const char* path = "neon-shim";
#elif AMBIENT_HAS_NEON
    const char* path = "neon";
#else
    const char* path = "scalar";
#endif
    std::printf("convolver path: %s\n", path);
#ifdef AMBIENT_EXPECT_PATH
    CHECK(std::strcmp(path, AMBIENT_EXPECT_PATH) == 0, "the build runs the vector path it was made for");
#endif
    convolverChecks();
    if (failures == 0) std::printf("convtest: all checks passed\n");
    else std::printf("convtest: %d failures\n", failures);
    return failures == 0 ? 0 : 1;
}
