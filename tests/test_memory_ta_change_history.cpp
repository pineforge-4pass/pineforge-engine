// Change-history contents follow the configured lookback and the current call length.
// Change has no fixed history ceiling. These checks concern live elements,
// not deque allocation bytes, and do not claim codegen emits a series length.
#include <pineforge/ta.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <exception>
#include <string>
#include <vector>

namespace memory_ta_change_history {
using pineforge::ta::Change;

// Repository test-side access pattern: observe the actual private deque,
// without a product friend, accessor, layout mirror or substitute algorithm.
template<class Tag, auto Member> struct Access {
    friend auto access(Tag) { return Member; }
};
struct History { friend auto access(History); };
template struct Access<History, &Change::history>;

std::size_t checks = 0;
std::size_t failures = 0;
std::size_t current_step = 0;
std::string trial;

void check(bool passed, const char* expression, int line) {
    ++checks;
    if (!passed) {
        ++failures;
        std::fprintf(stderr, "FAIL [change-history] %s:%d: %s (trial=%s step=%zu)\n",
                     __FILE__, line, expression, trial.c_str(), current_step);
    }
}
#define MEM_CHECK(expression) check(bool(expression), #expression, __LINE__)

template<class Action>
void run_trial(const char* name, Action&& action) {
    trial = name;
    current_step = 0;
    try {
        action();
    } catch (const std::exception& error) {
        check(false, error.what(), __LINE__);
    } catch (...) {
        check(false, "unknown exception", __LINE__);
    }
}

// An append-only tape and a monotonically advancing first-live index form the
// oracle. It never reads a product count/value to choose its expected state.
// At input index j, the constructor horizon needs j-M through j, while the
// current length needs j-L through j. Their union starts at the earlier index;
// previously retired indices cannot become live again when a length widens.
class TapeModel {
    Change change_;
    const std::size_t constructor_lag_;
    std::vector<double> tape_;
    std::size_t first_live_ = 0;

public:
    explicit TapeModel(int constructor_lag)
        : change_(constructor_lag),
          constructor_lag_(static_cast<std::size_t>(constructor_lag)) {}

    void step(int length, bool compare_all = true) {
        const std::size_t j = tape_.size();
        current_step = j + 1;
        // Distinct finite, exactly representable inputs expose insert/order
        // errors without tolerance comparisons or a NaN equality convention.
        const double value = static_cast<double>(3 * j)
                           + static_cast<double>(j % 7) / 8.0;
        tape_.push_back(value);
        const auto lag = static_cast<std::size_t>(length);
        const auto constructor_first = j > constructor_lag_ ? j - constructor_lag_ : 0;
        const auto length_first = j > lag ? j - lag : 0;
        const auto required_first = constructor_first < length_first
                                  ? constructor_first : length_first;
        if (required_first > first_live_) first_live_ = required_first;
        const auto expected_count = tape_.size() - first_live_;

        // Real insert/retire path through ta::Change::compute.
        const double result = change_.compute(value, length);
        const auto& history = change_.*access(History{});
        MEM_CHECK(history.size() == expected_count);
        // Direct independent evidence that this call reached the push_back.
        // Guard back() so a missing insertion can still report failures.
        MEM_CHECK(!history.empty() && history.back() == value);
        if (compare_all) {
            MEM_CHECK(std::equal(history.begin(), history.end(),
                                tape_.begin() + static_cast<std::ptrdiff_t>(first_live_),
                                tape_.end()));
        }
        if (lag >= expected_count) {
            MEM_CHECK(std::isnan(result));
        } else {
            MEM_CHECK(result == value - tape_[j - lag]);
        }
    }

    std::size_t size() const { return (change_.*access(History{})).size(); }
};

std::size_t fixed_history(int constructor_lag, int length, std::size_t bars) {
    TapeModel model(constructor_lag);
    for (std::size_t j = 0; j < bars; ++j) model.step(length);
    // Each lag D needs D past samples and the current one. Once warmed up,
    // the union of the two horizons therefore contains 1 + the larger lag.
    const auto expected = std::size_t{1}
                        + static_cast<std::size_t>(constructor_lag > length
                                                  ? constructor_lag : length);
    MEM_CHECK(model.size() == expected);
    return model.size();
}

void fixed_pair(int constructor_lag, int length) {
    constexpr std::size_t n = 64;
    const auto short_count = fixed_history(constructor_lag, length, n);
    const auto long_count = fixed_history(constructor_lag, length, 4 * n);
    MEM_CHECK(short_count == long_count);
}

std::size_t growing_history(std::size_t bars) {
    constexpr int constructor_lag = 3;
    TapeModel model(constructor_lag);
    for (std::size_t j = 0; j < bars; ++j) {
        // L=j requests the first input on every call, so no input is obsolete.
        // Check count, insertion and result each time; full equality at regular
        // checkpoints and at the end avoids quadratic full-tape comparisons.
        model.step(static_cast<int>(j), (j + 1) % 512 == 0 || j + 1 == bars);
    }
    const auto peak_count = model.size();
    MEM_CHECK(peak_count == bars);

    // A short current length retains only constructor_lag past samples plus
    // the current sample. The former large series length is not a high-water
    // retention promise. This call must execute many real pop_front steps.
    constexpr auto constructor_span = std::size_t{1} + constructor_lag;
    model.step(1);
    MEM_CHECK(model.size() == constructor_span);

    // Widen after retirement: the four surviving samples plus one new input
    // are all that exist. A large request cannot resurrect retired samples.
    model.step(static_cast<int>(bars + 17));
    MEM_CHECK(model.size() == constructor_span + 1);
    // Return to a usable short lag; this also retires the oldest survivor.
    model.step(constructor_lag);
    MEM_CHECK(model.size() == constructor_span);
    return peak_count;
}
} // namespace memory_ta_change_history

int main() {
    using namespace memory_ta_change_history;
    run_trial("constructor horizon dominates, N and 4N", [] { fixed_pair(11, 3); });
    run_trial("call length dominates, N and 4N", [] { fixed_pair(3, 11); });
    run_trial("default-sized horizon, N and 4N", [] { fixed_pair(1, 1); });
    run_trial("zero call length, N and 4N", [] { fixed_pair(3, 0); });

    // Separate trials continue even if another trial reports a failure or
    // exception. The longer series goes beyond the source-layer 15000 window;
    // that unrelated layer is not involved in these direct Change calls.
    constexpr std::size_t n = 4096;
    std::size_t short_peak = 0;
    std::size_t long_peak = 0;
    run_trial("growing series N, then shrink and widen",
              [&] { short_peak = growing_history(n); });
    run_trial("growing series 4N, then shrink and widen",
              [&] { long_peak = growing_history(4 * n); });
    run_trial("growth follows the supplied length, no fixed ceiling", [&] {
        MEM_CHECK(short_peak == n);
        MEM_CHECK(long_peak == 4 * n);
        MEM_CHECK(long_peak == 4 * short_peak);
    });

    std::printf("memory proof ta_change_history: %zu checks, %zu failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
