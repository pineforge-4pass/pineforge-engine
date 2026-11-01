// memory proof matrix_report_replay: matrix populations and the real report-replay scratch owners.
// Expected geometry is arithmetic over this script and its input.
// All sizes below are source-layout expectations, not measured byte usage.
#include <pineforge/generic_matrix.hpp>
#include <pineforge/matrix.hpp>
#include <pineforge/na.hpp>
#include <pineforge/native_host.hpp>
#include <pineforge/source/pine_strategy_host.hpp>
#include "../src/report_replay.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace {
using namespace pineforge;
using IntMatrix = GenericMatrix<int>;
using BoolMatrix = GenericMatrix<bool>;
using Replay = report::ReportReplay;
using TradeSequence = detail::ScratchSequence<report::ReplayTrade>;
using PointSequence = detail::ScratchSequence<report::ReplayEquity>;

template<class Tag, auto Member> struct Access {
    friend auto access(Tag) { return Member; }
};
struct IntStorage { friend auto access(IntStorage); };
struct BoolStorage { friend auto access(BoolStorage); };
struct IntSnapshot { friend auto access(IntSnapshot); };
struct BoolSnapshot { friend auto access(BoolSnapshot); };
struct NumericSnapshot { friend auto access(NumericSnapshot); };
struct ReplayTrades { friend auto access(ReplayTrades); };
struct ReplayPoints { friend auto access(ReplayPoints); };
struct TradeWindow { friend auto access(TradeWindow); };
struct PointWindow { friend auto access(PointWindow); };
template struct Access<IntStorage, &IntMatrix::storage_>;
template struct Access<BoolStorage, &BoolMatrix::storage_>;
template struct Access<IntSnapshot, &IntMatrix::Snapshot::state_>;
template struct Access<BoolSnapshot, &BoolMatrix::Snapshot::state_>;
template struct Access<NumericSnapshot, &NumericMatrix::Snapshot::state_>;
template struct Access<ReplayTrades, &Replay::trades_>;
template struct Access<ReplayPoints, &Replay::points_>;
template struct Access<TradeWindow, &TradeSequence::window_>;
template struct Access<PointWindow, &PointSequence::window_>;

