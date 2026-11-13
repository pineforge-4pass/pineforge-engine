#include <pineforge/source/pine_strategy_host.hpp>
#include <pineforge/checked_settings.hpp>

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

using namespace pineforge;

int failures = 0;

void check(bool ok, const std::string& row, const char* what) {
    if (ok) return;
    ++failures;
    std::fprintf(stderr, "FAIL %s: %s\n", row.c_str(), what);
}

void check_equal(const std::string& got, const std::string& want,
                 const std::string& row, const char* what) {
    if (got == want) return;
    ++failures;
    std::fprintf(stderr, "FAIL %s: %s\n  got: %s\n  want: %s\n",
                 row.c_str(), what, got.c_str(), want.c_str());
}

class TimeframeStrategy final : public source::PineStrategyHost {
public:
    TimeframeStrategy() {
        source::PineStrategyConfig config;
        config.initial_capital = 10000.0;
        configure_pine_strategy(config);
        set_syminfo_timezone("UTC");
        set_syminfo_session("24x7");
        set_syminfo_mintick(0.01);
    }

    void on_source_bar(const Bar&) override { ++callbacks; }
    int callbacks = 0;
};

struct Refusal {
    const char* name;
    const char* input_tf;
    const char* script_tf;
    bool magnifier;
    std::int64_t step_ms;
    const char* text;
    const char* args;
    int samples = 4;
    MagnifierDistribution distribution = MagnifierDistribution::ENDPOINTS;
    bool timeframe_failure = true;
};

std::vector<Bar> bars_at(std::int64_t step_ms) {
    std::vector<Bar> bars;
    for (int i = 0; i < 60; ++i) {
        bars.push_back({100.0, 101.0, 99.0, 100.0, 1.0,
                        1704067200000LL + static_cast<std::int64_t>(i) * step_ms});
    }
    return bars;
}

// Use the generated batch entry point's checked boundary around the public
// run and report calls; the returned status comes from the host's real error.
int checked_run(TimeframeStrategy& host, const std::vector<Bar>& bars,
                const char* input_tf, const char* script_tf, bool magnifier,
                int samples, MagnifierDistribution distribution,
                char* error, std::size_t capacity) {
    ReportC report{};
    const int status = checked_settings::boundary(error, capacity, [&] {
        host.run(bars.data(), static_cast<int>(bars.size()), input_tf, script_tf,
                 magnifier, samples, distribution);
        host.fill_report(&report);
        const auto& failure = host.last_error();
        checked_settings::require(failure.empty(), failure.c_str(), PF_SETTINGS_RUN_FAILED);
    });
    BacktestEngine::free_report(&report);
    return status;
}

void expect_refusal(const Refusal& row) {
    TimeframeStrategy host;
    const auto bars = bars_at(row.step_ms);
    char error[2048]{};
    const int status = checked_run(host, bars, row.input_tf, row.script_tf,
                                   row.magnifier, row.samples, row.distribution,
                                   error, sizeof error);
    const auto handle = static_cast<pf_strategy_t>(static_cast<BacktestEngine*>(&host));
    const char* text = strategy_get_last_error(handle);
    const char* code = strategy_get_last_error_code(handle);
    const char* args = strategy_get_last_error_args(handle);
    check(status == PF_SETTINGS_RUN_FAILED, row.name, "checked run status is 5");
    check(strategy_last_run_status(handle) == 0, row.name, "begin refusal keeps run status 0");
    check(text && code && args, row.name, "C error getters return non-null strings");
    check_equal(text ? text : "<null>", row.text, row.name, "refusal text");
    check_equal(error, row.text, row.name, "checked error buffer");
    check_equal(code ? code : "<null>", "run_options_rejected", row.name, "failure code");
    check_equal(args ? args : "<null>", row.args, row.name, "canonical failure args");
    check(host.callbacks == 0, row.name, "refusal precedes script callbacks");
    if (row.timeframe_failure && text)
        check(std::string(text).find("field") == std::string::npos,
              row.name, "timeframe refusal exposes no internal field");
}

void expect_minute_input_accepted() {
    constexpr const char* name = "minute_input_builds_magnified_chart";
    TimeframeStrategy host;
    const auto bars = bars_at(60000);
    char error[2048]{};
    const int status = checked_run(host, bars, "1", "15", true, 4,
                                   MagnifierDistribution::ENDPOINTS, error, sizeof error);
    const auto handle = static_cast<pf_strategy_t>(static_cast<BacktestEngine*>(&host));
    check(status == PF_SETTINGS_OK, name, "checked run status is 0");
    check(strategy_last_run_status(handle) == 0, name, "completed run status is 0");
    check(host.callbacks > 0, name, "accepted run reaches the script");
    check_equal(error, "", name, "checked error buffer");
    const char* text = strategy_get_last_error(handle);
    const char* code = strategy_get_last_error_code(handle);
    const char* args = strategy_get_last_error_args(handle);
    check_equal(text ? text : "<null>", "", name, "no refusal text");
    check_equal(code ? code : "<null>", "", name, "no failure code");
    check_equal(args ? args : "<null>", "", name, "no failure args");
}

