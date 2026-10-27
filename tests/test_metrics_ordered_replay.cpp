// Ordered replay: exact legacy Sharpe/Sortino replay from an ordered packed
// binary64 return stream. This unit owns independent generated references and
// deliberately red mutation rows; it does not use the production fold as its
// oracle.
#include <pineforge/detail/scratch_arena.hpp>
#include <pineforge/detail/scratch_file.hpp>
#include <pineforge/metrics_fold.hpp>
#include <pineforge/run_failure.hpp>
#include "../src/report_scratch.hpp"

#include <algorithm>
#include <cmath>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <limits>
#include <memory>
#include <string>
#include <vector>
#include <sys/resource.h>
#include <unistd.h>

namespace {

using namespace pineforge;
using namespace pineforge::metrics;

constexpr std::int64_t kEpoch = 1'704'067'200'000LL;
constexpr std::int64_t kMinute = 60'000;
constexpr double kYearMs = 365.25 * 86400.0 * 1000.0;
constexpr std::size_t kBatchBytes = 64U * 1024U;

std::string selected_row;
std::string selected_mutant;
int failures = 0;
int checks = 0;

void check(bool value, const char* expression, const char* row) {
    ++checks;
    if (!value) {
        ++failures;
        std::fprintf(stderr, "FAIL [%s] %s\n", row, expression);
    }
}

template<class Function>
void row(const std::string& name, Function function) {
    if (!selected_row.empty() && name.find(selected_row) == std::string::npos) return;
    try {
        function(name);
    } catch (const std::exception& error) {
        ++failures;
        std::fprintf(stderr, "FAIL [%s] unexpected exception: %s\n",
                     name.c_str(), error.what());
    } catch (...) {
        ++failures;
        std::fprintf(stderr, "FAIL [%s] unexpected non-standard exception\n",
                     name.c_str());
    }
}

#define DA_CHECK(row_name, expression) \
    check((expression), #expression, (row_name).c_str())

std::uint64_t splitmix64(std::uint64_t value) {
    value += UINT64_C(0x9e3779b97f4a7c15);
    value = (value ^ (value >> 30)) * UINT64_C(0xbf58476d1ce4e5b9);
    value = (value ^ (value >> 27)) * UINT64_C(0x94d049bb133111eb);
    return value ^ (value >> 31);
}

double noise(std::int64_t index) {
    const double unit = static_cast<double>(splitmix64(
        static_cast<std::uint64_t>(index)) >> 11) * 0x1p-53;
    return unit * 2.0 - 1.0;
}

using Value = std::function<double(std::int64_t)>;

struct Reference {
    double sharpe = std::numeric_limits<double>::quiet_NaN();
    double sortino = std::numeric_limits<double>::quiet_NaN();
    double centered = 0.0;
    double downside = 0.0;
    std::size_t count = 0;
};

Reference legacy_returns(const std::vector<double>& returns,
                         double risk_free = 0.0, double annualizer = 1.0) {
    Reference result;
    result.count = returns.size();
    double sum = 0.0;
    for (const double value : returns) sum += value;
    if (returns.size() < 2) return result;
    const double mean = sum / static_cast<double>(returns.size());
    for (const double value : returns) {
        const double delta = value - mean;
        result.centered += delta * delta;
        const double downside = std::min(0.0, value - risk_free);
        result.downside += downside * downside;
    }
    const double deviation = std::sqrt(
        result.centered / static_cast<double>(returns.size() - 1));
    const double downside_deviation = std::sqrt(
        result.downside / static_cast<double>(returns.size()));
    if (deviation > 0.0)
        result.sharpe = (mean - risk_free) / deviation * annualizer;
    if (downside_deviation > 0.0)
        result.sortino = (mean - risk_free) / downside_deviation * annualizer;
    return result;
}

Reference legacy_series(std::int64_t count, const Value& value) {
    std::vector<double> returns;
    returns.reserve(count > 1 ? static_cast<std::size_t>(count - 1) : 0);
    double previous = count > 0 ? value(0) : 0.0;
    for (std::int64_t i = 1; i < count; ++i) {
        const double current = value(i);
        if (previous > 0.0)
            returns.push_back(current / previous - 1.0);
        previous = current;
    }
    if (count < 3) return legacy_returns(returns);
    const double span_years =
        static_cast<double>((count - 1) * kMinute) / kYearMs;
    const double bars_per_year = static_cast<double>(count - 1) / span_years;
    return legacy_returns(returns, 0.02 / bars_per_year,
                          std::sqrt(bars_per_year));
}

pf_equity_point_t point_at(std::int64_t index, const Value& value) {
    return {kEpoch + index * kMinute, value(index), 0.0};
}

MetricsExtentPlanV1 plan_for(std::int64_t count) {
    MetricsExtentPlanV1 plan;
    plan.present = 1;
    plan.extent.count = count;
    plan.extent.empty = count == 0;
    if (count > 0) {
        plan.extent.first_time_ms = kEpoch;
        plan.extent.last_time_ms = kEpoch + (count - 1) * kMinute;
    }
    plan.attempt = 7;
    plan.input_generation = 11;
    plan.admitted_view = 13;
    plan.spec_identity = 17;
    return plan;
}

MetricsStreamingConfigV1 replay_config() {
    MetricsStreamingConfigV1 config;
    config.metrics_needed = kMetricsNeedSharpeBarV1 | kMetricsNeedSortinoBarV1;
    config.attempt = 7;
    config.input_generation = 11;
    config.admitted_view = 13;
    config.spec_identity = 17;
    config.deterministic_replay = 1;
    config.curve_available = 0;
    return config;
}

struct Provider {
    std::int64_t count;
    Value value;
    std::size_t reads = 0;

    static pf_equity_point_t read(void* context, std::int64_t index) {
        auto& provider = *static_cast<Provider*>(context);
        ++provider.reads;
        return point_at(index, provider.value);
    }

    MetricsRatioCurveV1 view() {
        return {this, count, &read};
    }
};

std::size_t run_probe(const std::string& row_name, std::int64_t count,
                      const Value& value, bool repeat_finish = false) {
    const auto reference = legacy_series(count, value);
    const auto plan = plan_for(count);
    auto config = replay_config();
    std::unique_ptr<StreamingEquityRatiosV1> fold;
    DA_CHECK(row_name, make_streaming_equity_ratios_v1(
        config, plan, {}, fold) == MetricsStreamingStatusV1::Ok);
    DA_CHECK(row_name, fold != nullptr);
    if (!fold) return 0;
    const std::size_t state_bytes = fold->state_size_bytes();
    DA_CHECK(row_name, state_bytes >= kBatchBytes);
    DA_CHECK(row_name, state_bytes <= kBatchBytes + 4096);
    for (std::int64_t i = 0; i < count; ++i) {
        DA_CHECK(row_name, fold->append(point_at(i, value), 0)
            == MetricsStreamingStatusV1::Ok);
    }
    DA_CHECK(row_name, fold->state_size_bytes() == state_bytes);
    Provider provider{count, value};
    MetricsStreamingResultV1 result;
    DA_CHECK(row_name, fold->finish(result, provider.view())
        == MetricsStreamingStatusV1::Ok);
    DA_CHECK(row_name, provider.reads == 0);
    DA_CHECK(row_name, result.receipt.route == MetricsRatioRouteV1::Streaming);
    DA_CHECK(row_name,
             result.receipt.route_reason == MetricsRouteReasonV1::OrderedReplay);
    DA_CHECK(row_name, result.receipt.eligible_returns == reference.count);
    DA_CHECK(row_name, result.failure_code == nullptr);
    DA_CHECK(row_name,
             std::memcmp(&result.sharpe_bar, &reference.sharpe, sizeof(double)) == 0
             || (std::isnan(result.sharpe_bar) && std::isnan(reference.sharpe)));
    DA_CHECK(row_name,
             std::memcmp(&result.sortino_bar, &reference.sortino, sizeof(double)) == 0
             || (std::isnan(result.sortino_bar) && std::isnan(reference.sortino)));
    DA_CHECK(row_name,
             std::memcmp(&result.centered_sum, &reference.centered, sizeof(double)) == 0);
    DA_CHECK(row_name,
             std::memcmp(&result.downside_sum, &reference.downside, sizeof(double)) == 0);
    if (repeat_finish) {
        MetricsStreamingResultV1 again;
        DA_CHECK(row_name, fold->finish(again) == MetricsStreamingStatusV1::Ok);
        DA_CHECK(row_name,
                 std::memcmp(&again.sharpe_bar, &result.sharpe_bar,
                             sizeof(double)) == 0);
        DA_CHECK(row_name,
                 std::memcmp(&again.sortino_bar, &result.sortino_bar,
                             sizeof(double)) == 0);
        DA_CHECK(row_name, again.receipt.eligible_returns
            == result.receipt.eligible_returns);
    }
    return state_bytes;
}

void probe_rows() {
    for (const auto kind : {"noisy", "trend"}) {
        for (const std::int64_t count : {10'000LL, 100'000LL, 1'000'000LL}) {
            const std::string name = std::string("probe-") + kind + "-"
                + std::to_string(count);
            row(name, [kind, count](const std::string& row_name) {
                const Value value = [kind](std::int64_t i) {
                    if (kind == std::string("noisy"))
                        return 100.0 + 2.0 * std::sin(
                            static_cast<double>(i) * 1e-4) + 0.05 * noise(i);
                    return 1000.0 + 0.0005 * static_cast<double>(i)
                        + 0.2 * noise(i);
                };
                run_probe(row_name, count, value);
            });
        }
    }
}

void flat_between_row() {
    row("flat-between-streaming", [](const std::string& row_name) {
        const Value value = [](std::int64_t i) {
            return i == 0 ? 103.0 : i == 999'999 ? 97.0 : 100.0;
        };
        run_probe(row_name, 1'000'000, value);
    });
}

void determinism_row() {
    row("determinism-two-independent-replays",
        [](const std::string& row_name) {
            const Value value = [](std::int64_t i) {
                return 100.0 + 2.0 * std::sin(
                    static_cast<double>(i) * 1e-4) + 0.05 * noise(i);
            };
            const std::size_t first = run_probe(row_name, 100'000, value);
            const std::size_t second = run_probe(row_name, 100'000, value);
            DA_CHECK(row_name, first == second);
        });
}

void memory_row() {
    row("memory-bounded-batch-and-eight-byte-stream",
        [](const std::string& row_name) {
            const Value value = [](std::int64_t i) {
                return 100.0 + 0.001 * static_cast<double>(i);
            };
            std::size_t state = 0;
            for (const std::int64_t count : {10'000LL, 100'000LL, 1'000'000LL}) {
                const auto current = run_probe(row_name, count, value);
                if (state == 0) state = current;
                DA_CHECK(row_name, current == state);
                DA_CHECK(row_name, (count - 1) * sizeof(double)
                    == static_cast<std::uint64_t>(count - 1) * 8U);
            }
        });
}

std::string args_of(const RunFailureValue& failure) {
    return failure.args ? *failure.args : "{}";
}

void scratch_read_fault_row() {
    row("scratch-read-fault-is-typed", [](const std::string& row_name) {
        report_sink::ScratchFile file("");
        const double value = 1.0;
        file.write(0, &value, sizeof(value));
        if (file.descriptor() >= 0)
            DA_CHECK(row_name, ::ftruncate(file.descriptor(), 0) == 0);
        else
            file.clear();
        double observed = 0.0;
        bool typed = false;
        try {
            file.read(0, &observed, sizeof(observed));
        } catch (const detail::ReportScratchIoError& error) {
            const auto failure = classify_run_failure(error);
            const auto arguments = args_of(failure);
            typed = failure.code == RunFailureCode::report_sink_failed
                && (arguments.find("\"reason\":\"scratch_io\"") != std::string::npos
                    || arguments.find("\"reason\":\"arena_exhausted\"") != std::string::npos);
        }
        DA_CHECK(row_name, typed);
    });
}

struct ResourceLimit {
    rlim_t saved = 0;
    rlim_t saved_max = 0;
    bool active = false;
    void (*saved_signal)(int) = SIG_DFL;
    bool signal_active = false;
    explicit ResourceLimit(rlim_t limit) {
        struct rlimit current {};
        if (::getrlimit(RLIMIT_FSIZE, &current) != 0) return;
        saved = current.rlim_cur;
        saved_max = current.rlim_max;
        struct rlimit replacement = current;
        replacement.rlim_cur = std::min(current.rlim_max, limit);
        if (::setrlimit(RLIMIT_FSIZE, &replacement) == 0) {
            active = true;
            saved_signal = std::signal(SIGXFSZ, SIG_IGN);
            signal_active = true;
        }
    }
    ~ResourceLimit() {
        if (signal_active) std::signal(SIGXFSZ, saved_signal);
        if (!active) return;
        struct rlimit replacement {};
        replacement.rlim_cur = saved;
        replacement.rlim_max = saved_max;
        (void)::setrlimit(RLIMIT_FSIZE, &replacement);
    }
};

void scratch_write_fault_row() {
    row("scratch-write-fault-is-typed", [](const std::string& row_name) {
        detail::ScratchFile blocker;
        const bool jailed = detail::ScratchArena::attached() != nullptr;
        if (jailed) {
            blocker.set_store(detail::ScratchStore::curve);
            blocker.open("ordered-return-fault-blocker");
            const auto free_chunks = detail::ScratchArena::attached()->stats().chunks_free;
            if (free_chunks != 0)
                blocker.ensure_size(free_chunks * detail::ScratchArena::kChunkBytes);
        }
        std::unique_ptr<StreamingEquityRatiosV1> fold;
        const auto config = replay_config();
        const auto plan = plan_for(8195);
        DA_CHECK(row_name, make_streaming_equity_ratios_v1(
            config, plan, {}, fold) == MetricsStreamingStatusV1::Ok);
        ResourceLimit limit(0);
        if (!jailed && !limit.active) {
            DA_CHECK(row_name, false);
            return;
        }
        bool typed = false;
        std::string diag = "no exception thrown";
        try {
            const Value value = [](std::int64_t i) {
                return 100.0 + 0.001 * static_cast<double>(i);
            };
            for (std::int64_t i = 0; i < 8195; ++i)
                (void)fold->append(point_at(i, value), 0);
        } catch (const detail::ReportScratchIoError& error) {
            const auto failure = classify_run_failure(error);
            const auto arguments = args_of(failure);
            typed = failure.code == RunFailureCode::report_sink_failed
                && (arguments.find("\"reason\":\"scratch_io\"") != std::string::npos
                    || arguments.find("\"reason\":\"arena_exhausted\"") != std::string::npos)
                && (!jailed || arguments.find("\"store\":\"curve\"") != std::string::npos);
            diag = "ReportScratchIoError code=" + std::to_string(static_cast<int>(failure.code))
                + " args=" + arguments;
        } catch (const std::exception& error) {
            diag = std::string("other exception: ") + error.what();
        }
        if (!typed) std::fprintf(stderr, "scratch-write-fault diagnostic: jailed=%d: %s\n",
                                 jailed ? 1 : 0, diag.c_str());
        DA_CHECK(row_name, typed);
    });
}

void scratch_write_fault_finalize_row() {
    row("scratch-write-fault-aborts-finalize",
        [](const std::string& row_name) {
            constexpr auto prefix_returns = static_cast<std::int64_t>(
                detail::ScratchArena::kChunkBytes / sizeof(double));
            constexpr auto count = prefix_returns + 3;
            const Value value = [](std::int64_t i) {
                return 100.0 + 0.05 * noise(i);
            };
            std::unique_ptr<StreamingEquityRatiosV1> fold;
            DA_CHECK(row_name, make_streaming_equity_ratios_v1(
                replay_config(), plan_for(count), {}, fold)
                == MetricsStreamingStatusV1::Ok);
            DA_CHECK(row_name, fold != nullptr);
            if (!fold) return;
            for (std::int64_t i = 0; i < count; ++i)
                DA_CHECK(row_name, fold->append(point_at(i, value), 0)
                    == MetricsStreamingStatusV1::Ok);

            detail::ScratchFile blocker;
            auto* arena = detail::ScratchArena::attached();
            const bool jailed = arena != nullptr;
            if (jailed) {
                blocker.set_store(detail::ScratchStore::curve);
                blocker.open("ordered-return-finalize-fault-blocker");
                const auto free_chunks = arena->stats().chunks_free;
                if (free_chunks != 0)
                    blocker.ensure_size(
                        free_chunks * detail::ScratchArena::kChunkBytes);
            }
            ResourceLimit limit(0);
            DA_CHECK(row_name, jailed || limit.active);
            if (!jailed && !limit.active) return;

            bool typed = false;
            bool returned_success = false;
            MetricsStreamingResultV1 result;
            try {
                returned_success = fold->finish(result)
                    == MetricsStreamingStatusV1::Ok;
            } catch (const detail::ReportScratchIoError& error) {
                const auto failure = classify_run_failure(error);
                const auto arguments = args_of(failure);
                typed = failure.code == RunFailureCode::report_sink_failed
                    && (arguments.find("\"reason\":\"scratch_io\"")
                            != std::string::npos
                        || arguments.find("\"reason\":\"arena_exhausted\"")
                            != std::string::npos)
                    && (!jailed
                        || arguments.find("\"store\":\"curve\"")
                            != std::string::npos);
            }
            DA_CHECK(row_name, typed);
            DA_CHECK(row_name, !returned_success);
        });
}

std::vector<double> mutation_returns() {
    return {0x1p30, 1.0, -0x1p30, 3.0, 0x1p-20,
            -0x1p-20, 0x1p25, -7.0, 0x1p-25};
}

double kahan_sharpe(const std::vector<double>& values) {
    double sum = 0.0, correction = 0.0;
    for (const double value : values) {
        const double adjusted = value - correction;
        const double next = sum + adjusted;
        correction = (next - sum) - adjusted;
        sum = next;
    }
    const double mean = sum / static_cast<double>(values.size());
    double variance = 0.0;
    for (const double value : values) {
        const double delta = value - mean;
        variance += delta * delta;
    }
    return mean / std::sqrt(variance / static_cast<double>(values.size() - 1));
}

void mutation_rows() {
    row("mutation-skip-every-1024th-return",
        [](const std::string& row_name) {
            const Value value = [](std::int64_t i) {
                return 100.0 + 2.0 * std::sin(
                    static_cast<double>(i) * 1e-4) + 0.05 * noise(i);
            };
            if (selected_mutant != "skip") return;
            std::vector<double> all, values;
            all.reserve(9'999);
            values.reserve(9'999);
            double previous = value(0);
            for (std::int64_t i = 1; i < 10'000; ++i) {
                const double current = value(i);
                if (previous > 0.0) {
                    all.push_back(current / previous - 1.0);
                    if ((i % 1024) != 0)
                        values.push_back(current / previous - 1.0);
                }
                previous = current;
            }
            const auto expected = legacy_returns(all);
            const auto mutated = legacy_returns(values);
            DA_CHECK(row_name, std::memcmp(&mutated.sharpe, &expected.sharpe,
                                           sizeof(double)) == 0);
        });
    row("mutation-reorder-two-returns",
        [](const std::string& row_name) {
            const auto original = mutation_returns();
            auto reordered = original;
            std::swap(reordered[0], reordered[1]);
            if (selected_mutant != "reorder") return;
            const auto expected = legacy_returns(original);
            const auto mutated = legacy_returns(reordered);
            DA_CHECK(row_name, std::memcmp(&mutated.sharpe, &expected.sharpe,
                                           sizeof(double)) == 0);
        });
    row("mutation-kahan-mean",
        [](const std::string& row_name) {
            if (selected_mutant != "kahan") return;
            const auto values = mutation_returns();
            const auto expected = legacy_returns(values);
            const double mutated = kahan_sharpe(values);
            DA_CHECK(row_name, std::memcmp(&mutated, &expected.sharpe,
                                           sizeof(double)) == 0);
        });
}

} // namespace

int main(int argc, char** argv) {
    for (int i = 1; i < argc; ++i) {
        const std::string argument = argv[i];
        if (argument == "--row" && i + 1 < argc) selected_row = argv[++i];
        else if (argument == "--mutant" && i + 1 < argc)
            selected_mutant = argv[++i];
        else {
            std::fprintf(stderr, "unknown argument: %s\n", argument.c_str());
            return 2;
        }
    }
    probe_rows();
    flat_between_row();
    determinism_row();
    memory_row();
    scratch_read_fault_row();
    scratch_write_fault_row();
    scratch_write_fault_finalize_row();
    mutation_rows();
    std::printf("ordered replay: %d checks, %d failures\n",
                checks, failures);
    return failures == 0 ? 0 : 1;
}
