#include <pineforge/source/pine_input_domain.hpp>

#include <array>
#include <cstdio>

int main() {
    using pineforge::NativePriceProvenance;
    const std::array<NativePriceProvenance, 9> provenances{{
        NativePriceProvenance::Confirmed, NativePriceProvenance::ObservedPrint,
        NativePriceProvenance::ModeledOHLCOpen, NativePriceProvenance::ModeledOHLCClose,
        NativePriceProvenance::CarriedOpen, NativePriceProvenance::AfterCalculationClose,
        NativePriceProvenance::PartialFinalized, NativePriceProvenance::Calculation,
        NativePriceProvenance::CurrentExecution}};
    int failures = 0;
    for (const auto provenance : provenances) {
        if (!pineforge::source::modeled_pine_input(false, provenance)) ++failures;
        if (pineforge::source::modeled_pine_input(true, provenance)
            != (provenance != NativePriceProvenance::ObservedPrint)) ++failures;
    }
    std::printf("input-domain: 18 checks, %d failures\n", failures);
    return failures == 0 ? 0 : 1;
}
