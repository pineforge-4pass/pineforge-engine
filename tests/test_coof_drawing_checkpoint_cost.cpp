// K-DRAWSNAP: a drawing script's calc_on_order_fills checkpoint costs O(bars).
//
// The shape is the lane's synthetic reproduction, synth-label-coof.pine
// (CG-POPFIX round 3), which alone took the verifier's 300 s budget on the
// 222,295-bar ETH lane:
//
//   //@version=6
//   strategy("PF synth label-coof", overlay = true, calc_on_order_fills = true,
//            max_labels_count = 100, max_lines_count = 100)
//   var label live = na
//   label.delete(live)
//   live := label.new(bar_index, high, "x")
//
// One label is live at any time and every bar leaves one more tombstone. The
// host below is that script lowered the way the code generator lowers it --
// its state checkpointed by value, the four drawing arenas included -- plus
// a market entry every tenth bar and a close five bars later, so the run also
// takes fill recalculations. A second label arena of a record kind that counts
// its copies runs the same churn beside the real one: its count is the cost
// the checkpoint pays, measured without a clock. The heap blocks the
// checkpoint hooks allocate are counted as well (a copy that cloned the
// arena's block tree instead of sharing it would copy no record).
//
// The bound: every script execution writes records in at most two blocks of
// 32 that a checkpoint shares, so the run copies at most 64 records per
// execution, however many records it has created, and N -> 2N bars at most
// doubles the copies (ratio <= 2.25 with slack); the hooks allocate only the
// order_ copies, at most cap ids each. Before K-DRAWSNAP each checkpoint
// copied every record ever allocated: 27,195,600 records for 4,000 bars and
// 108,791,200 for 8,000 here, a ratio of 4.
#define PINEFORGE_TEST_ALLOCATION_BYTES
#include "global_allocation_replacement.hpp"

#include <pineforge/bar.hpp>
#include <pineforge/drawing.hpp>
#include <pineforge/source/pine_strategy_host.hpp>

#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <optional>
#include <string>
#include <vector>

using namespace pineforge;

static int tests_passed = 0;
static int tests_failed = 0;

