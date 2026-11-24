// Indicator lengths at the int limit. A length of 2,147,483,647 is a valid
// int, so the arithmetic around it (length + 1, left + right + 1,
// occurrence + 1) must not overflow. Each row feeds one or two values, so no
// window of that size is ever filled.
//
// Without an argument every row runs; with one argument only the row of that
// name runs.
#include <pineforge/na.hpp>
#include <pineforge/ta.hpp>

#include <climits>
#include <cstdio>
#include <cstring>

using namespace pineforge;

namespace {

int failures = 0;
const char* only_row = nullptr;

bool selected(const char* row) {
    if (only_row != nullptr && std::strcmp(only_row, row) != 0) return false;
    std::printf("row %s\n", row);
    std::fflush(stdout);
    return true;
}

void expect_value(const char* row, double got, double want) {
    if (!is_na(got) && got == want) {
        std::printf("PASS %s: %.17g\n", row, got);
    } else {
        std::printf("FAIL %s: got %.17g, want %.17g\n", row, got, want);
        ++failures;
    }
}

void expect_na(const char* row, double got) {
    if (is_na(got)) {
        std::printf("PASS %s: na\n", row);
    } else {
        std::printf("FAIL %s: got %.17g, want na\n", row, got);
        ++failures;
    }
}

}  // namespace

int main(int argc, char** argv) {
    if (argc > 1) only_row = argv[1];

    if (selected("ema")) {
        // alpha = 2 / (length + 1) = 2 / 2^31 = 2^-30, so one step from 0
        // towards 2^31 moves the average to 2^-30 * 2^31 = 2.
        ta::EMA ema(INT_MAX, ta::EmaSeeding::FirstValue);
        expect_value("ema seeds with the first value", ema.compute(0.0), 0.0);
        expect_value("ema first step", ema.compute(2147483648.0), 2.0);
    }
    if (selected("valuewhen")) {
        // Up to max_occurrence + 1 occurrences are kept, so the previous
        // occurrence is still there after the second one.
        ta::ValueWhen when(INT_MAX);
        expect_value("valuewhen occurrence 0", when.compute(true, 1.0, 0), 1.0);
        expect_value("valuewhen occurrence 1", when.compute(true, 2.0, 1), 1.0);
    }
    if (selected("mom")) {
        // length + 1 values are needed before the first value.
        ta::Mom mom(INT_MAX);
        expect_na("mom first bar", mom.compute(1.0));
        expect_na("mom first bar recalculated", mom.recompute(2.0));
    }
    if (selected("roc")) {
        ta::ROC roc(INT_MAX);
        expect_na("roc first bar", roc.compute(1.0));
        expect_na("roc first bar recalculated", roc.recompute(2.0));
    }
    if (selected("rising")) {
        ta::Rising rising(INT_MAX);
        expect_value("rising first bar", rising.compute(1.0), 0.0);
        expect_value("rising first bar recalculated", rising.recompute(2.0), 0.0);
    }
    if (selected("falling")) {
        ta::Falling falling(INT_MAX);
        expect_value("falling first bar", falling.compute(1.0), 0.0);
        expect_value("falling first bar recalculated", falling.recompute(2.0), 0.0);
    }
    if (selected("percentrank")) {
        ta::PercentRank rank(INT_MAX);
        expect_na("percentrank first bar", rank.compute(1.0));
        expect_na("percentrank first bar recalculated", rank.recompute(2.0));
    }
    if (selected("pivothigh")) {
        // left + right + 1 values are needed before a pivot can be confirmed.
        ta::PivotHigh pivot(INT_MAX, 1);
        expect_na("pivothigh first bar", pivot.compute(1.0));
        expect_na("pivothigh first bar recalculated", pivot.recompute(2.0));
    }
    if (selected("pivotlow")) {
        ta::PivotLow pivot(INT_MAX, 1);
        expect_na("pivotlow first bar", pivot.compute(1.0));
        expect_na("pivotlow first bar recalculated", pivot.recompute(2.0));
    }
    if (selected("valuewhen-negative")) {
        // Occurrences are numbered from 0 back in time; there is none at -1.
        ta::ValueWhen when(1);
        expect_na("valuewhen occurrence -1 before any occurrence",
                  when.compute(false, 1.0, -1));
        expect_na("valuewhen occurrence -1 after one occurrence",
                  when.compute(true, 2.0, -1));
    }
    return failures == 0 ? 0 : 1;
}
