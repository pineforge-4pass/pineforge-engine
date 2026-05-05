#include <pineforge/pineforge.h>
#include <stdio.h>

int main(void)
{
    pf_version_t v = pf_version_get();
    printf("%d.%d.%d\n", (int)v.major, (int)v.minor, (int)v.patch);
    return 0;
}