std::size_t checks = 0, failures = 0;
bool check(const char* row, bool ok, const char* file, int line,
           const char* expression, const char* explanation) {
    ++checks;
    if (!ok) {
        ++failures;
        std::fprintf(stderr, "FAIL [%s] %s:%d: %s -- %s\n",
                     row, file, line, expression, explanation);
    }
    return ok;
}
#define ROW_CHECK(row, expression, explanation) \
    check(row, static_cast<bool>(expression), __FILE__, __LINE__, #expression, explanation)

constexpr std::int64_t start = INT64_C(1704067200000);
constexpr std::int64_t minute = 60000;
std::vector<Bar> flat_bars(std::size_t count) {
    std::vector<Bar> result;
    for (std::size_t i = 0; i < count; ++i)
        result.push_back({100.0, 100.0, 100.0, 100.0, 1.0,
                          start + static_cast<std::int64_t>(i) * minute});
    return result;
}

using Model = std::vector<std::vector<int>>;
// Three retained rows, four retained columns. Every script calculation first
// shifts a row through add_row/remove_row, then a column through add_col/remove_col.
// The independent model owns integers; bool uses parity and numeric uses the
// exact double representation of the same small integers.
Model initial_model() { return Model(3, std::vector<int>(4, 0)); }

template<class Data>
bool generic_equal(const Data& actual, const Model& model, bool boolean) {
    if (actual.size() != model.size()) return false;
    for (std::size_t r = 0; r < model.size(); ++r) {
        if (actual[r].size() != model[r].size()) return false;
        for (std::size_t c = 0; c < model[r].size(); ++c)
            if (actual[r][c] != (boolean ? model[r][c] % 2 : model[r][c])) return false;
    }
    return true;
}

bool numeric_equal(const Eigen::MatrixXd& actual, const Model& model) {
    if (actual.rows() != static_cast<Eigen::Index>(model.size())
        || actual.cols() != static_cast<Eigen::Index>(model.front().size())) return false;
    for (std::size_t r = 0; r < model.size(); ++r)
        for (std::size_t c = 0; c < model[r].size(); ++c)
            if (actual(static_cast<Eigen::Index>(r), static_cast<Eigen::Index>(c))
                != static_cast<double>(model[r][c])) return false;
    return true;
}

enum class MatrixProof { Storage, GenericSnapshot, NumericSnapshot };

class MatrixHost final : public source::PineStrategyHost {
public:
    MatrixHost(const char* row, MatrixProof proof)
        : row_(row), proof_(proof), ints_(IntMatrix::new_(3, 4, 0)),
          bools_(BoolMatrix::new_(3, 4, false)), numeric_(NumericMatrix::new_(3, 4, 0.0)),
          anchor_int_(ints_.snapshot()), anchor_bool_(bools_.snapshot()),
          anchor_numeric_(numeric_.snapshot()) {
        source::PineStrategyConfig config;
        config.initial_capital = 10000.0;
        config.default_qty_type = static_cast<int>(QtyType::FIXED);
        config.default_qty_value = 1.0;
        config.pyramiding = 1;
        config.calc_on_order_fills = false;
        config.process_orders_on_close = false;
        configure_pine_strategy(config);
        set_syminfo_timezone("UTC");
        set_syminfo_session("24x7");
        set_syminfo_type("crypto");
    }

    void on_source_bar(const Bar&) override {
        const auto bar = calculations++;
        // Ordinary engine submission, fills and closes accompany the script
        // matrix calls; none of the observed matrices is seeded through Access.
        const double absent = pineforge::na<double>();
        if (bar % 3 == 0) strategy_entry("matrix", true, absent, absent, 1.0);
        if (bar % 3 == 1) strategy_close("matrix");

        std::vector<int> row;
        std::vector<bool> bool_row;
        std::vector<double> numeric_row;
        for (int c = 0; c < 4; ++c) {
            const int value = static_cast<int>(bar) * 10 + c + 1;
            row.push_back(value);
            bool_row.push_back(value % 2 != 0);
            numeric_row.push_back(static_cast<double>(value));
        }
        if (proof_ == MatrixProof::NumericSnapshot) {
            numeric_.add_row(numeric_.rows(), numeric_row);
            numeric_.remove_row(0);
        } else {
            ints_.add_row(ints_.rows(), row);
            bools_.add_row(bools_.rows(), bool_row);
            ints_.remove_row(0);
            bools_.remove_row(0);
        }
        model_.erase(model_.begin());
        model_.push_back(row);
        capture();
        if (!assert_population()) return; // Counts were recorded before any unsafe next operation.

        std::vector<int> column;
        std::vector<bool> bool_column;
        std::vector<double> numeric_column;
        for (std::size_t r = 0; r < model_.size(); ++r) {
            const int value = static_cast<int>(bar) * 10 + static_cast<int>(r) + 5;
            column.push_back(value);
            bool_column.push_back(value % 2 != 0);
            numeric_column.push_back(static_cast<double>(value));
            model_[r].erase(model_[r].begin());
            model_[r].push_back(value);
        }
        if (proof_ == MatrixProof::NumericSnapshot) {
            numeric_.add_col(numeric_.columns(), numeric_column);
            numeric_.remove_col(0);
        } else {
            ints_.add_col(ints_.columns(), column);
            bools_.add_col(bools_.columns(), bool_column);
            ints_.remove_col(0);
            bools_.remove_col(0);
        }
        capture();
        if (!assert_population()) return;

        // The retained state must be a detached complete copy, not an alias
        // whose size happens to match. Restore uses the production checkpoint
        // operation after a real script-visible edit of the original matrix.
        if (proof_ == MatrixProof::NumericSnapshot) {
            numeric_.set(0, 0, -1.0);
            assert_population();
            numeric_.restore(*saved_numeric_);
        } else {
            ints_.set(0, 0, -1);
            bools_.set(0, 0, model_[0][0] % 2 == 0);
            if (proof_ == MatrixProof::GenericSnapshot) assert_population();
            ints_.restore(*saved_int_);
            bools_.restore(*saved_bool_);
        }
        assert_population();
    }

    bool assert_population() const {
        if (proof_ == MatrixProof::Storage) {
            const auto& int_data = (ints_.*access(IntStorage{}))->data;
            const auto& bool_data = (bools_.*access(BoolStorage{}))->data;
            return ROW_CHECK(row_, generic_equal(int_data, model_, false)
                              && generic_equal(bool_data, model_, true),
                             "both actual Storage::data containers equal the independent 3-by-4 model (12 cells each)");
        }
        if (proof_ == MatrixProof::GenericSnapshot) {
            if (!ROW_CHECK(row_, saved_int_.has_value() && saved_bool_.has_value(),
                           "the script actually took both rolling snapshots")) return false;
            return ROW_CHECK(row_, generic_equal(*saved_int_.*access(IntSnapshot{}), model_, false)
                              && generic_equal(*saved_bool_.*access(BoolSnapshot{}), model_, true)
                              && generic_equal(anchor_int_.*access(IntSnapshot{}), initial_model(), false)
                              && generic_equal(anchor_bool_.*access(BoolSnapshot{}), initial_model(), true),
                             "each of the two live snapshots per kind holds exactly one detached 12-cell matrix");
        }
        if (!ROW_CHECK(row_, saved_numeric_.has_value(), "the script took its rolling numeric snapshot")) return false;
        return ROW_CHECK(row_, numeric_equal(*saved_numeric_.*access(NumericSnapshot{}), model_)
                          && numeric_equal(anchor_numeric_.*access(NumericSnapshot{}), initial_model()),
                         "each actual Eigen snapshot holds one complete 3-by-4 copy, including the unchanged anchor");
    }

    std::size_t retained_cells() const {
        if (proof_ == MatrixProof::Storage) {
            std::size_t cells = 0;
            for (const auto& r : (ints_.*access(IntStorage{}))->data) cells += r.size();
            for (const auto& r : (bools_.*access(BoolStorage{}))->data) cells += r.size();
            return cells;
        }
        if (proof_ == MatrixProof::GenericSnapshot) {
            if (!saved_int_ || !saved_bool_) return 0;
            std::size_t cells = 0;
            for (const auto& r : *saved_int_.*access(IntSnapshot{})) cells += r.size();
            for (const auto& r : *saved_bool_.*access(BoolSnapshot{})) cells += r.size();
            for (const auto& r : anchor_int_.*access(IntSnapshot{})) cells += r.size();
            for (const auto& r : anchor_bool_.*access(BoolSnapshot{})) cells += r.size();
            return cells;
        }
        return saved_numeric_ ? static_cast<std::size_t>(
            (*saved_numeric_.*access(NumericSnapshot{})).size()
            + (anchor_numeric_.*access(NumericSnapshot{})).size()) : 0;
    }
    std::size_t calculations = 0;

private:
    void capture() {
        if (proof_ == MatrixProof::NumericSnapshot) {
            saved_numeric_ = numeric_.snapshot();
        } else {
            saved_int_ = ints_.snapshot();
            saved_bool_ = bools_.snapshot();
        }
    }
    const char* row_;
    MatrixProof proof_;
    IntMatrix ints_;
    BoolMatrix bools_;
    NumericMatrix numeric_;
    IntMatrix::Snapshot anchor_int_;
    BoolMatrix::Snapshot anchor_bool_;
    NumericMatrix::Snapshot anchor_numeric_;
    std::optional<IntMatrix::Snapshot> saved_int_;
    std::optional<BoolMatrix::Snapshot> saved_bool_;
    std::optional<NumericMatrix::Snapshot> saved_numeric_;
    Model model_ = initial_model();
};

void matrix_case(const char* row, MatrixProof proof) {
    std::array<std::size_t, 2> cells{};
    for (std::size_t trial = 0; trial < cells.size(); ++trial) {
        MatrixHost host(row, proof);
        const auto input = flat_bars(trial == 0 ? 32 : 128);
        host.run(input.data(), static_cast<int>(input.size()), "1", "1");
        ROW_CHECK(row, host.last_run_status() == 0 && host.last_error().empty(),
                  "the engine completed the script matrix operations");
        ROW_CHECK(row, host.calculations == input.size(), "the script ran on N or 4N bars");
        host.assert_population();
        cells[trial] = host.retained_cells();
    }
    // Storage: int + bool = 2*3*4. Generic snapshots: two holders of each
    // kind = 2*2*3*4. Numeric snapshots: two holders = 2*3*4.
    const std::size_t expected = proof == MatrixProof::GenericSnapshot ? 2 * 2 * 3 * 4 : 2 * 3 * 4;
    ROW_CHECK(row, cells[0] == expected && cells[1] == expected && cells[0] == cells[1],
              "actual retained cell populations are fixed as script bars grow from N to 4N");
}
void storage_case(const char* row) { matrix_case(row, MatrixProof::Storage); }
void generic_snapshot_case(const char* row) { matrix_case(row, MatrixProof::GenericSnapshot); }
void numeric_snapshot_case(const char* row) { matrix_case(row, MatrixProof::NumericSnapshot); }

class ReplayHost final : public NativeStrategyHost {
public:
    explicit ReplayHost(const char* row) : row_(row) {}
    void on_native_bar(const Bar&, const NativeDecisionContext&) override {
        const auto index = calculations++;
        // Entry submitted at 3k fills at 3k+1; flatten submitted at 3k+1
        // closes it at 3k+2. A 3T-bar tape therefore has exactly T closes.
        if (index % 3 == 0) put({native_order::Transact{1.0}, "matrix-entry", ""});
        if (index % 3 == 1) put({native_order::Flatten{}, "matrix-close", ""});
    }
    Replay* replay() const { return report_replay_state(); }
    std::size_t closed() const { return trades_.size(); }
    std::size_t calculations = 0;
private:
    void put(const native_order::Request& request) {
        const auto result = submit(request);
        ROW_CHECK(row_, result.status == native_order::SubmitStatus::Accepted && result.handle.has_value(),
                  "the real native submit path accepted the entry or flatten");
    }
    const char* row_;
};

NativeRunSpec replay_spec(std::uint64_t run) {
    NativeRunSpec config;
    config.identity = {"matrix-replay", run};
    config.input_tf = config.script_tf = "1";
    config.tickerid = "TEST:MATRIX";
    config.type = "crypto";
    config.timezone = "UTC";
    config.session = "24x7";
    config.initial_capital = 10000.0;
    config.point_value = config.account_fx = 1.0;
    config.price_tick = 0.25;
    config.fee_kind = NativeFeeKind::CashPerExecution;
    config.fee_value = 0.0;
    config.report_policy = NativeReportPolicy::KernelRecorded;
    return config;
}

struct ReportOwner {
    ReportC value{};
    ~ReportOwner() { BacktestEngine::free_report(&value); }
};

void replay_case(const char* row, bool points) {
    std::array<std::size_t, 2> windows{}, after{};
    // Both sizes exceed each related sequence's resident window. 1024 real
    // closes need 3072 equity points, and quadrupling gives 4096/12288.
    for (std::size_t trial = 0; trial < windows.size(); ++trial) {
        const std::size_t closes = trial == 0 ? 1024 : 4096;
        const auto input = flat_bars(3 * closes);
        ReplayHost host(row);
        ROW_CHECK(row, host.configure_native(replay_spec(trial + 1)).status == NativeSetupStatus::Applied,
                  "configure the native kernel recorder");
        // Keep replay artifacts in a unique directory and remove that
        // directory and its contents when this trial ends.
        std::string directory = "/tmp/pineforge-matrix-replay-XXXXXX";
        if (!ROW_CHECK(row, ::mkdtemp(directory.data()) != nullptr, "create a unique companion directory")) continue;
        struct Cleanup {
            const char* path;
            ~Cleanup() {
                std::error_code error;
                std::filesystem::remove_all(path, error);
            }
        } cleanup{directory.c_str()};
        const std::string companion = directory + "/replay.ndjson";
        ROW_CHECK(row, host.set_report_replay_v1(companion.c_str(), "matrix-log") == 0,
                  "enable the real companion before engine begin");
        host.run(input.data(), static_cast<int>(input.size()));
        ROW_CHECK(row, host.last_run_status() == 0 && host.last_error().empty()
                       && host.calculations == input.size() && host.closed() == closes,
                  "all 3T bars and T distinct engine closes reached the recorder");
        auto* replay = host.replay();
        if (!ROW_CHECK(row, replay != nullptr && replay->begun(), "the real replay owner was installed")) continue;
        const auto& trades = *replay.*access(ReplayTrades{});
        const auto& equity = *replay.*access(ReplayPoints{});
        if (points) {
            ROW_CHECK(row, equity.size() == input.size(),
                      "one actual scratch equity row is retained for each of the 3T script bars before emission");
            bool equal = equity.size() == input.size();
            for (std::size_t i = 0; equal && i < equity.size(); ++i) {
                const auto value = equity.at(i);
                equal = value.time == input[i].timestamp && value.settlements == (i + 1) / 3
                    && value.net == 0.0 && value.term == 0.0
                    && value.basis == report::EquityBasis::kernel_open;
            }
            ROW_CHECK(row, equal, "the whole ReplayEquity container equals the independent flat-price/time/settlement model");
            windows[trial] = (equity.*access(PointWindow{})).size();
            // This resident array's extent is fixed by type.
            ROW_CHECK(row, windows[trial] == (64u * 1024u) / sizeof(report::ReplayEquity),
                      "the actual related resident array has only the source's 64-KiB element allotment");
        } else {
            ROW_CHECK(row, trades.size() == closes,
                      "the actual replay trade sequence contains exactly one row per real engine close");
            bool equal = trades.size() == closes;
            for (std::size_t i = 0; equal && i < trades.size(); ++i) {
                const auto value = trades.at(i);
                equal = value.settlement == i && !value.marked && !value.fifo && !value.fees
                    && value.parent == 0 && value.slice == 0 && value.slices == 0 && value.child == 0
                    && value.transaction_qty == 0.0 && value.whole_qty == 0.0 && value.whole_pnl == 0.0
                    && value.whole_commission == 0.0 && value.whole_runup == 0.0 && value.whole_drawdown == 0.0
                    && value.whole_exit_fee == 0.0 && value.transaction_fee == 0.0;
            }
            ROW_CHECK(row, equal, "the whole ReplayTrade container equals the independent ordinary-close ordinal model");
            windows[trial] = (trades.*access(TradeWindow{})).size();
            // This resident array's extent is fixed by type.
            ROW_CHECK(row, windows[trial] == (64u * 1024u) / sizeof(report::ReplayTrade),
                      "the actual trade resident array stays one 64-KiB element allotment while T grows");
        }
        // No primary sink was installed, so requesting the final real report
        // emits all Q/R records now. Keep checking container counts even when
        // a missing retirement makes emission refuse partway through.
        ReportOwner report;
        try {
            host.fill_report(&report.value);
        } catch (const std::exception& error) {
            ROW_CHECK(row, false, error.what());
        }
        if (points) {
            ROW_CHECK(row, equity.size() == 0,
                      "successful Q consumption retires every actual replay equity operand");
            after[trial] = equity.size();
        } else {
            ROW_CHECK(row, trades.size() == 0,
                      "completed replay output retires every retained closed-trade annotation");
            after[trial] = trades.size();
        }
        ROW_CHECK(row, replay->complete() && replay->q_count() == input.size() && replay->r_count() == closes,
                  "the report really emitted all independent equity/trade ordinals");
    }
    const auto expected_window = points ? (64u * 1024u) / sizeof(report::ReplayEquity)
                                        : (64u * 1024u) / sizeof(report::ReplayTrade);
    // Window extents are fixed by type; post-output empty counts are runtime checks.
    ROW_CHECK(row, windows[0] == expected_window && windows[1] == expected_window
                   && windows[0] == windows[1] && after[0] == 0 && after[1] == 0,
              "the real resident population and post-output empty count are the same at T/B and 4T/4B");
}
void replay_trades_case(const char* row) { replay_case(row, false); }
void replay_points_case(const char* row) { replay_case(row, true); }

struct Case { const char* row; void (*run)(const char*); };
} // namespace

int main() {
    const Case cases[] = {
        {"matrix_storage_populations", storage_case},
        {"matrix_snapshot_lifetimes", generic_snapshot_case},
        {"numeric_snapshot_lifetime", numeric_snapshot_case},
        {"replay_trade_retirement", replay_trades_case},
        {"replay_equity_retirement", replay_points_case},
    };
    for (const auto& item : cases) {
        try {
            item.run(item.row);
        } catch (const std::exception& error) {
            ROW_CHECK(item.row, false, error.what());
        } catch (...) {
            ROW_CHECK(item.row, false, "unknown exception in this row; continuing dispatch");
        }
    }
    std::printf("memory proof matrix_report_replay: %zu checks, %zu failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
