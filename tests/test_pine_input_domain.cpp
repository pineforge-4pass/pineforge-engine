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
        if (pineforge::source::modeled_pine_input(provenance)
            != (provenance != NativePriceProvenance::ObservedPrint)) ++failures;
    }
    std::printf("input-domain: 9 checks, %d failures\n", failures);
    return failures == 0 ? 0 : 1;
}
