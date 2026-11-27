// A change whose largest length is -1 asks for a history of no entries. On an
// object that holds none there is then no slot to store a value in: the call
// answers na, stores nothing, and a later call with a real length starts a
// fresh history.
#include "ta_invalid_length_cases.hpp"

namespace {

// The history is private: read how many slots it owns.
template<class Tag, auto Field>
struct Access { friend auto member(Tag) { return Field; } };
struct History { friend auto member(History); };
template struct Access<History, &pineforge::ta::Change::history_>;

void row(int max_length, const char* label) {
    using namespace ta_invalid_length;
    pineforge::ta::Change delta(max_length);
    const auto slots = [&delta] { return (delta.*member(History{})).size(); };

    calling("change", label, "compute(4, -1)");
    expect_na("change", label, "compute(4, -1)", delta.compute(4.0, -1));
    expect_nothing_held("change", label, "after compute(4, -1)", slots());

    calling("change", label, "compute(6.5, -1)");
    expect_na("change", label, "compute(6.5, -1)", delta.compute(6.5, -1));
    calling("change", label, "recompute(5.25, -1)");
    expect_na("change", label, "recompute(5.25, -1)", delta.recompute(5.25, -1));
    expect_nothing_held("change", label, "after three calls", slots());

    // The same object with a length of 1: the first value has no predecessor,
    // the second is compared with the first. 9.5 - 7 is exact.
    calling("change", label, "compute(7, 1)");
    expect_na("change", label, "compute(7, 1)", delta.compute(7.0, 1));
    calling("change", label, "compute(9.5, 1)");
    const double step = delta.compute(9.5, 1);
    const bool ok = step == 2.5;
    std::printf("%s change %s compute(9.5, 1): got %.17g, want 2.5\n",
                ok ? "PASS" : "FAIL", label, step);
    std::fflush(stdout);
    if (!ok) ++failures;
}

}  // namespace

int main() {
    // Both spellings of a largest length of -1: the constructor's own -1, and
    // a smaller constructor length under a call length of -1.
    row(-1, "max length -1");
    row(-5, "max length -5");
    return ta_invalid_length::result();
}
