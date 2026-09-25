#include <pineforge/pineforge.h>
#include <stdio.h>
#include <string.h>

#ifndef PF_SMOKE_PACKAGE_VERSION_FULL
#error "build through cmake/smoke_consumer/CMakeLists.txt, which passes the package's PineForge_VERSION_FULL"
#endif

/* 1 when this TU fused x * x - p into one multiply-add. x * x - p is then the
 * rounding error of x * x (2^-60 here); rounded twice, as TradingView's runtime
 * and libpineforge round it, it is 0. x and y are two reads of one volatile,
 * so no compiler can share a single product between p and the subtraction.
 * Only a target with an FMA instruction (ARM64, x86-64 with FMA enabled) can
 * tell the two apart. */
static int fused_multiply_add(void)
{
    volatile double in = 1.0 + 0x1p-30;
    double x = in;
    double y = in;
    double p = y * y;
    return x * x - p != 0.0;
}

int main(void)
{
    if (fused_multiply_add()) {
        fprintf(stderr, "smoke consumer: a multiply-add was fused; "
                        "PineForge's -ffp-contract=off did not reach this TU\n");
        return 1;
    }
    /* One version, three places: the CMake package (PineForge_VERSION_FULL),
     * the installed <pineforge/version.h> (PINEFORGE_VERSION_FULL) and the
     * linked library (pf_version_string()), a release candidate's -rc.N
     * included; pf_version_get() is its MAJOR.MINOR.PATCH. */
    const char* full = pf_version_string();
    pf_version_t v = pf_version_get();
    char mmp[64];
    size_t mmp_length;
    if (!full || strcmp(full, PINEFORGE_VERSION_FULL) != 0
        || strcmp(full, PF_SMOKE_PACKAGE_VERSION_FULL) != 0) {
        fprintf(stderr, "smoke consumer: library %s, header %s and package %s "
                        "name different versions\n",
                full ? full : "(null)", PINEFORGE_VERSION_FULL,
                PF_SMOKE_PACKAGE_VERSION_FULL);
        return 1;
    }
    snprintf(mmp, sizeof(mmp), "%d.%d.%d", (int)v.major, (int)v.minor, (int)v.patch);
    mmp_length = strlen(mmp);
    if (v.major != PINEFORGE_VERSION_MAJOR || v.minor != PINEFORGE_VERSION_MINOR
        || v.patch != PINEFORGE_VERSION_PATCH || strncmp(full, mmp, mmp_length) != 0
        || (full[mmp_length] != '\0' && full[mmp_length] != '-')) {
        fprintf(stderr, "smoke consumer: pf_version_get() %s is not the "
                        "MAJOR.MINOR.PATCH of %s\n", mmp, full);
        return 1;
    }
    printf("%s\n", full);
    return 0;
}
