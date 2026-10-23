#pragma once

// B1-11 / E-10: real sink publication, with scratch confined to scratch-s4e.
// Keep the C declarations ahead of engine.hpp's strategy-declaration guard.
#include <pineforge/pineforge.h>
#include <pineforge/report_sink.h>
#include <pineforge/source/pine_strategy_host.hpp>

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

namespace sealed_accessors_test {

inline int checks = 0;
inline int failures = 0;

inline void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

inline void check(bool condition, const std::string& message) {
    ++checks;
    if (!condition) {
        ++failures;
        std::fprintf(stderr, "FAIL: %s\n", message.c_str());
    }
}

inline std::filesystem::path root() {
    const auto result = std::filesystem::path(__FILE__).parent_path().parent_path();
    require(result.is_absolute(), "CMake must supply an absolute test source path");
    return result;
}

struct Directory {
    std::string path;

    Directory() {
        const auto scratch = root() / "scratch-s4e";
        std::filesystem::create_directories(scratch);
        std::string pattern = (scratch / "rep-accessors-XXXXXX").string();
        const char* created = ::mkdtemp(pattern.data());
        require(created != nullptr, "cannot create accessor test scratch");
        path = created;
        require(::setenv("TMPDIR", path.c_str(), 1) == 0, "cannot bind test scratch");
        std::fprintf(stderr, "scratch: %s (retained)\n", path.c_str());
    }
    // Retain all artifacts for the supervisor; no cleanup or system-temp path.
};

struct Report {
    pineforge::ReportC value{};
    ~Report() { pineforge::BacktestEngine::free_report(&value); }
};

class Host final : public pineforge::source::PineStrategyHost {
public:
    Host() {
        set_syminfo_timezone("UTC");
        set_syminfo_session("24x7");
        set_syminfo_mintick(0.25);
        pineforge::source::PineStrategyConfig config;
        config.initial_capital = 10000.0;
        config.default_qty_type = static_cast<int>(pineforge::QtyType::FIXED);
        config.default_qty_value = 1.25;
        config.pyramiding = 3;
        config.commission_type = static_cast<int>(pineforge::CommissionType::CASH_PER_ORDER);
        config.commission_value = 0.125;
        configure_pine_strategy(config);
        set_trade_history_streamable(true);
    }

    pf_strategy_t handle() { return static_cast<pineforge::BacktestEngine*>(this); }
    std::size_t sealed_base() const { return trades_.base(); }

    void on_source_bar(const pineforge::Bar&) override {
        const int index = pine_bar_index();
        if (commanded_ == index) return;
        commanded_ = index;
        const double missing = std::numeric_limits<double>::quiet_NaN();
        if (index % 6 == 0) {
            const bool long_side = (index / 6) % 2 == 0;
            strategy_entry("entry-a", long_side, missing, missing, 1.25, "a");
            strategy_entry("entry-b", long_side, missing, missing, 2.5, "b");
        }
        if (index % 6 == 3) strategy_close_all();
    }

private:
    int commanded_ = -1;
};

struct SinkRun {
    Host host;
    Report report;

    explicit SinkRun(const Directory& directory) {
        const std::string path = directory.path + "/sealed.ndjson";
        const pf_report_sink_v1 sink{
            sizeof sink, 1, path.c_str(), "B1-11-E-10", directory.path.c_str()};
        require(strategy_set_report_sink_v1(host.handle(), &sink) == 0, "sink refused");
        // The established L8c production-seal cadence: many completed exit-time
        // groups, a resident closed suffix, then two open lots at range end.
        std::vector<pineforge::Bar> bars;
        for (int index = 0; index < 146; ++index) {
            const double price = 100.0 + (index % 13 - 6) * 0.25;
            bars.push_back({price, price + 1.5, price - 1.5,
                price + (index % 2 == 0 ? 0.25 : -0.25), 1.0,
                INT64_C(1736121600000) + index * INT64_C(60000)});
        }
        host.run(bars.data(), static_cast<int>(bars.size()), "1", "1", false);
        require(host.last_run_status() == 0 && host.last_error().empty(),
                "sink run failed: " + host.last_error());
        require(host.sealed_base() > 0, "sink run did not seal a row");
        require(host.sealed_base() < static_cast<std::size_t>(host.trade_count()),
                "sink run has no resident closed suffix");
        host.fill_report(&report.value);
        require(host.last_error().empty(), "sink capture failed: " + host.last_error());
        require(report.value.trades == nullptr && report.value.trades_len == 0,
                "sink unexpectedly returned a full trade array");
        require(host.report_trade_count() > host.trade_count(),
                "sink fixture has no range-end rows");
        pf_report_sink_receipt_v1 receipt{};
        receipt.struct_size = sizeof receipt;
        receipt.version = 1;
        require(strategy_report_sink_receipt_v1(host.handle(), &receipt) == 0,
                "sink receipt query refused");
        require(receipt.complete == 1 && receipt.count_t == host.report_trade_count(),
                "sink did not publish all report rows");
    }
};

inline std::string header_text() {
    const auto path = root() / "include" / "pineforge" / "pineforge.h";
    std::fprintf(stderr, "doc check follows: %s\n", path.c_str());
    std::ifstream input(path, std::ios::binary);
    require(input.is_open(), "cannot read the public accessor contract");
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

inline std::string paragraph(const std::string& header, const std::string& declaration) {
    const auto end = header.find(declaration);
    require(end != std::string::npos, "accessor declaration absent: " + declaration);
    const auto begin = header.rfind("/**", end);
    require(begin != std::string::npos, "accessor doxygen absent: " + declaration);
    return header.substr(begin, end - begin);
}

inline void absent_views(pf_strategy_t handle, int index) {
    check(strategy_closed_trade_close_cause(handle, index) == -1,
          "legacy close-cause -1 for index " + std::to_string(index));
    check(strategy_closed_trade_entry_incarnation(handle, index) == 0,
          "unavailable incarnation must be zero");
    check(strategy_closed_trade_entry_id(handle, index) == nullptr,
          "unavailable entry id must be NULL");
    check(strategy_closed_trade_exit_id(handle, index) == nullptr,
          "unavailable exit id must be NULL");
    check(strategy_closed_trade_exit_comment(handle, index) == nullptr,
          "unavailable exit comment must be NULL");
}

inline int result(const char* name) {
    std::printf("%s: %d checks, %d failures\n", name, checks, failures);
    return failures ? 1 : 0;
}

} // namespace sealed_accessors_test
