// B2-14 RED: a stream snapshot folds a pending hash leaf, then the source
// host may rewrite that same leaf before the stream finishes. The rewritten
// sequence must match a control run that never took the snapshot.
#include <pineforge/native_host.hpp>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <vector>

namespace {
using namespace pineforge;

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

std::vector<Bar> tape() {
    std::vector<Bar> result;
    for (int index = 0; index < 8; ++index) {
        const double price = 100.0 + index * 0.25;
        result.push_back({price, price + 0.5, price - 0.5, price + 0.125,
                          1.0, INT64_C(1704067200000) + index * INT64_C(60000)});
    }
    return result;
}

class HashHost final : public NativeStrategyHost {
public:
    explicit HashHost(std::uint64_t run_number) {
        NativeRunSpec spec;
        spec.identity = {"p2-hash-rewrite", run_number};
        spec.input_tf = spec.script_tf = "1";
        spec.ticker = "P2";
        spec.tickerid = "TEST:P2";
        spec.timezone = "UTC";
        spec.session = "24x7";
        spec.initial_capital = 10000.0;
        spec.point_value = spec.account_fx = 1.0;
        spec.price_tick = 0.25;
        spec.report_policy = NativeReportPolicy::KernelRecorded;
        require(configure_native(spec).status == NativeSetupStatus::Applied,
                "hash rewrite fixture configure refused");
        set_broker_state_hash_recording(true);
        require(set_optional_report_mode(report_sequence::ReportMode::SummaryOnly) == 0,
                "hash rewrite fixture summary mode refused");
    }

    void on_native_bar(const Bar&, const NativeDecisionContext&) override {}

    void rewrite_last_hash(std::uint64_t value) {
        require(!broker_state_hashes_.empty(), "hash rewrite fixture has no pending leaf");
        broker_state_hashes_.back() = value;
    }
};

void finish(HashHost& host, const std::vector<Bar>& bars) {
    for (std::size_t index = 2; index < bars.size(); ++index)
        require(host.stream_push_bar(bars[index]), "stream push failed");
    require(host.stream_end(false), "stream end failed");
    ReportC report{};
    host.fill_report(&report);
    BacktestEngine::free_report(&report);
}

} // namespace

int main() {
    try {
        const auto bars = tape();
        HashHost with_snapshot(1), without_snapshot(1);
        require(with_snapshot.stream_begin(bars.data(), 2, "1", "1"),
                "snapshot stream begin failed");
        require(without_snapshot.stream_begin(bars.data(), 2, "1", "1"),
                "control stream begin failed");

        constexpr std::uint64_t stale = UINT64_C(0x1111222233334444);
        constexpr std::uint64_t rewritten = UINT64_C(0xaaaabbbbccccdddd);
        with_snapshot.rewrite_last_hash(stale);
        ReportC snapshot{};
        with_snapshot.fill_report(&snapshot);
        BacktestEngine::free_report(&snapshot);

        // This is the source host's later in-place rewrite of the leaf that
        // the snapshot already folded.
        with_snapshot.rewrite_last_hash(rewritten);
        without_snapshot.rewrite_last_hash(rewritten);
        finish(with_snapshot, bars);
        finish(without_snapshot, bars);

        const auto left = with_snapshot.optional_sequence_summary();
        const auto right = without_snapshot.optional_sequence_summary();
        require(left.hashes.count == right.hashes.count,
                "snapshot changed the hash witness count");
        require(left.hashes.digest == right.hashes.digest,
                "snapshot left a stale hash leaf in the final witness");
        std::puts("B2-14 rewritten hash leaf after stream snapshot: PASS");
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "B2-14: %s\n", error.what());
        return 1;
    }
}
