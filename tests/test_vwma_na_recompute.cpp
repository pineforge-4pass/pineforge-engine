#include "ta_na_recompute_cases.hpp"

int main() {
    using ta_na_cases::expect;
    const double missing = pineforge::na<double>();
    const auto row = [&](const char* label, double src, double vol, double final_src,
                         double final_vol, double want, double want_next) {
        pineforge::ta::VWMA average(2);
        average.compute(2, 1);
        average.compute(8, 3);
        average.compute(src, vol);
        expect("vwma", label, "replacement", average.recompute(final_src, final_vol), want);
        expect("vwma", label, "next", average.compute(4, 1), want_next);
    };
    // The newest two complete pairs give (24+12)/5, (12+4)/3, or (24+4)/4.
    row("na->value", 6, missing, 6, 2, 36.0 / 5, 16.0 / 3);
    row("value->na", 6, 2, missing, 2, missing, 7);
    row("na->na", missing, 2, 6, missing, missing, 7);

    pineforge::ta::VWMA average(2);
    average.compute(1, 1);
    average.compute(1, 1);
    average.compute(0x1p54, 1);
    // 2 + 2^54 rounds to 2^54; subtracting 1 rounds to 2^54 again.
    // The established finite replacement therefore gives (2^54 - 2^54 + 3)/2.
    expect("vwma", "value->value", "replacement", average.recompute(3, 1), 1.5);
    average.recompute(missing, 1);
    // Replaying from the two earlier ones instead gives (2 + 3 - 1)/2.
    expect("vwma", "value->na->value", "replacement", average.recompute(3, 1), 2);
    average.recompute(0x1p54, 1);
    expect("vwma", "after gap", "repeated replacement", average.recompute(3, 1), 2);
    return ta_na_cases::failures != 0;
}
