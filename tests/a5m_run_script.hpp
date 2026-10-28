#pragma once

// A script laid out the way the code generator lays out a Pine script with
// `var` matrices (the generator's golden matrix_eigen_pca.cpp): matrix members
// default-constructed (na), reset in prepare_script_run, created on the first
// bar, and checkpointed through Snapshot/restore for calc_on_order_fills, with
// commit_script_state taking a new checkpoint. It installs no matrix Runtime:
// its matrices live wherever the engine's run puts them.
//
// Every execution appends one row to three growing matrices (numeric, int,
// bool), rewrites a 2x2 numeric matrix and reads its spectrum (the corpus
// matrix strategies' shape), and makes and reads two fixed-size temporaries.
// Expectations are closed-form functions of the bar index (value_at,
// boolean_at) and an input-derived row counter, never matrix reads.

#include "a5m_growth_fixture.hpp"

#include <pineforge/source/pine_strategy_host.hpp>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace a5m_run {

namespace ms = pineforge::detail::matrix_store;
using a5m_test::require;
using a5m_growth::boolean_at;
using a5m_growth::value_at;

// A check whose failure leaves matrices over a closed Runtime: unwinding would
// destroy them (a use after free), so it reports and leaves at once.
inline void require_or_exit(bool ok, const char* why) {
    if (ok) return;
    std::fprintf(stderr, "A5M run: %s\n", why);
    std::fflush(stderr);
    std::_Exit(1);
}

// value_at() steps by eighths, so eight times it is an exact integer.
inline int tick_at(int bar, int column) { return static_cast<int>(value_at(bar, column) * 8.0); }

// The payload bytes one bar's rows add: two doubles, two ints, four flags.
inline constexpr std::uint64_t kRowBytes = 2 * sizeof(double) + 2 * sizeof(int) + 4;
// The matrices a run's first bar creates: closes, ticks, mask and cov.
inline constexpr std::uint64_t kScriptMatrices = 4;

inline std::vector<pineforge::Bar> tape(int count) {
    std::vector<pineforge::Bar> bars(static_cast<std::size_t>(count));
    for (int i = 0; i < count; ++i) {
        const double close = value_at(i, 0);
        bars[static_cast<std::size_t>(i)] =
            {close, close + 1.0, close - 1.0, close, 1000.0,
             INT64_C(1736121600000) + static_cast<std::int64_t>(i) * 60000};
    }
    return bars;
}

// Where the script expects its matrices: the run's own Runtime (the production
// install), or where the caller chose explicitly: native matrices, or a
// Runtime of its own (Options::chosen).
enum class Expect { run_store, explicit_native, explicit_runtime };

struct Options {
    bool coof = false;
    int trade_every = 0;  // 0: no orders
    int warmup = 2;       // executions before the heap windows count
    bool heap_windows = false;
    Expect expect = Expect::run_store;
    const ms::Runtime* chosen = nullptr; // Expect::explicit_runtime
};

class Script final : public pineforge::source::PineStrategyHost {
public:
    // Generated-style `var` state.
    pineforge::PineMatrix closes;
    pineforge::PineGenericMatrix<int> ticks;
    pineforge::PineGenericMatrix<bool> mask;
    pineforge::PineMatrix cov;
    bool var_initialized = false;
    int rows = 0; // input-derived: the rows the script has appended
    // Set by a test: the next execution makes its call rollback-only (poison).
    bool poison_next = false;

    // Observations, read by the tests after a run.
    int executions = 0, snapshots = 0, restores = 0, lifecycles = 0;
    std::int64_t retained = 0, peak = 0;
    std::size_t requested_peak = 0;
    // The run's Runtime as the lifecycle's first execution saw it; the
    // previous lifecycle's; and what that first execution found of it.
    std::weak_ptr<ms::Runtime> runtime;
    std::weak_ptr<ms::Runtime> previous_runtime;
    bool previous_closed_at_first_bar = false;
    bool previous_same_at_first_bar = false;
    std::uint64_t objects_at_first_bar = 0;

    explicit Script(const Options& options) : options_(options) {
        pineforge::source::PineStrategyConfig config{};
        config.calc_on_order_fills = options.coof;
        configure_pine_strategy(config);
    }

    void prepare_script_run(const pineforge::Bar*, int, bool) override {
        checkpoint_.reset();
        closes = decltype(closes){};
        ticks = decltype(ticks){};
        mask = decltype(mask){};
        cov = decltype(cov){};
        var_initialized = false;
        rows = 0;
        ++lifecycles;
        previous_runtime = runtime;
        runtime.reset();
        first_bar_ = true;
    }

