#include <pineforge/pineforge.h>
#include <stdio.h>

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
    pf_version_t v = pf_version_get();
    printf("%d.%d.%d\n", (int)v.major, (int)v.minor, (int)v.patch);
    return 0;
}
