#include <pineforge/run_failure.hpp>
#include <pineforge/source/pine_strategy_host.hpp>

#include <cstdint>
#include <cstdio>
#include <optional>
#include <string>

namespace {

enum class Kind { Int, Float, Bool, Int64 };

struct Case {
    const char* label;
    Kind kind;
    const char* title;
    const char* invalid;
    const char* valid;
    double valid_value;
    const char* expected_type;
    bool in_preparation = false;
};

constexpr pineforge::Bar bars[] = {
    {100, 102, 99, 101, 5, 60'000},
    {101, 103, 100, 102, 5, 120'000},
    {102, 104, 101, 103, 5, 180'000},
};
constexpr int bar_count = sizeof(bars) / sizeof(bars[0]);
constexpr int expected_bars = 3;
static_assert(bar_count == expected_bars, "the issue fixture has three bars");

bool same_bar(const pineforge::Bar& expected, const pineforge::Bar& actual) {
    return expected.timestamp == actual.timestamp && expected.open == actual.open
        && expected.high == actual.high && expected.low == actual.low
        && expected.close == actual.close && expected.volume == actual.volume;
}

void print_bar(const char* label, const pineforge::Bar& bar) {
    std::fprintf(stderr, " %s={time=%lld, open=%.17g, high=%.17g, low=%.17g, close=%.17g, volume=%.17g}",
                 label, static_cast<long long>(bar.timestamp), bar.open, bar.high, bar.low,
                 bar.close, bar.volume);
}

class InputProbe final : public pineforge::source::PineStrategyHost {
public:
    explicit InputProbe(const Case& row) : row_(row) {}

    void prepare_script_run(const pineforge::Bar* input, int count, bool) override {
        prepared_count = count;
        prepared_input = input != nullptr;
        if (prepared_input && count == expected_bars) {
            for (int i = 0; i < count; ++i) {
                if (!same_bar(bars[i], input[i])) {
                    wrong_bar = i;
                    wrong_value = input[i];
                    break;
                }
            }
        }
        if (row_.in_preparation) read_input();
    }

    void on_source_bar(const pineforge::Bar& bar) override {
        ++callbacks;
        if (callbacks == 1) first_callback_time = bar.timestamp;
        if (!row_.in_preparation && callbacks == 1) read_input();
    }

    int prepared_count = 0;
    bool prepared_input = false;
    int wrong_bar = -1;
    pineforge::Bar wrong_value{};
    std::optional<std::int64_t> first_callback_time;
    int calls = 0;
    int callbacks = 0;
    std::optional<std::string> supplied;
    std::optional<double> value;

private:
    void read_input() {
        ++calls;
        const auto it = inputs_.find(row_.title);
        if (it != inputs_.end()) supplied = it->second;
        switch (row_.kind) {
        case Kind::Int: value = get_input_int(row_.title, 7); break;
        case Kind::Float: value = get_input_double(row_.title, 3.5); break;
        case Kind::Bool: value = get_input_bool(row_.title, true); break;
        case Kind::Int64: value = static_cast<double>(get_input_int64(row_.title, 8)); break;
        }
    }

    const Case& row_;
};

bool check_prepared_bars(const Case& row, const char* run, const InputProbe& probe) {
    if (!probe.prepared_input || probe.prepared_count != expected_bars) {
        std::fprintf(stderr,
                     "NOT COMPARED %s: %s run expected %d prepared bars, input present=1;"
                     " got %d, input present=%d\n",
                     row.label, run, expected_bars, probe.prepared_count, probe.prepared_input);
        return false;
    }
    if (probe.wrong_bar >= 0) {
        std::fprintf(stderr, "NOT COMPARED %s: %s run prepared bar %d:", row.label, run,
                     probe.wrong_bar);
        print_bar("expected", bars[probe.wrong_bar]);
        print_bar("got", probe.wrong_value);
        std::fputc('\n', stderr);
        return false;
    }
    return true;
}

bool check_case(const Case& row) {
    InputProbe control(row);
    control.set_input(row.title, row.valid);
    control.run(bars, bar_count, "1", "1");
    if (!check_prepared_bars(row, "valid control", control)) return false;
    if (control.supplied != row.valid || control.last_run_status() != 0 || !control.last_error().empty()
        || control.calls != 1 || control.callbacks != expected_bars
        || control.first_callback_time != bars[0].timestamp
        || control.value != row.valid_value) {
        std::fprintf(stderr,
                     "NOT COMPARED %s: valid control \"%s\" expected status=0, calls=1, bars=%d,"
                     " first bar=%lld, value=%g, error=\"\"; got status=%d, calls=%d, bars=%d,"
                     " first bar=%lld, returned=%d, value=%g, error=\"%s\", input=\"%s\", present=%d\n",
                     row.label, row.valid, expected_bars, static_cast<long long>(bars[0].timestamp),
                     row.valid_value, control.last_run_status(), control.calls, control.callbacks,
                     static_cast<long long>(control.first_callback_time.value_or(-1)), control.value.has_value(),
                     control.value.value_or(0), control.last_error().c_str(),
                     control.supplied ? control.supplied->c_str() : "", control.supplied.has_value());
        return false;
    }

    InputProbe probe(row);
    probe.set_input(row.title, row.invalid);
    probe.run(bars, bar_count, "1", "1");
    if (!check_prepared_bars(row, "invalid input", probe)) return false;
    if (!row.in_preparation && probe.first_callback_time != bars[0].timestamp) {
        std::fprintf(stderr, "NOT COMPARED %s: invalid run expected first bar=%lld; got %lld\n",
                     row.label, static_cast<long long>(bars[0].timestamp),
                     static_cast<long long>(probe.first_callback_time.value_or(-1)));
        return false;
    }
    if (probe.calls != 1 || !probe.supplied || *probe.supplied != row.invalid) {
        std::fprintf(stderr,
                     "NOT COMPARED %s: expected input=\"%s\", present=1, calls=1;"
                     " got input=\"%s\", present=%d, calls=%d, status=%d, error=\"%s\"\n",
                     row.label, row.invalid, probe.supplied ? probe.supplied->c_str() : "",
                     probe.supplied.has_value(), probe.calls, probe.last_run_status(),
                     probe.last_error().c_str());
        return false;
    }

    const int expected_callbacks = row.in_preparation ? 0 : 1;
    const std::string expected_error = std::string("Invalid value for input '")
        + row.title + "': expected " + row.expected_type;
    const std::string expected_args = std::string("{\"input\":\"")
        + row.title + "\",\"reason\":\"unparseable_value\"}";
    const char* code = pineforge::run_failure_code_of(probe);
    const char* args = pineforge::run_failure_args_of(probe);
    if (probe.last_run_status() != 1 || probe.callbacks != expected_callbacks || probe.value
        || probe.last_error() != expected_error || std::string(code) != "setting_rejected"
        || args != expected_args) {
        std::fprintf(stderr,
                     "FAIL %s: input=\"%s\" expected status=1, bars=%d, returned=0,"
                     " code=setting_rejected, args=%s, error=\"%s\"; got status=%d, bars=%d,"
                     " returned=%d, value=%g, code=\"%s\", args=\"%s\", error=\"%s\"\n",
                     row.label, row.invalid, expected_callbacks, expected_args.c_str(),
                     expected_error.c_str(), probe.last_run_status(), probe.callbacks,
                     probe.value.has_value(), probe.value.value_or(0), code, args,
                     probe.last_error().c_str());
        return false;
    }

    std::printf("PASS %s: %s\n", row.label, probe.last_error().c_str());
    return true;
}

}  // namespace

int main() {
    const Case cases[] = {
        {"int/abc", Kind::Int, "Length", "abc", "21", 21, "an integer"},
        {"int/12abc", Kind::Int, "Length", "12abc", "21", 21, "an integer"},
        {"int/12.5 (preparation)", Kind::Int, "Length", "12.5", "21", 21, "an integer", true},
        {"float/abc", Kind::Float, "Threshold", "abc", "12.5", 12.5, "a finite number"},
        {"float/12abc", Kind::Float, "Threshold", "12abc", "12.5", 12.5, "a finite number"},
        {"bool/abc", Kind::Bool, "Enabled", "abc", "false", 0, "true, false, 1 or 0"},
        {"int64/12abc", Kind::Int64, "Start", "12abc", "1700000000000", 1'700'000'000'000.0, "a 64-bit integer"},
        {"int/empty", Kind::Int, "Length", "", "21", 21, "an integer"},
        {"float/nan", Kind::Float, "Threshold", "nan", "12.5", 12.5, "a finite number"},
        {"int64/overflow", Kind::Int64, "Start", "9223372036854775808",
         "1700000000000", 1'700'000'000'000.0, "a 64-bit integer"},
    };
    static_assert(sizeof(cases) / sizeof(cases[0]) > 0, "the issue test needs cases");
    bool passed = true;
    for (const auto& row : cases)
        if (!check_case(row)) passed = false;
    return passed ? 0 : 1;
}