    void snapshot_script_state() override {
        ++snapshots;
        checkpoint_.emplace(Checkpoint{take(closes), take(ticks), take(mask), take(cov),
                                       var_initialized, rows});
    }
    void commit_script_state() override { snapshot_script_state(); }
    void restore_script_state() override {
        if (!checkpoint_) return;
        ++restores;
        put(closes, checkpoint_->closes);
        put(ticks, checkpoint_->ticks);
        put(mask, checkpoint_->mask);
        put(cov, checkpoint_->cov);
        var_initialized = checkpoint_->var_initialized;
        rows = checkpoint_->rows;
        if (var_initialized) verify_rows("calc-on-fill restore lost or kept a row");
    }

    void on_source_bar(const pineforge::Bar& bar) override {
        ++executions;
        const int index = pine_bar_index();
        require(bar.close == value_at(index, 0), "synthetic bar sequence changed");
        observe_runtime();
        if (options_.heap_windows && executions > options_.warmup) {
            // Only executions after the warmup are recorded: the run's Store
            // opens (its fixed workspace) on the first one.
            const auto before = a5m_growth::heap_bytes();
            {
                a5m_growth::OperationWindow window(requested_peak);
                execute(index);
            }
            retained += a5m_growth::heap_bytes() - before;
            peak = std::max(peak, retained);
        } else {
            execute(index);
        }
        if (options_.trade_every) {
            // The decision reads the matrices, so a changed cell changes trades.
            const bool rising = closes.get(rows - 1, 0) > closes.get(0, 0);
            if (index % options_.trade_every == 1 && rising && live_position_size() == 0)
                strategy_entry("L", true);
            if (index % options_.trade_every == options_.trade_every / 2 + 1 &&
                live_position_size() > 0)
                strategy_close("L");
        }
    }

    // The committed population after a run: one exact row per bar.
    void verify_final(int bars) const {
        require(rows == bars, "the script's row counter does not match the bar count");
        verify_rows("a rolled-back execution left a row behind, or one was lost");
        verify_cells(bars);
    }
    // Rows [0, count) hold exactly their bars' values.
    void verify_cells(int count) const {
        require(closes.rows() == count && ticks.rows() == count && mask.rows() == count,
                "the matrices do not hold the expected rows");
        for (int r = 0; r < count; ++r) {
            for (int c = 0; c < 2; ++c) {
                const double expected = value_at(r, c);
                const double actual = closes.get(r, c);
                require(std::memcmp(&expected, &actual, sizeof actual) == 0,
                        "a numeric cell's bytes changed");
                require(ticks.get(r, c) == tick_at(r, c), "an integer cell changed");
            }
            for (int c = 0; c < 4; ++c)
                require(mask.get(r, c) == boolean_at(r, c), "a boolean cell changed");
        }
    }

    // Every matrix in the place the script expects (Expect).
    void verify_placement() const {
        const bool external = options_.expect != Expect::explicit_native;
        require(closes.is_external_storage() == external &&
                    ticks.is_external_storage() == external &&
                    mask.is_external_storage() == external &&
                    cov.is_external_storage() == external,
                external ? "a run matrix fell back to the heap"
                         : "an explicit native choice was overridden by the run");
    }

    // A fingerprint of the trades, to hold two runs' results equal.
    std::string fingerprint() const {
        std::uint64_t hash = UINT64_C(1469598103934665603);
        const auto fold = [&hash](const void* bytes, std::size_t size) {
            const auto* p = static_cast<const unsigned char*>(bytes);
            for (std::size_t i = 0; i < size; ++i) hash = (hash ^ p[i]) * UINT64_C(1099511628211);
        };
        for (const pineforge::Trade& t : trades_) {
            fold(&t.entry_time, sizeof t.entry_time);
            fold(&t.exit_time, sizeof t.exit_time);
            fold(&t.entry_price, sizeof t.entry_price);
            fold(&t.exit_price, sizeof t.exit_price);
            fold(&t.qty, sizeof t.qty);
            fold(&t.pnl, sizeof t.pnl);
        }
        char text[96];
        std::snprintf(text, sizeof text, "trades=%zu fnv=%016llx", trades_.size(),
                      static_cast<unsigned long long>(hash));
        return text;
    }

private:
    struct Checkpoint {
        std::optional<pineforge::PineMatrix::Snapshot> closes;
        std::optional<pineforge::PineGenericMatrix<int>::Snapshot> ticks;
        std::optional<pineforge::PineGenericMatrix<bool>::Snapshot> mask;
        std::optional<pineforge::PineMatrix::Snapshot> cov;
        bool var_initialized;
        int rows;
    };
    template <class Matrix>
    static std::optional<typename Matrix::Snapshot> take(const Matrix& value) {
        if (value.is_na()) return std::nullopt;
        return value.snapshot();
    }
    template <class Matrix>
    static void put(Matrix& value, const std::optional<typename Matrix::Snapshot>& snapshot) {
        if (!snapshot) {
            value = Matrix{};
            return;
        }
        value.restore(*snapshot);
    }

