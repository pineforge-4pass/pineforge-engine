#pragma once

// Report-only provenance. Nothing here supplies an execution decision. The
// optional owner shares ReportSinkState's output/hash waiver and copy policy.
#include <pineforge/detail/scratch_sequence.hpp>
#include <pineforge/report_sink_writer.hpp>

#include <cstdint>
#include <memory>
#include <string>

namespace pineforge {
struct Trade;
namespace report {

struct ReplayTrade {
    std::uint64_t settlement = 0, parent = 0;
    std::uint64_t slice = 0, slices = 0, child = 0;
    double transaction_qty = 0, whole_qty = 0, whole_pnl = 0;
    double whole_commission = 0, whole_runup = 0, whole_drawdown = 0;
    double whole_exit_fee = 0, transaction_fee = 0;
    bool marked = false, fifo = false, fees = false;
};

enum class EquityBasis : std::uint8_t { kernel_open, range_end, terminal_quote, anchor };
struct ReplayEquity {
    std::int64_t time = 0;
    std::uint64_t settlements = 0;
    double net = 0, term = 0;
    EquityBasis basis = EquityBasis::kernel_open;
};

class ReportReplay {
public:
    ReportReplay(std::string path, std::string run_id);
    std::unique_ptr<ReportReplay> configuration_copy() const;
    void begin(const std::string& scratch_dir, double capital, double pointvalue,
               bool selected, std::int64_t start, std::int64_t end);
    void close(std::size_t global);
    ReplayTrade trade(std::size_t global) const;
    void replace(std::size_t global, const ReplayTrade& value);
    void resize_trades(std::size_t global_end);
    void retire(std::size_t global_end);
    void point(const pf_equity_point_t& value, double net);
    void amend_point(std::int64_t time, EquityBasis basis, double term, double net);
    void write_q(const pf_equity_point_t& point);
    void write_r(std::size_t global, bool terminal);
    void finish(double net, std::string_view base_sha256);
    bool complete() const noexcept { return complete_; }
    bool begun() const noexcept { return writer_ != nullptr; }
    double pointvalue() const noexcept { return pointvalue_; }
    const std::string& path() const noexcept { return path_; }
    const std::string& scratch_directory() const noexcept { return directory_; }
    std::uint64_t q_count() const noexcept { return q_count_; }
    std::uint64_t r_count() const noexcept { return r_count_; }

private:
    std::string path_, run_id_, directory_;
    detail::ScratchSequence<ReplayTrade> trades_;
    detail::ScratchSequence<ReplayEquity> points_;
    std::unique_ptr<report_sink::LineWriter> writer_;
    std::size_t trade_base_ = 0;
    std::uint64_t settlements_ = 0, q_count_ = 0, r_count_ = 0;
    double capital_ = 0, pointvalue_ = 0;
    bool selected_ = false, last_selected_ = false, complete_ = false;
    std::int64_t start_ = 0, end_ = 0;
};
} // namespace report
} // namespace pineforge
