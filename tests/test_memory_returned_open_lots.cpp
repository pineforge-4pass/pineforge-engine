// open_lot_snapshot: the owning open-lot snapshot is sized by live lots and gives
// its allocations back when the caller drops it. No product write seam.
// Counts below come from the independent order script.
// No byte layout is pinned; allocation tracking observes ownership and release.
#define PINEFORGE_TEST_ALLOCATION_BYTES
#define PINEFORGE_TEST_ALLOCATION_CENSUS
#include "global_allocation_replacement.hpp"

#include <pineforge/native_host.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <vector>

namespace {
using namespace pineforge;
namespace no = pineforge::native_order;
constexpr std::int64_t kStart = 1736121600000LL;
constexpr std::size_t kPeriod = 8;
constexpr std::size_t kN = 32;
constexpr const char* kComment =
    "open_lot_snapshot owning snapshot release, including copied entry text";
constexpr const char* kA = "open_lot_snapshot first opening with independently tracked units";
constexpr const char* kB = "open_lot_snapshot second opening with independently tracked units";
constexpr const char* kC = "open_lot_snapshot third opening with independently tracked units";
constexpr const char* kTail = "open_lot_snapshot surviving opening at each period boundary";

struct Checks {
    const char* row = "open_lot_snapshot";
    std::size_t checks = 0, failures = 0, bars = 0, sample = 0;
    void check(bool ok, const char* file, int line, const char* expression) {
        ++checks;
        if (ok) return;
        ++failures;
        std::fprintf(stderr, "FAIL [%s] %s:%d: %s (bars=%zu sample=%zu)\n",
                     row, file, line, expression, bars, sample);
    }
};
#define MEM_CHECK(c, expression) \
    (c).check(bool(expression), __FILE__, __LINE__, #expression)

// Test-owned FIFO arithmetic. Its population is never copied from a host
// accessor, a callback's fill facts, or the returned snapshot.
struct ExpectedLot {
    const char* label = "";
    double units = 0;
};
struct Model {
    std::array<ExpectedLot, 3> lots{};
    std::size_t size = 0;
    void open(const char* label, double units) {
        lots[size++] = {label, units};
    }
    void reduce(double units) {
        while (size != 0 && units != 0) {
            const double taken = std::min(units, lots[0].units);
            units -= taken;
            lots[0].units -= taken;
            if (lots[0].units != 0) break;
            for (std::size_t i = 1; i < size; ++i) lots[i - 1] = lots[i];
            --size;
        }
    }
    void flatten() { size = 0; }
};

struct Recording {
    bool previous = global_allocation::recording;
    Recording() { global_allocation::recording = true; }
    ~Recording() { global_allocation::recording = previous; }
};

struct Result {
    std::size_t final_count = 0;
    std::size_t peak_count = 0;
    std::size_t samples = 0;
    std::size_t final_bytes_before = 0, final_bytes_after = 0;
    unsigned seen_counts = 0;
};

// The recording interval contains the API call, comparisons and destruction
// only. Engine state changes, model updates and tape allocations are outside it.
// Allocation tracking keeps a marked block charged after recording stops; no
// counter is reset, so blocks retained between intervals remain observable.
std::size_t observe(NativeStrategyHost& host, const Model& expected,
                    double mark, Checks& c, Result& result) {
    c.sample = ++result.samples;
    const auto before_bytes = global_allocation::live_bytes;
    const auto before_blocks = global_allocation::live_blocks;
    const auto before_allocations = global_allocation::recorded_allocations;
    const auto before_releases = global_allocation::recorded_releases;
    std::size_t count = 0;
    {
        Recording recording;
        {
            const auto rows = host.native_open_lots(mark);
            count = rows.size();
            // open_lot_snapshot COUNT: exactly one returned row per model live lot.
            MEM_CHECK(c, rows.size() == expected.size);
            result.peak_count = std::max(result.peak_count, rows.size());
            if (rows.size() <= 3) result.seen_counts |= 1U << rows.size();
            // Guard indexing after a failed count so a missing row cannot turn
            // a diagnostic into an out-of-range access or stop later checks.
            const auto common = std::min(rows.size(), expected.size);
            for (std::size_t i = 0; i < common; ++i) {
                MEM_CHECK(c, rows[i].ordinal == i);
                MEM_CHECK(c, rows[i].entry_label == expected.lots[i].label);
                MEM_CHECK(c, rows[i].entry_comment == kComment);
                MEM_CHECK(c, rows[i].signed_units == expected.lots[i].units);
                MEM_CHECK(c, rows[i].entry_price == 100.0);
                MEM_CHECK(c, rows[i].mark == mark);
            }
            // Instrumentation witness only: a nonempty owning vector must
            // really allocate, so release equality cannot pass on a dead hook.
            if (expected.size != 0) {
                MEM_CHECK(c, global_allocation::recorded_allocations > before_allocations);
                MEM_CHECK(c, global_allocation::live_bytes > before_bytes);
            }
        }
        // open_lot_snapshot RELEASE: zero bytes and blocks survive caller scope,
        // including the vector's entries and their separately copied text.
        MEM_CHECK(c, global_allocation::live_bytes == before_bytes);
        MEM_CHECK(c, global_allocation::live_blocks == before_blocks);
        MEM_CHECK(c, global_allocation::recorded_allocations - before_allocations
                    == global_allocation::recorded_releases - before_releases);
    }
    result.final_bytes_before = before_bytes;
    result.final_bytes_after = global_allocation::live_bytes;
    return count;
}

class Host final : public NativeStrategyHost {
public:
    Host(Checks& checks, Result& result) : c_(checks), result_(result) {}
    std::size_t calculations = 0, applied = 0;
    Model model;

    void on_native_applied(const no::ExecutionAppliedEvent&,
                           const NativeDecisionContext&) override {
        ++applied;
    }

    void on_native_bar(const Bar&, const NativeDecisionContext&) override {
        const auto index = calculations++;
        // Two calls without a fill between them also expose a buffer that
        // appends to a persistent previous answer instead of starting afresh.
        observe(*this, model, 100.0, c_, result_);
        observe(*this, model, 101.0, c_, result_);
        // AfterCalculation fills occur after this callback. The model below
        // predicts the next callback independently of admission/fill results.
        switch (index % kPeriod) {
        case 0:
            if (index != 0) {
                send({no::Flatten{}, "period-boundary close", ""});
                model.flatten();
            }
            break;
        case 1: open(kA, 1.0); break;
        case 2: open(kB, 2.0); break;
        case 3: open(kC, 3.0); break;
        case 4:
            send({no::Reduce{no::ExplicitUnits{1.5}}, "FIFO partial close", ""});
            // 1.5 removes A's 1 and half a unit of B: B=1.5, C=3.
            model.reduce(1.5);
            break;
        case 5:
            send({no::Flatten{}, "pyramid close", ""});
            model.flatten();
            break;
        case 6: open(kTail, 4.0); break;
        case 7: break;
        }
    }

private:
    void send(const no::Request& request) {
        const auto result = submit(request);
        MEM_CHECK(c_, result.status == no::SubmitStatus::Accepted);
        MEM_CHECK(c_, result.handle.has_value());
    }
    void open(const char* label, double units) {
        send({no::Transact{units}, label, kComment});
        model.open(label, units);
    }
    Checks& c_;
    Result& result_;
};

NativeRunSpec spec() {
    NativeRunSpec s;
    s.identity = {"open-lot-snapshot", 1};
    s.input_tf = s.script_tf = "1";
    s.tickerid = "TEST:LOTS";
    s.timezone = "UTC";
    s.session = "24x7";
    s.initial_capital = 10000;
    s.point_value = s.account_fx = 1;
    s.price_tick = 0.01;
    s.fee_kind = NativeFeeKind::CashPerExecution;
    s.fee_value = 0;
    s.close_execution = NativeCloseExecution::AfterCalculation;
    s.report_policy = NativeReportPolicy::KernelRecorded;
    return s;
}

Result run_script(std::size_t bars_count, Checks& c) {
    c.bars = bars_count;
    Result result;
    Host host(c, result);
    const auto setup = host.configure_native(spec());
    MEM_CHECK(c, setup.status == NativeSetupStatus::Applied);
    std::vector<Bar> bars;
    bars.reserve(bars_count);
    for (std::size_t i = 0; i < bars_count; ++i) {
        bars.push_back({100, 100, 100, 100, 1,
                        kStart + static_cast<std::int64_t>(i) * 60000});
    }
    host.run(bars.data(), static_cast<int>(bars.size()));
    MEM_CHECK(c, host.last_error().empty());
    MEM_CHECK(c, host.native_state().kind == NativeLifecycleKind::Completed);
    MEM_CHECK(c, host.calculations == bars_count);
    // Four opens, two full closes, one partial close per complete period;
    // only the first period lacks the boundary close of the preceding tail.
    MEM_CHECK(c, host.applied == 7 * (bars_count / kPeriod) - 1);
    result.final_count = observe(host, host.model, 99.0, c, result);
    // Last phase has no command: phase 6's +4 lot is the sole survivor.
    MEM_CHECK(c, host.model.size == 1);
    MEM_CHECK(c, result.final_count == 1);
    // The three successive openings are the independently derived peak;
    // the full and partial closes must exercise every count 0,1,2,3.
    MEM_CHECK(c, result.peak_count == 3);
    MEM_CHECK(c, result.seen_counts == ((1U << 4) - 1));
    MEM_CHECK(c, result.samples == 2 * bars_count + 1);
    return result;
}

void returned_live_lots_and_release(Checks& c) {
    const auto short_run = run_script(kN, c);
    const auto long_run = run_script(4 * kN, c);
    // open_lot_snapshot FIXED-AS-BARS-GROW: both complete-period runs end with
    // one +4 lot, reach three simultaneous lots, and retain zero read bytes.
    MEM_CHECK(c, short_run.final_count == long_run.final_count
                && short_run.final_count == 1);
    MEM_CHECK(c, short_run.peak_count == long_run.peak_count
                && short_run.peak_count == 3);
    MEM_CHECK(c, short_run.final_bytes_before == short_run.final_bytes_after
                && long_run.final_bytes_before == long_run.final_bytes_after);
    MEM_CHECK(c, short_run.final_bytes_after == 0 && long_run.final_bytes_after == 0);
}

struct Row {
    const char* id;
    void (*body)(Checks&);
};
} // namespace

int main() {
    Checks c;
    const Row rows[] = {{"open_lot_snapshot", returned_live_lots_and_release}};
    for (const auto& row : rows) {
        c.row = row.id;
        try {
            row.body(c);
        } catch (const std::exception& error) {
            MEM_CHECK(c, false);
            std::fprintf(stderr, "FAIL [%s] %s:%d: exception: %s\n",
                         row.id, __FILE__, __LINE__, error.what());
        } catch (...) {
            MEM_CHECK(c, false);
            std::fprintf(stderr, "FAIL [%s] %s:%d: unknown exception\n",
                         row.id, __FILE__, __LINE__);
        }
    }
    std::printf("memory proof returned_open_lots: %zu checks, %zu failures\n", c.checks, c.failures);
    return c.failures == 0 ? 0 : 1;
}