    void observe_runtime() {
        ms::Runtime* current = ms::current_runtime();
        if (options_.expect == Expect::explicit_native) {
            require(current == nullptr, "an explicit native choice was overridden by the run");
            return;
        }
        if (options_.expect == Expect::explicit_runtime) {
            require(current != nullptr && current == options_.chosen,
                    "an explicit Runtime was overridden by the run");
            return;
        }
        require(current != nullptr, "no matrix Runtime is current inside the run");
        const std::shared_ptr<ms::Runtime> shared = current->weak_from_this().lock();
        require(shared != nullptr, "the run's matrix Runtime is not run-owned");
        if (first_bar_) {
            first_bar_ = false;
            previous_same_at_first_bar = shared == previous_runtime.lock();
            previous_closed_at_first_bar = previous_runtime.expired();
            runtime = shared;
            return;
        }
        require(shared == runtime.lock(), "the run's matrix Runtime changed inside the run");
    }

    void verify_rows(const char* why) const {
        require(closes.rows() == rows && ticks.rows() == rows && mask.rows() == rows &&
                    (rows == 0 || (closes.columns() == 2 && ticks.columns() == 2 &&
                                   mask.columns() == 4)),
                why);
        if (rows == 0) return;
        const int last = rows - 1;
        const double expected = value_at(last, 1);
        const double actual = closes.get(last, 1);
        require(std::memcmp(&expected, &actual, sizeof actual) == 0 &&
                    ticks.get(last, 0) == tick_at(last, 0) &&
                    mask.get(last, 3) == boolean_at(last, 3),
                why);
    }

    // One execution's matrix work. A recalculation that starts from the bar's
    // committed close execution already holds this bar's row; its own append
    // is speculative and the engine rolls it back.
    void execute(int index) {
        if (!var_initialized) {
            closes = pineforge::PineMatrix::new_(0, 2);
            ticks = pineforge::PineGenericMatrix<int>::new_(0, 0, 0);
            mask = pineforge::PineGenericMatrix<bool>::new_(0, 0);
            cov = pineforge::PineMatrix::new_(2, 2, 0.0);
            var_initialized = true;
            verify_placement();
            if (const ms::Runtime* current = ms::current_runtime())
                objects_at_first_bar = current->store().stats().objects_live;
        }
        require(rows == index || rows == index + 1, "an execution started from a foreign row count");
        verify_rows("an execution started from a changed matrix");
        closes.add_row(rows, {value_at(index, 0), value_at(index, 1)});
        ticks.add_row(rows, {tick_at(index, 0), tick_at(index, 1)});
        mask.add_row(rows, {boolean_at(index, 0), boolean_at(index, 1),
                            boolean_at(index, 2), boolean_at(index, 3)});
        ++rows;
        if (poison_next) {
            poison_next = false;
            poison();
        }
        // The corpus covariance shape: rewrite a symmetric 2x2, read its spectrum.
        const double spread = value_at(index, 1) - value_at(index, 0);
        cov.set(0, 0, 1.0 + spread);
        cov.set(0, 1, spread);
        cov.set(1, 0, spread);
        cov.set(1, 1, 1.0 + spread);
        const std::vector<double> spectrum = cov.eigenvalues();
        require(spectrum.size() == 2 && spectrum[0] >= spectrum[1],
                "the covariance spectrum was not read");
        // Temporaries of fixed size, made and dropped inside the execution.
        const auto flipped = cov.transpose();
        const auto column = mask.submatrix(rows - 1, rows, 0, 4).transpose();
        require(flipped.get(0, 1) == spread && column.rows() == 4 && column.columns() == 1 &&
                    column.get(2, 0) == boolean_at(index, 2),
                "a temporary matrix read a wrong cell");
    }

    // A failure inside the call: a host scope on the run's Store that rolls
    // back joins the call's transaction and leaves it rollback-only, so the
    // next matrix operation is refused and the run fails.
    void poison() {
        ms::Runtime* current = ms::current_runtime();
        require(current != nullptr, "no matrix Runtime is current for the failing call");
        auto transaction = current->store().begin();
        transaction->rollback();
    }

    Options options_;
    std::optional<Checkpoint> checkpoint_;
    bool first_bar_ = true;
};

} // namespace a5m_run