const Refusal refusals[] = {
    {
        "input_not_divisible_magnified", "2", "15", true, 120000,
        "input timeframe '2' cannot build script timeframe '15' using intrabar timeframe '2' "
        "(15 is not a multiple of 2); pass bars whose timeframe divides the script timeframe "
        "'15', for example 1-minute bars.",
        R"({"input_tf":"2","intrabar_tf":"2","option":"intrabar_timeframe","reason":"intrabar_not_divisible","script_tf":"15"})"
    },
    {
        "input_not_divisible_unmagnified", "2", "15", false, 120000,
        "input timeframe '2' cannot build script timeframe '15' "
        "(15 is not a multiple of 2); pass bars whose timeframe divides the script timeframe "
        "'15', for example 1-minute bars.",
        R"({"input_tf":"2","option":"script_timeframe","reason":"not_divisible","script_tf":"15"})"
    },
    {
        "monthly_script_finer_than_input", "3M", "M", false, 7776000000LL,
        "input timeframe '3M' cannot build script timeframe 'M' "
        "(script timeframe 'M' is finer than input timeframe '3M'); pass bars whose timeframe "
        "divides the script timeframe 'M', for example 1-minute bars.",
        R"({"input_tf":"3M","option":"script_timeframe","reason":"script_finer_than_input","script_tf":"M"})"
    },
    {
        "monthly_script_finer_than_intrabar", "3M", "M", true, 7776000000LL,
        "input timeframe '3M' cannot build script timeframe 'M' using intrabar timeframe '3M' "
        "(script timeframe 'M' is finer than intrabar timeframe '3M'); pass bars whose timeframe "
        "divides the script timeframe 'M', for example 1-minute bars.",
        R"({"input_tf":"3M","intrabar_tf":"3M","option":"intrabar_timeframe","reason":"script_finer_than_intrabar","script_tf":"M"})"
    },
    {
        "seconds_do_not_divide_minute", "45S", "1", false, 45000,
        "input timeframe '45S' cannot build script timeframe '1' "
        "(1 is not a multiple of 45S); pass bars whose timeframe divides the script timeframe "
        "'1', for example 1-minute bars.",
        R"({"input_tf":"45S","option":"script_timeframe","reason":"not_divisible","script_tf":"1"})"
    },
    {
        "intrabar_seconds_do_not_divide_script", "4S", "15S", true, 4000,
        "input timeframe '4S' cannot build script timeframe '15S' using intrabar timeframe '4S' "
        "(15S is not a multiple of 4S); pass bars whose timeframe divides the script timeframe "
        "'15S', for example 1-second bars.",
        R"({"input_tf":"4S","intrabar_tf":"4S","option":"intrabar_timeframe","reason":"intrabar_not_divisible","script_tf":"15S"})"
    },
    {
        "calendar_counts_are_not_multiples", "2D", "3D", false, 172800000,
        "input timeframe '2D' cannot build script timeframe '3D' "
        "(3D is not a multiple of 2D); pass bars whose timeframe divides the script timeframe "
        "'3D', for example 1-minute bars.",
        R"({"input_tf":"2D","option":"script_timeframe","reason":"not_divisible","script_tf":"3D"})"
    },
    {
        "sample_refusal_precedes_timeframe_pairing", "2", "15", true, 120000,
        "Pine adapter produced invalid native run spec field 30",
        R"({"option":"intrabar_samples"})",
        1, MagnifierDistribution::ENDPOINTS, false
    },
    {
        "distribution_refusal_precedes_timeframe_pairing", "2", "15", true, 120000,
        "Pine adapter produced invalid native run spec field 31",
        R"({"option":"intrabar_distribution"})",
        4, static_cast<MagnifierDistribution>(99), false
    }
};

} // namespace

int main(int argc, char** argv) {
    bool selected = false;
    for (const auto& row : refusals) {
        if (argc > 1 && std::strcmp(argv[1], row.name) != 0) continue;
        selected = true;
        expect_refusal(row);
    }
    if (argc == 1 || std::strcmp(argv[1], "minute_input_builds_magnified_chart") == 0) {
        selected = true;
        expect_minute_input_accepted();
    }
    check(selected && argc <= 2, "row_selection", "select an existing row or run all rows");
    return failures == 0 ? 0 : 1;
}
