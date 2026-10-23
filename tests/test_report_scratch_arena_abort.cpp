// CR150-CX: generated wrappers call run() and then fill_report(). A failed
// curve flush must remain a typed run failure through that second call.
// Static read only here; fail-before and execution belong to the remote lab.
#include "report_sink_failure_fixture.hpp"

#include <pineforge/detail/scratch_arena.hpp>
#include <pineforge/native_host.hpp>

namespace {
using namespace cr150_w1;
using pineforge::detail::ScratchArena;
constexpr auto chunk = ScratchArena::kChunkBytes;

struct ArenaAttachment {
    Fd backing;
    ArenaAttachment(Directory& directory, std::uint64_t bytes)
        : backing(::open(directory.file("arena").c_str(), O_CREAT | O_EXCL | O_RDWR, 0600)) {
        require(backing.value >= 0
                    && ::ftruncate(backing.value, static_cast<off_t>(bytes)) == 0,
                "prepare real arena backing");
        ScratchArena::attach(backing.value, bytes);
    }
    ~ArenaAttachment() { ScratchArena::detach(); }
};

class FlatHost final : public pineforge::NativeStrategyHost {
public:
    int callbacks = 0;
    FlatHost() {
        pineforge::NativeRunSpec spec;
        spec.identity = {"arena-abort", 1};
        spec.input_tf = spec.script_tf = "1";
        spec.tickerid = "TEST:ARENA";
        spec.timezone = "UTC";
        spec.session = "24x7";
        spec.initial_capital = 1000.0;
        spec.point_value = spec.account_fx = 1.0;
        spec.price_tick = 0.01;
        spec.report_policy = pineforge::NativeReportPolicy::KernelRecorded;
        require(configure_native(spec).status == pineforge::NativeSetupStatus::Applied,
                "configure flat curve host");
    }
    void on_native_bar(const pineforge::Bar&, const pineforge::NativeDecisionContext&) override {
        ++callbacks;
    }
};

void run_then_fill(const std::vector<pineforge::Bar>& bars, bool exhausted) {
    Directory directory;
    ArenaAttachment attachment(directory, (exhausted ? 1 : 2) * chunk);
    FlatHost host;
    install_path(host, directory.file("report.ndjson"), directory.path);
    // Do not use the fixture's run helper: it catches exceptions escaping run.
    host.run(bars.data(), static_cast<int>(bars.size()), "1", "1", false, 4,
             pineforge::MagnifierDistribution::ENDPOINTS);
    const auto snapshot = ScratchArena::attached()->stats();
    const std::string original_text = host.last_error();
    const auto expected_args = exhaustion_args(chunk, chunk, chunk, chunk, "curve");
    if (exhausted) {
        require(host.callbacks > 0 && host.callbacks < static_cast<int>(bars.size()),
                "curve exhaustion interrupts a real run");
        expect_engine_failure(host, expected_args);
        require(original_text.find("scratch arena exhausted") != std::string::npos,
                "run retains the arena diagnostic");
        require(snapshot.bytes_used == chunk && snapshot.high_water == chunk,
                "curve really filled the one-chunk arena");
    } else {
        require(host.callbacks == static_cast<int>(bars.size())
                    && host.last_run_status() == 0 && original_text.empty(),
                "same input completes in the two-chunk control");
    }

    for (int read = 0; read < 2; ++read) {
        pineforge::ReportC report{};
        host.fill_report(&report);
        const bool empty = report.equity_curve == nullptr && report.equity_curve_len == 0
            && report.trades == nullptr && report.total_trades == 0
            && report.input_bars_processed == 0 && report.script_bars_processed == 0;
        pineforge::BacktestEngine::free_report(&report);
        if (exhausted) {
            require(empty, "failed storage supplies no completed report");
            expect_engine_failure(host, expected_args);
            require(host.last_error() == original_text,
                    "report read preserves the original typed run failure");
            const auto after = ScratchArena::attached()->stats();
            require(after.bytes_used == snapshot.bytes_used && after.high_water == snapshot.high_water,
                    "report read leaves arena usage and high-water counters unchanged");
        } else {
            pf_report_sink_receipt_v1 receipt{sizeof receipt, 1};
            require(host.last_run_status() == 0 && host.last_error().empty()
                        && host.report_sink_receipt_v1(&receipt) == 0 && receipt.complete
                        && receipt.count_e == static_cast<std::int64_t>(bars.size()),
                    "control publishes its complete curve and success trailer");
        }
    }
}
} // namespace

int main() {
    try {
        const auto bars = tape(static_cast<int>(2 * chunk / sizeof(pf_equity_point_t)));
        run_then_fill(bars, true);
        run_then_fill(bars, false);
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "test_report_scratch_arena_abort: %s\n", error.what());
        return 1;
    }
}
