#pragma once

#include <pineforge/ta.hpp>

#include <cmath>
#include <cstdio>
#include <initializer_list>

namespace ta_na_cases {

inline int failures = 0;

inline void expect(const char* indicator, const char* transition, const char* bar,
                   double got, double want) {
    const bool equal = pineforge::is_na(want) ? pineforge::is_na(got)
                                              : std::fabs(got - want) <= 1e-12;
    std::printf("%s %s %s %s: got %.17g, want %.17g\n",
                equal ? "PASS" : "FAIL", indicator, transition, bar, got, want);
    if (!equal) ++failures;
}

// A missing input consumes no sample. Each case starts with the same earlier
// bars and checks both the replacement and the first bar that inherits it.
template<class Make>
void transitions(const char* name, Make make, std::initializer_list<double> earlier,
                 double value, double next, double missing_result,
                 double accepted, double after_accepted, double after_missing) {
    const double missing = pineforge::na<double>();
    const auto row = [&](const char* label, double first, double final,
                         double want, double want_next) {
        auto indicator = make();
        for (double prior : earlier) indicator.compute(prior);
        indicator.compute(first);
        expect(name, label, "replacement", indicator.recompute(final), want);
        expect(name, label, "next", indicator.compute(next), want_next);
    };
    row("na->value", missing, value, accepted, after_accepted);
    row("value->na", value, missing, missing_result, after_missing);
    row("na->na", missing, missing, missing_result, after_missing);
}

} // namespace ta_na_cases
