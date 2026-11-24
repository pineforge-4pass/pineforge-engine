#pragma once

#include <iostream>

#if defined(__linux__)

#include <pineforge/pineforge.h>
#include <pineforge/trade_ledger.hpp>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <dirent.h>
#include <iomanip>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <sys/stat.h>
#include <system_error>
#include <tuple>
#include <unistd.h>
#include <utility>
#include <vector>

namespace run_results_test {

constexpr char kProcFd[] = "/proc/self/fd";
constexpr std::string_view kDeleted = " (deleted)";
constexpr std::string_view kIndex = "pineforge-trade-index-";
constexpr std::string_view kPayload = "pineforge-trade-payload-";
constexpr std::string_view kReportScratch = "pineforge-scratch-";
constexpr std::string_view kOrderScratch = "pineforge-o1e2-";
constexpr std::string_view kRoots = "pineforge-roots-";
constexpr std::string_view kCommandScratch = "pineforge-command-";
constexpr char kStubMessage[] = "run results release is not supported";
constexpr char kBusyMessage[] = "run results release refused while a run or stream is active";
constexpr char kReleasedMessage[] = "trade ledger: closed trades released";
constexpr char kJournalReleasedMessage[] = "native command journal: run results released";
constexpr int kRoundTrips = static_cast<int>(pineforge::TradeLedger::kResidentRows) + 1;
constexpr double kCapital = 10000.0;
constexpr double kTick = 0.25;

inline void require(bool condition, std::string_view message) {
    if (!condition) throw std::runtime_error(std::string(message));
}

inline bool starts_with(std::string_view text, std::string_view prefix) {
    return text.substr(0, prefix.size()) == prefix;
}

inline std::string_view scratch_name(std::string_view target) {
    if (target.size() < kDeleted.size()
        || target.substr(target.size() - kDeleted.size()) != kDeleted) return {};
    target.remove_suffix(kDeleted.size());
    const auto slash = target.find_last_of('/');
    const auto name = slash == std::string_view::npos ? target : target.substr(slash + 1);
    return starts_with(name, "pineforge-") ? name : std::string_view{};
}

inline std::string_view store_name(std::string_view name, std::string_view scenario) {
    constexpr std::pair<std::string_view, std::string_view> stores[] = {
        {kIndex, "closed-trade ledger index"},          // src/engine_orders.cpp:1101
        {kPayload, "closed-trade ledger payload"},     // src/engine_orders.cpp:1102
        {kReportScratch, "equity curve, position tail or metrics replay"}, // src/report_scratch.cpp:91
        {"pineforge-script-state-", "script collection state"},           // src/script_store.cpp:29
        {"pineforge-window-", "numeric history window"},                   // include/pineforge/series.hpp:273
        {"pineforge-id-pairs-", "adapter ID pair tables"},                  // include/pineforge/detail/id_pair_scratch_file.hpp:63
        {"pineforge-ids-", "adapter cohort or exit rank tables"},            // include/pineforge/detail/disk_id_table.hpp:92
        {"pineforge-sequence-", "retained sequence"},                       // include/pineforge/detail/scratch_sequence.hpp:164
        {"pineforge-feed-", "feed projection"},                            // include/pineforge/detail/feed_projection.hpp:370
        {kCommandScratch, "command scratch store"},                        // src/command_scratch_production.cpp:249
        {"pineforge-openings-", "native opening selector"},                 // src/native_order.cpp:127
        {kRoots, "native successor roots"},                                 // src/native_order.cpp:8324
        {kOrderScratch, "native order journal, receipt or chain scratch"},  // src/native_order.cpp:1108
    };
    // The bare batch host installs a curve sink and retains the command journal.
    // It has no Pine position tail, replay spool or deferred group effects.
    if (scenario == "batch_sink") {
        if (starts_with(name, kReportScratch)) return "equity curve";
        if (starts_with(name, kOrderScratch)) return "native command journal";
    }
    for (const auto& store : stores)
        if (starts_with(name, store.first)) return store.second;
    return "unknown";
}

struct Descriptor {
    std::string number;
    std::string target;
    dev_t device;
    ino_t inode;
    std::uint64_t bytes;
    std::uint64_t allocated;