#define CHECK(expr)                                                            \
    do {                                                                       \
        if (!(expr)) {                                                         \
            std::printf("  FAIL  %s:%d  %s\n", __FILE__, __LINE__, #expr);     \
            ++tests_failed;                                                    \
        } else {                                                               \
            ++tests_passed;                                                    \
        }                                                                      \
    } while (0)

namespace {

std::size_t g_hook_bytes = 0;
void count_hook_bytes(std::size_t size) { g_hook_bytes += size; }

// Records what one checkpoint hook allocates.
struct HookWindow {
    std::size_t start = global_allocation::allocations;
    std::size_t& allocations;
    explicit HookWindow(std::size_t& sink) : allocations(sink) {
        global_allocation::on_recorded = count_hook_bytes;
        global_allocation::recording = true;
    }
    ~HookWindow() {
        global_allocation::recording = false;
        global_allocation::on_recorded = nullptr;
        allocations += global_allocation::allocations - start;
    }
};

// A label record whose copies are counted; moves are not copies.
struct CountedRec {
    int64_t x = 0;
    double y = 0.0;
    bool alive = false;
    static inline std::uint64_t copies = 0;

    CountedRec() = default;
    CountedRec(int64_t x_, double y_) : x(x_), y(y_) {}
    CountedRec(const CountedRec& o) : x(o.x), y(o.y), alive(o.alive) { ++copies; }
    CountedRec& operator=(const CountedRec& o) {
        x = o.x; y = o.y; alive = o.alive;
        ++copies;
        return *this;
    }
    CountedRec(CountedRec&&) noexcept = default;
    CountedRec& operator=(CountedRec&&) noexcept = default;
};

class LabelChurnHost final : public source::PineStrategyHost {
public:
    Label live;
    int32_t counted_live = -1;
    DrawingArena<LineRec> _pf_lines_{100};
    DrawingArena<BoxRec> _pf_boxes_{50};
    DrawingArena<LabelRec> _pf_labels_{100};
    DrawingArena<LinefillRec> _pf_linefills_{50};
    DrawingArena<CountedRec> counted_labels{100};
    bool _var_initialized = false;

    struct State {
        Label live;
        int32_t counted_live;
        DrawingArena<LineRec> lines;
        DrawingArena<BoxRec> boxes;
        DrawingArena<LabelRec> labels;
        DrawingArena<LinefillRec> linefills;
        DrawingArena<CountedRec> counted;
        bool var_initialized;
    };
    std::optional<State> checkpoint_;
    int executions = 0;
    int hook_calls = 0;
    std::size_t hook_allocations = 0;

    LabelChurnHost() {
        source::PineStrategyConfig cfg{};
        cfg.calc_on_order_fills = true;
        configure_pine_strategy(cfg);
    }

    void snapshot_script_state() override {
        ++hook_calls;
        HookWindow window(hook_allocations);
        checkpoint_.emplace(State{live, counted_live, _pf_lines_, _pf_boxes_, _pf_labels_,
                                  _pf_linefills_, counted_labels, _var_initialized});
    }

    void restore_script_state() override {
        if (!checkpoint_) return;
        ++hook_calls;
        HookWindow window(hook_allocations);
        live = checkpoint_->live;
        counted_live = checkpoint_->counted_live;
        _pf_lines_ = checkpoint_->lines;
        _pf_boxes_ = checkpoint_->boxes;
        _pf_labels_ = checkpoint_->labels;
        _pf_linefills_ = checkpoint_->linefills;
        counted_labels = checkpoint_->counted;
        _var_initialized = checkpoint_->var_initialized;
    }

    void commit_script_state() override { snapshot_script_state(); }

    void on_source_bar(const Bar&) override {
        ++executions;
        if (!_var_initialized) _var_initialized = true;
        pf_label_delete(_pf_labels_, live);
        live = pf_label_new(_pf_labels_, pine_bar_index(), current_bar_.high, std::string("x"),
                            XLoc::bar_index, YLoc::price);
        counted_labels.erase(counted_live);
        counted_live = counted_labels.alloc(CountedRec{pine_bar_index(), current_bar_.high});
        const int bar = pine_bar_index();
        if (bar % 10 == 0 && signed_position_size() == 0.0) strategy_entry("L", true);
        if (bar % 10 == 5) strategy_close("L");
    }
};

std::vector<Bar> churn_feed(int n) {
    std::vector<Bar> bars;
    bars.reserve(static_cast<std::size_t>(n));
    for (int i = 0; i < n; ++i) {
        const double mid = 100.0 + 10.0 * std::sin(i * 0.05);
        Bar b{};
        b.timestamp = 1'700'000'100'000LL + static_cast<std::int64_t>(i) * 900'000;
        b.open = mid;
        b.high = mid + 1.5;
        b.low = mid - 1.5;
        b.close = mid + 0.5;
        b.volume = 1000.0;
        bars.push_back(b);
    }
    return bars;
}

struct ChurnRun {
    std::uint64_t copies = 0;
    int hook_calls = 0;
    std::size_t hook_allocations = 0;
    std::size_t hook_bytes = 0;
    int executions = 0;
    int trades = 0;
    int32_t labels_created = 0;
    std::size_t labels_live = 0;
    double seconds = 0.0;
    std::string error;
};

ChurnRun run_churn(int n) {
    const std::vector<Bar> bars = churn_feed(n);
    LabelChurnHost host;
    CountedRec::copies = 0;
    g_hook_bytes = 0;
    const auto t0 = std::chrono::steady_clock::now();
    host.run(bars.data(), n, "15", "15", false);
    const auto t1 = std::chrono::steady_clock::now();
    ChurnRun r;
    r.copies = CountedRec::copies;
    r.hook_calls = host.hook_calls;
    r.hook_allocations = host.hook_allocations;
    r.hook_bytes = g_hook_bytes;
    r.executions = host.executions;
    r.trades = host.trade_count();
    r.labels_created = host._pf_labels_.size();
    r.labels_live = host._pf_labels_.order().size();
    r.seconds = std::chrono::duration<double>(t1 - t0).count();
    r.error = host.last_error();
    return r;
}

void test_label_churn_checkpoint_scales_linearly() {
    std::printf("test_label_churn_checkpoint_scales_linearly\n");
    const int n = 4000;
    const ChurnRun a = run_churn(n);
    const ChurnRun b = run_churn(2 * n);
    const double ratio = a.copies ? static_cast<double>(b.copies) / static_cast<double>(a.copies) : 0.0;
    std::printf("  %5d bars: %9llu record copies over %5d executions, %3d trades, %.3f s\n",
                n, (unsigned long long)a.copies, a.executions, a.trades, a.seconds);
    std::printf("  %5d bars: %9llu record copies over %5d executions, %3d trades, %.3f s\n",
                2 * n, (unsigned long long)b.copies, b.executions, b.trades, b.seconds);
    std::printf("  ratio %.3f (bound 2.25); per execution %.1f and %.1f (bound 64)\n", ratio,
                a.executions ? static_cast<double>(a.copies) / a.executions : 0.0,
                b.executions ? static_cast<double>(b.copies) / b.executions : 0.0);
    const double byte_ratio = a.hook_bytes
        ? static_cast<double>(b.hook_bytes) / static_cast<double>(a.hook_bytes) : 0.0;
    std::printf("  checkpoint hooks: %d calls allocate %zu blocks / %zu bytes, then %d calls "
                "%zu / %zu; byte ratio %.3f\n", a.hook_calls, a.hook_allocations, a.hook_bytes,
                b.hook_calls, b.hook_allocations, b.hook_bytes, byte_ratio);
    CHECK(a.error.empty());
    CHECK(b.error.empty());
    // The run took fill recalculations: more executions than bars.
    CHECK(a.executions > n);
    CHECK(a.trades > 0);
    // The real label arena kept one tombstone per bar and one live label.
    CHECK(a.labels_created == n);
    CHECK(a.labels_live == 1u);
    CHECK(a.copies <= 64ULL * static_cast<std::uint64_t>(a.executions));
    CHECK(b.copies <= 64ULL * static_cast<std::uint64_t>(b.executions));
    CHECK(a.copies > 0 && ratio <= 2.25);
    CHECK(a.hook_bytes > 0 && byte_ratio <= 2.25);
    // Five order_ copies of at most 100 ids per hook, never the records.
    CHECK(b.hook_bytes <= 5u * 8192u * static_cast<std::size_t>(b.hook_calls));
}

}  // namespace

int main() {
    test_label_churn_checkpoint_scales_linearly();
    std::printf("\n%d passed, %d failed\n", tests_passed, tests_failed);
    return tests_failed == 0 ? 0 : 1;
}
