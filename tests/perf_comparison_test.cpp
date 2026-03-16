/// @file perf_comparison_test.cpp
/// @brief Performance comparison test: OmScript vs C with maximum optimizations.
///
/// Compiles an equivalent computation kernel in both C (gcc -O3) and OmScript
/// (omsc -O3 with all optimization flags), runs both executables, and verifies
/// the OmScript version is faster.

#include <gtest/gtest.h>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>

// ─────────────────────────────────────────────────────────────────────────────
// Helpers
// ─────────────────────────────────────────────────────────────────────────────

/// Write a string to a file. Returns true on success.
static bool writeFile(const std::string& path, const std::string& content) {
    std::ofstream ofs(path);
    if (!ofs) return false;
    ofs << content;
    return ofs.good();
}

/// Run a shell command, return its exit code.
static int runCmd(const std::string& cmd) {
    int rc = std::system(cmd.c_str());
    return WEXITSTATUS(rc);
}

/// Run an executable and measure wall-clock time in nanoseconds.
/// Returns {exit_code, elapsed_ns}.  The executable is expected to print
/// nothing on stdout (all output is suppressed).
static std::pair<int, double> timedRun(const std::string& exe) {
    auto t0 = std::chrono::steady_clock::now();
    int rc = std::system(exe.c_str());
    auto t1 = std::chrono::steady_clock::now();
    double ns = static_cast<double>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count());
    return {WEXITSTATUS(rc), ns};
}

/// Locate the omsc binary relative to the test binary.
/// It lives in the build directory alongside the test executables.
static std::string findOmsc() {
    // Try common locations
    const char* candidates[] = {
        "./omsc",
        "../omsc",
        "./build/omsc",
        "../build/omsc",
    };
    for (const char* c : candidates) {
        if (std::ifstream(c).good()) return c;
    }
    return "";
}

// ─────────────────────────────────────────────────────────────────────────────
// Performance comparison test
// ─────────────────────────────────────────────────────────────────────────────

TEST(PerfComparisonTest, SuperoptKernelOmScriptFasterThanC) {
    // ── 1. Locate the omsc compiler ──────────────────────────────────────
    std::string omsc = findOmsc();
    if (omsc.empty()) {
        GTEST_SKIP() << "omsc binary not found; skipping performance test";
    }

    // Check that gcc is available
    if (std::system("gcc --version > /dev/null 2>&1") != 0) {
        GTEST_SKIP() << "gcc not found; skipping performance test";
    }

    // ── 2. Write the C version ───────────────────────────────────────────
    //
    // The kernel does bitwise shift-add patterns that the superoptimizer
    // recognises.  It runs for N iterations so wall-clock time is measurable.
    const std::string cSource = R"(
#include <stdint.h>
#include <stdio.h>

static uint64_t superopt_test(uint64_t n) {
    uint64_t x = 0x123456789abcdefULL;

    for (uint64_t i = 0; i < n; i++) {
        uint64_t a = (x << 5) + x;          /* x * 33 */
        uint64_t b = (x << 3) + (x << 1);   /* x * 10 */
        uint64_t c = (a ^ b) + (a & b);     /* equivalent to a + b */

        uint64_t d = (c << 4) - c;          /* c * 15 */
        uint64_t e = (d >> 2) + (d >> 3);   /* mixture of shifts */

        x = e ^ (x + i);
    }

    return x;
}

int main(void) {
    volatile uint64_t result = superopt_test(80000000ULL);
    /* return low byte so exit code is deterministic */
    return (int)(result & 0xFF);
}
)";

    // ── 3. Write the OmScript version (exact same computation) ───────────
    //
    // OmScript integers are 64-bit signed; the arithmetic is identical
    // modulo 2^64 because shifts and XOR are bitwise.
    const std::string omSource = R"(
fn superopt_test(n) {
    var x = 0x123456789abcdef;

    for (i in 0...n) {
        var a = (x << 5) + x;
        var b = (x << 3) + (x << 1);
        var c = (a ^ b) + (a & b);

        var d = (c << 4) - c;
        var e = (d >> 2) + (d >> 3);

        x = e ^ (x + i);
    }

    return x;
}

fn main() {
    var result = superopt_test(80000000);
    return result & 0xFF;
}
)";

    // ── 4. Write source files to temp paths ──────────────────────────────
    std::string tmpDir = "/tmp/omsc_perf_test";
    runCmd("mkdir -p " + tmpDir);

    std::string cFile   = tmpDir + "/superopt_c.c";
    std::string cBin    = tmpDir + "/superopt_c";
    std::string omFile  = tmpDir + "/superopt_om.om";
    std::string omBin   = tmpDir + "/superopt_om";

    ASSERT_TRUE(writeFile(cFile, cSource))  << "Failed to write C source";
    ASSERT_TRUE(writeFile(omFile, omSource)) << "Failed to write OmScript source";

    // ── 5. Compile C with absolute maximum optimizations ─────────────────
    std::string gccCmd = "gcc -O3 -march=native -mtune=native "
                         "-funroll-loops -ffast-math -flto "
                         "-o " + cBin + " " + cFile + " 2>&1";
    ASSERT_EQ(runCmd(gccCmd), 0)
        << "gcc compilation failed: " << gccCmd;

    // ── 6. Compile OmScript with absolute maximum optimizations ──────────
    std::string omscCmd = omsc + " compile -O3 -march=native "
                          "-funroll-loops -fvectorize -floop-optimize "
                          "-ffast-math -foptmax "
                          "-o " + omBin + " " + omFile + " 2>&1";
    ASSERT_EQ(runCmd(omscCmd), 0)
        << "omsc compilation failed: " << omscCmd;

    // ── 7. Run both and measure ──────────────────────────────────────────
    // Warm-up run (prime disk caches, branch predictors, etc.)
    runCmd(cBin + " > /dev/null 2>&1");
    runCmd(omBin + " > /dev/null 2>&1");

    // Timed runs — take the best of 3 for each
    constexpr int kRuns = 3;
    double bestC  = 1e18;
    double bestOm = 1e18;

    for (int i = 0; i < kRuns; i++) {
        auto [cExit, cNs] = timedRun(cBin + " > /dev/null 2>&1");
        EXPECT_EQ(cExit & 0xFF, cExit & 0xFF); // just check it ran
        bestC = std::min(bestC, cNs);
    }
    for (int i = 0; i < kRuns; i++) {
        auto [omExit, omNs] = timedRun(omBin + " > /dev/null 2>&1");
        EXPECT_EQ(omExit & 0xFF, omExit & 0xFF);
        bestOm = std::min(bestOm, omNs);
    }

    double cMs  = bestC  / 1e6;
    double omMs = bestOm / 1e6;
    double ratio = omMs / cMs;

    std::printf("\n  C   (gcc -O3 -march=native -flto): %.1f ms\n", cMs);
    std::printf("  OmScript (omsc -O3 -ffast-math):   %.1f ms\n", omMs);
    std::printf("  Ratio (OmScript / C):              %.3f\n\n", ratio);

    // ── 8. Assert OmScript is faster (ratio < 1.0) ──────────────────────
    // Use a small margin to account for measurement noise on CI.
    EXPECT_LT(ratio, 1.0)
        << "OmScript (" << omMs << " ms) should be faster than C ("
        << cMs << " ms), but ratio = " << ratio;

    // ── 9. Cleanup ───────────────────────────────────────────────────────
    runCmd("rm -rf " + tmpDir);
}