    bool same_open_file(const Descriptor& other) const {
        // The number alone can be reused by a newly opened run file.
        return number == other.number && device == other.device && inode == other.inode
            && target == other.target;
    }
};
using Descriptors = std::vector<Descriptor>;

inline std::string fd_target(int dir, const char* entry) {
    std::string target(256, '\0');
    for (;;) {
        const auto length = ::readlinkat(dir, entry, target.data(), target.size());
        if (length < 0) {
            const int error = errno;
            if (error == EINTR) continue;
            if (error == ENOENT) return {};
            throw std::system_error(error, std::generic_category(),
                                    std::string("readlinkat /proc/self/fd/") + entry);
        }
        if (static_cast<std::size_t>(length) < target.size()) {
            target.resize(static_cast<std::size_t>(length));
            return target;
        }
        target.resize(target.size() * 2);
    }
}

inline Descriptors read_descriptors(bool scratch_only) {
    std::unique_ptr<DIR, int (*)(DIR*)> dir(::opendir(kProcFd), ::closedir);
    if (!dir) throw std::system_error(errno, std::generic_category(), "opendir /proc/self/fd");
    const int dir_fd = ::dirfd(dir.get());
    if (dir_fd < 0) throw std::system_error(errno, std::generic_category(), "dirfd /proc/self/fd");
    const auto own_fd = std::to_string(dir_fd);
    Descriptors found;
    for (;;) {
        errno = 0;
        const auto* entry = ::readdir(dir.get());
        if (!entry) {
            if (errno != 0)
                throw std::system_error(errno, std::generic_category(), "readdir /proc/self/fd");
            return found;
        }
        const char* number = entry->d_name;
        if (number[0] == '.' || own_fd == number) continue;
        auto target = fd_target(dir_fd, number);
        if (target.empty() || (scratch_only && scratch_name(target).empty())) continue;
        struct stat info{};
        // Follow the proc entry to the open (possibly unlinked) inode.
        int status;
        do { status = ::fstatat(dir_fd, number, &info, 0); } while (status < 0 && errno == EINTR);
        if (status < 0) {
            const int error = errno;
            if (error == ENOENT) continue;
            throw std::system_error(error, std::generic_category(),
                                    std::string("fstatat /proc/self/fd/") + number);
        }
        found.push_back({number, std::move(target), info.st_dev, info.st_ino,
                         static_cast<std::uint64_t>(info.st_size),
                         static_cast<std::uint64_t>(info.st_blocks) * 512u});
    }
}

inline bool contains(const Descriptors& files, std::string_view prefix) {
    return std::any_of(files.begin(), files.end(), [&](const Descriptor& file) {
        return starts_with(scratch_name(file.target), prefix);
    });
}

inline void require_ledger(const Descriptors& files) {
    require(contains(files, kIndex) && contains(files, kPayload),
            "instrument: ledger files not observed");
}

inline bool same_files(const Descriptors& first, const Descriptors& second) {
    return first.size() == second.size()
        && std::all_of(first.begin(), first.end(), [&](const Descriptor& file) {
            return std::any_of(second.begin(), second.end(), [&](const Descriptor& other) {
                return file.same_open_file(other) && file.bytes == other.bytes;
            });
        });
}

inline bool keeps_files(const Descriptors& before, const Descriptors& after,
                        std::string_view transient_prefix = {}) {
    return std::all_of(before.begin(), before.end(), [&](const Descriptor& file) {
        if (!transient_prefix.empty() && starts_with(scratch_name(file.target), transient_prefix))
            return true;
        return std::any_of(after.begin(), after.end(), [&](const Descriptor& other) {
            return file.same_open_file(other);
        });
    });
}

inline bool retired_files(const Descriptors& before, const Descriptors& after) {
    return std::none_of(before.begin(), before.end(), [&](const Descriptor& file) {
        return std::any_of(after.begin(), after.end(), [&](const Descriptor& other) {
            return file.target == other.target && file.device == other.device && file.inode == other.inode;
        });
    });
}

inline void check_next_begin(const Descriptors& completed, const Descriptors& current) {
    require(retired_files(completed, current)
                && !contains(current, kIndex) && !contains(current, kPayload),
            "stage C: stream begin retained the previous run's results");
}

class Measurements {
public:
    explicit Measurements(const char* arm) : arm_(arm) {}

    void baseline() { baseline_ = read_descriptors(false); }

    Descriptors capture(std::string_view stage, std::string_view scenario,
                        std::string_view point = {}) {
        auto files = read_descriptors(true);
        files.erase(std::remove_if(files.begin(), files.end(), [&](const Descriptor& file) {
            return std::any_of(baseline_.begin(), baseline_.end(), [&](const Descriptor& initial) {
                return file.same_open_file(initial);
            });
        }), files.end());
        Totals totals{std::string(scenario) + "." + std::string(stage), files.size(), 0, 0};
        if (!point.empty()) totals.key += "." + std::string(point);
        for (const auto& file : files) {
            totals.bytes += file.bytes;
            totals.allocated += file.allocated;
            line(stage, scenario, point) << " fd=" << file.number
                << " target=" << std::quoted(file.target) << " store="
                << std::quoted(std::string(store_name(scratch_name(file.target), scenario)))
                << " st_size=" << file.bytes << " allocated_bytes=" << file.allocated << '\n';
        }
        line(stage, scenario, point) << " fds=" << totals.count << " bytes=" << totals.bytes
            << " allocated_bytes=" << totals.allocated << '\n';
        totals_.push_back(std::move(totals));
        return files;
    }

    std::ostream& line(std::string_view stage, std::string_view scenario,
                       std::string_view point = {}) const {
        std::cout << "RELEASE stage=" << stage << " arm=" << arm_ << " case=" << scenario;
        if (!point.empty()) std::cout << " point=" << point;
        return std::cout;
    }

    int summary(std::string_view verdict, std::string_view reason = {}) const {
        std::cout << "RELEASE summary=" << verdict << " arm=" << arm_;
        if (!reason.empty()) std::cout << " reason=" << std::quoted(std::string(reason));
        for (const auto& totals : totals_)
            std::cout << ' ' << totals.key << ".fds=" << totals.count
                      << ' ' << totals.key << ".bytes=" << totals.bytes
                      << ' ' << totals.key << ".allocated_bytes=" << totals.allocated;
        std::cout << '\n';
        return verdict == "PASS" ? 0 : (verdict == "SKIP" ? 77 : 1);
    }

private:
    struct Totals {
        std::string key;
        std::size_t count;
        std::uint64_t bytes, allocated;
    };
    const char* arm_;
    Descriptors baseline_;
    std::vector<Totals> totals_;
};

template <class Run>
int measured_main(const char* arm, Run run) {
    Measurements records(arm);
    try {
        try {
            records.baseline();
        } catch (const std::system_error& error) {
            const auto code = error.code();
            if (code == std::errc::no_such_file_or_directory || code == std::errc::not_a_directory
                || code == std::errc::permission_denied || code == std::errc::operation_not_permitted
                || code == std::errc::function_not_supported) {
                return records.summary("SKIP", std::string("/proc/self/fd unavailable: ") + error.what());
            }
            throw;
        }
        const std::string_view failure = run(records);
        return failure.empty() ? records.summary("PASS") : records.summary("FAIL", failure);
    } catch (const std::exception& error) {
        return records.summary("INVALID", error.what());
    }
}

struct ReleaseResult {
    bool released = false;
    bool repeated = false;
    bool stub = false;
};

inline std::string order_label(std::string_view side, int run, int i) {
    return std::string(side) + "-" + std::to_string(run) + "-" + std::to_string(i);
}

inline std::string entry_comment(int run, int i) {
    // Outgrow the ledger's fixed payload tail as well as the resident row window.
    return std::string(2 * pineforge::TradeLedger::kRecordBytes, 'x') + order_label("entry", run, i);
}

inline std::int64_t bar_time(int i) { return 1'704'067'200'000 + i * std::int64_t{300'000}; }
inline double bar_price(int i) { return 100.0 + (i % 12) / 4.0; }

template <class Bar>
Bar bar(int i) {
    const auto price = bar_price(i);
    return {price, price + 1.0, price - 1.0, price, 4.0, bar_time(i)};
}

template <class Row>
bool matches_booked(const Row& row, int i) {
    const int entry = 4 * i + 2;
    const int exit = 4 * i + 4;
    return row.entry_time == bar_time(entry) && row.exit_time == bar_time(exit)
        && row.entry_price == bar_price(entry) && row.exit_price == bar_price(exit)
        && row.pnl == bar_price(exit) - bar_price(entry) && row.qty == 1.0
        && row.commission == 0.0 && row.is_long == 1 && row.open_at_end == 0;
}

template <class Report>
void check_summary(const Report& report, int n) {
    double net = 0;
    for (int i = 0; i < n; ++i) net += bar_price(4 * i + 4) - bar_price(4 * i + 2);
    require(report.total_trades == n && report.metrics.all.num_trades == n
                && report.net_profit == net && report.metrics.all.net_profit == net,
            "fixture: report summary differs from the booked trades");
}

template <class Report>
void check_full_report(const Report& report, int n) {
    check_summary(report, n);
    require(report.trades && report.trades_len == n, "fixture: missing full report trade rows");
    for (int i = 0; i < n; ++i)
        require(matches_booked(report.trades[i], i), "fixture: report trade differs from booked trade");
}

template <class Row>
auto trade_fields(const Row& row) {
    return std::tie(row.entry_time, row.exit_time, row.entry_price, row.exit_price, row.pnl,
                    row.pnl_pct, row.is_long, row.max_runup, row.max_drawdown, row.qty,
                    row.commission, row.entry_bar_index, row.exit_bar_index, row.open_at_end);
}

inline auto trade_summary(const pf_trade_stats_t& s) {
    return std::array{double(s.num_trades), double(s.num_wins), double(s.num_losses),
        double(s.num_even), s.percent_profitable, s.net_profit, s.net_profit_pct,
        s.gross_profit, s.gross_profit_pct, s.gross_loss, s.gross_loss_pct, s.profit_factor,
        s.avg_trade, s.avg_trade_pct, s.avg_win, s.avg_win_pct, s.avg_loss, s.avg_loss_pct,
        s.ratio_avg_win_avg_loss, s.largest_win, s.largest_win_pct, s.largest_loss,
        s.largest_loss_pct, s.commission_paid, s.expectancy, double(s.max_consecutive_wins),
        double(s.max_consecutive_losses), s.avg_bars_in_trade, s.avg_bars_in_wins,
        s.avg_bars_in_losses};
}

inline auto equity_summary(const pf_equity_stats_t& s) {
    return std::array{s.max_equity_drawdown, s.max_equity_drawdown_pct, s.max_equity_runup,
        s.max_equity_runup_pct, s.buy_hold_return, s.buy_hold_return_pct, s.sharpe_monthly,
        s.sortino_monthly, s.sharpe_bar, s.sortino_bar, s.cagr, s.calmar, s.recovery_factor,
        s.time_in_market_pct, s.open_pl};
}

template <class Values>
bool same_summary_values(const Values& a, const Values& b) {
    return std::equal(a.begin(), a.end(), b.begin(),
        [](double x, double y) { return x == y || (std::isnan(x) && std::isnan(y)); });
}

template <class Report>
bool same_report(const Report& a, const Report& b) {
    const auto scalars = [](const Report& r) {
        return std::tie(r.total_trades, r.trades_len, r.net_profit, r.input_bars_processed,
                        r.script_bars_processed, r.magnifier_sub_bars_total,
                        r.magnifier_sample_ticks_total, r.input_tf_seconds, r.script_tf_seconds,
                        r.script_tf_ratio, r.needs_aggregation, r.bar_magnifier_enabled);
    };
    if (scalars(a) != scalars(b) || a.equity_curve_len != b.equity_curve_len) return false;
    for (int i = 0; i < a.trades_len; ++i)
        if (trade_fields(a.trades[i]) != trade_fields(b.trades[i])) return false;
    if (a.equity_curve_len > 0 && (!a.equity_curve || !b.equity_curve)) return false;
    for (std::int64_t i = 0; i < a.equity_curve_len; ++i) {
        const auto& x = a.equity_curve[i];
        const auto& y = b.equity_curve[i];
        if (std::tie(x.time_ms, x.equity, x.open_profit) != std::tie(y.time_ms, y.equity, y.open_profit))
            return false;
    }
    return same_summary_values(trade_summary(a.metrics.all), trade_summary(b.metrics.all))
        && same_summary_values(trade_summary(a.metrics.longs), trade_summary(b.metrics.longs))
        && same_summary_values(trade_summary(a.metrics.shorts), trade_summary(b.metrics.shorts))
        && same_summary_values(equity_summary(a.metrics.equity), equity_summary(b.metrics.equity));
}

template <class Report, void (*Free)(Report*)>
struct OwnedReport {
    Report value{};
    OwnedReport() = default;
    OwnedReport(const OwnedReport&) = delete;
    OwnedReport& operator=(const OwnedReport&) = delete;
    ~OwnedReport() { Free(&value); }
};

}  // namespace run_results_test
#endif
