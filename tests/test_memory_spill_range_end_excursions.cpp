// Range-end excursion storage follows open lots and report-source lifetime.
// Complete OpenReportRowSource::OwnedExcursions with the actual implementation,
// through direct implementation inclusion. The
// ordinary static-library link supplies the remaining engine implementation.
// Private members are read through explicit instantiation, never a layout
// mirror, injected storage, product accessor, or changed class definition.
#include "../src/engine_report.cpp"
#include <pineforge/native_host.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <memory>
#include <string>
#include <vector>

namespace memory_spill_range_end_excursions {
using namespace pineforge;
namespace no = pineforge::native_order;
using Sequence = detail::ScratchSequence<ClosedLotExcursion>;

template<class Tag, auto Member> struct Access {
    friend auto access(Tag) { return Member; }
};
struct Excursions { friend auto access(Excursions); };
struct File { friend auto access(File); };
struct Window { friend auto access(Window); };
struct Loaded { friend auto access(Loaded); };
template struct Access<Excursions, &OpenReportRowSource::excursions_>;
template struct Access<File, &Sequence::file_>;
template struct Access<Window, &Sequence::window_>;
template struct Access<Loaded, &Sequence::loaded_>;

std::size_t checks = 0, failures = 0;
const char* trial = "";

bool check(bool condition, const char* expression, const char* file, int line) {
    ++checks;
    if (!condition) {
        ++failures;
        std::fprintf(stderr, "FAIL [range-end] %s:%d: %s (%s)\n",
                     file, line, expression, trial);
    }
    return condition;
}
#define MEM_CHECK(expression) \
    ::memory_spill_range_end_excursions::check(bool(expression), #expression, __FILE__, __LINE__)

template<class F> void run_trial(const char* name, F body) {
    trial = name;
    try { body(); }
    catch (const std::exception& error) {
        check(false, "trial completed without exception", __FILE__, __LINE__);
        std::fprintf(stderr, "range-end exception (%s): %s\n", trial, error.what());
    }
    catch (...) {
        check(false, "trial completed without unknown exception", __FILE__, __LINE__);
    }
}

// Source arithmetic: ScratchSequence divides a 64*1024 byte window by
// sizeof(T), and appends one T for each terminal open lot. These are NOT
// caps on the number of lots or the backing file.
constexpr std::size_t window_bytes = 64u * 1024u;
constexpr std::size_t window_rows = window_bytes / sizeof(ClosedLotExcursion);
constexpr std::int64_t first_time = 1704067200000LL;

std::vector<Bar> tape(int count) {
    std::vector<Bar> bars;
    for (int i = 0; i < count; ++i)
        bars.push_back({100.0, 100.0, 100.0, 100.0, 1000.0,
                        first_time + std::int64_t(i) * 60000});
    return bars;
}

NativeRunSpec spec(bool report) {
    NativeRunSpec value;
    value.identity = {"range-end", 1};
    value.input_tf = "1";
    value.script_tf = "1";
    value.tickerid = "TEST:range-end";
    value.timezone = "UTC";
    value.session = "24x7";
    value.initial_capital = 1e9;
    value.point_value = 1.0;
    value.account_fx = 1.0;
    value.price_tick = 0.01;
    value.close_execution = NativeCloseExecution::AfterCalculation;
    value.report_policy = NativeReportPolicy::KernelRecorded;
    value.report_open_position_at_end = report;
    return value;
}

class Host final : public NativeStrategyHost {
public:
    Host(std::size_t lots, bool owns) : requested_(lots), owns_(owns) {}

    void on_native_bar(const Bar&, const NativeDecisionContext&) override {
        if (submitted_) return;
        submitted_ = true;
        for (std::size_t i = 0; i < requested_; ++i) {
            const double units = double(1 + i % 3);
            const auto result = submit({no::Transact{units}, "range-end-open", ""});
            if (!MEM_CHECK(result.status == no::SubmitStatus::Accepted && result.handle))
                continue;
            // Reference model comes from accepted command identities and the
            // submitted quantities, before settlement and scratch insertion.
            handles.push_back(*result.handle);
            quantities.push_back(units);
            expected.push_back({double(result.handle->incarnation) * 8.0 + units / 4.0,
                                double(result.handle->incarnation) * 4.0 + units / 2.0});
        }
    }
    bool owns_lot_excursions() const noexcept override { return owns_; }
    ClosedLotExcursion closed_lot_excursion(const ClosedLotExcursionFacts& f) const override {
        ++hook_calls;
        // A deterministic host answer, computed from actual fill facts.
        // Quarter units and integer identities keep both columns exact.
        return {8.0 * double(f.entry_incarnation) + 0.25 * f.closed_qty + phase,
                4.0 * double(f.entry_incarnation) + 0.5 * f.closed_qty + phase};
    }
    bool filled_model() const {
        if (pyramid_entries_.size() != handles.size()) return false;
        for (std::size_t i = 0; i < handles.size(); ++i)
            if (pyramid_entries_[i].entry_incarnation != handles[i].incarnation
                || pyramid_entries_[i].qty != quantities[i]) return false;
        return true;
    }
    OpenReportRowSource& source() { return range_end_trades_; }
    std::size_t lots() const { return pyramid_entries_.size(); }
    void remark() {
        // The same real insertion method as the run's terminal producer.
        range_end_trades_.begin(current_bar_.close, current_bar_.timestamp,
                                bar_index_, 1.0, true, pyramid_entries_.size());
    }
    std::vector<no::RequestHandle> handles;
    std::vector<double> quantities;
    std::vector<ClosedLotExcursion> expected;
    mutable std::size_t hook_calls = 0;
    double phase = 0.0;
private:
    std::size_t requested_;
    bool owns_, submitted_ = false;
};

std::size_t observe(Host& host, bool untouched_generation) {
    auto& source = host.source();
    const auto& owner = source.*access(Excursions{});
    MEM_CHECK(source.size() == host.expected.size());
    if (!MEM_CHECK(bool(owner))) return 0;
    const auto& values = owner->values;
    const auto& file = values.*access(File{});
    const auto& window = values.*access(Window{});
    const auto count = host.expected.size();
    MEM_CHECK(values.size() == count); // Exact storage count, not a work counter.
    // The window's extent is fixed by type; loaded counts are checked separately.
    MEM_CHECK(window.size() == window_rows
             && window.size() * sizeof(ClosedLotExcursion) == window_bytes);
    MEM_CHECK((values.*access(Loaded{})) <= window_rows);

    // push_back flushes the preceding page when the next page is entered.
    // With no reads since begin(), the last page remains in the write window.
    const std::size_t flushed_rows = count == 0 ? 0
        : ((count - 1) / window_rows) * window_rows;
    if (untouched_generation) {
        MEM_CHECK(file.disk_backed() == (flushed_rows != 0));
        MEM_CHECK(file.storage_bytes() == flushed_rows * sizeof(ClosedLotExcursion));
    }

    // Read EVERY stored pair, then rebuild every report row from it. The
    // handle/quantity model is independent of the sequence and its writer.
    // A wrong count is already a failure; use min only to avoid invalid reads
    // so a storage-count mismatch does not prevent later assertions.
    const auto calls_before = host.hook_calls;
    for (std::size_t i = 0; i < std::min(count, values.size()); ++i) {
        const auto actual = values[i];
        const auto expected = host.expected[i];
        MEM_CHECK(actual.favorable == expected.favorable
                 && actual.adverse == expected.adverse); // Whole sequence equality.
        if (i < source.size()) {
            const auto row = source.row(i);
            MEM_CHECK(row.open_at_end && row.qty == host.quantities[i]
                     && row.max_runup == expected.favorable
                     && row.max_drawdown == expected.adverse);
        }
        // The limit is fixed by type; Loaded is the runtime occupancy.
        MEM_CHECK((values.*access(Loaded{})) <= window_rows);
    }
    MEM_CHECK(host.hook_calls == calls_before);
    // Crossing a page on the complete forward read flushes the final tail.
    // A sequence fitting in one window has never needed a disk file at all.
    const auto persisted_rows = count > window_rows ? count : std::size_t{0};
    MEM_CHECK(file.disk_backed() == (persisted_rows != 0));
    MEM_CHECK(file.storage_bytes() == persisted_rows * sizeof(ClosedLotExcursion));
    return values.size();
}

std::size_t exercise(std::size_t population, int bars) {
    Host host(population, true);
    if (!MEM_CHECK(host.configure_native(spec(true)).status == NativeSetupStatus::Applied))
        return 0;
    const auto input = tape(bars);
    host.run(input.data(), static_cast<int>(input.size()));
    MEM_CHECK(host.last_error().empty());
    MEM_CHECK(host.native_state().kind == NativeLifecycleKind::Completed);
    MEM_CHECK(host.handles.size() == population && host.lots() == population);
    MEM_CHECK(host.filled_model());
    MEM_CHECK(host.hook_calls >= population); // Supplementary insert-path witness.
    const auto count = observe(host, false);

    // A second terminal generation must replace the previous numeric facts,
    // even when the same physical lots remain open. No strong test owner
    // keeps the old backing alive; its weak pointer must expire.
    const auto& owned = host.source().*access(Excursions{});
    std::weak_ptr previous(owned);
    host.phase = 32.0;
    for (auto& value : host.expected) {
        value.favorable += host.phase;
        value.adverse += host.phase;
    }
    const auto calls_before = host.hook_calls;
    host.remark();
    MEM_CHECK(previous.expired());
    MEM_CHECK(host.hook_calls - calls_before == population);
    MEM_CHECK(observe(host, true) == population);

    std::weak_ptr retired(host.source().*access(Excursions{}));
    host.source().clear(); // Clear the owning report source at its retirement boundary.
    MEM_CHECK(host.source().size() == 0);
    MEM_CHECK(!(host.source().*access(Excursions{}))); // Owning member released.
    MEM_CHECK(retired.expired()); // Backing destroyed, no retained generation.
    MEM_CHECK(host.lots() == population && host.filled_model());

    // A fresh empty mark also owns no excursion sequence.
    host.source().begin(100.0, first_time, 0, 1.0, true, 0);
    MEM_CHECK(host.source().empty() && !(host.source().*access(Excursions{})));
    return count;
}

void gated(std::size_t population, bool owns, bool report) {
    Host host(population, owns);
    if (!MEM_CHECK(host.configure_native(spec(report)).status == NativeSetupStatus::Applied))
        return;
    const auto input = tape(16);
    host.run(input.data(), static_cast<int>(input.size()));
    MEM_CHECK(host.last_error().empty());
    MEM_CHECK(host.native_state().kind == NativeLifecycleKind::Completed);
    MEM_CHECK(host.lots() == population && host.filled_model());
    MEM_CHECK(host.source().size() == (report ? population : 0));
    MEM_CHECK(!(host.source().*access(Excursions{})));
}
} // namespace memory_spill_range_end_excursions

int main() {
    using namespace memory_spill_range_end_excursions;
    // Small = floor(64 KiB / record bytes)/4 + 3; large = 4*small.
    // Thus large crosses the source window without assuming an ABI record
    // size. Neither number is claimed to be a source-enforced lot limit.
    // Exactly one window must stay in memory, including after a full read.
    // This boundary also distinguishes a half-window write-back page.
    const std::size_t small = window_rows / 4 + 3;
    const std::size_t populations[] = {small, 4 * small, window_rows};
    std::size_t short_counts[3]{}, long_counts[3]{};
    for (std::size_t i = 0; i < 3; ++i) {
        const auto population = populations[i];
        const auto short_name = "lots=" + std::to_string(population) + " bars=16";
        run_trial(short_name.c_str(), [&] { short_counts[i] = exercise(population, 16); });
        const auto long_name = "lots=" + std::to_string(population) + " bars=64";
        run_trial(long_name.c_str(), [&] { long_counts[i] = exercise(population, 64); });
        run_trial("fixed population as bars quadruple", [&] {
            MEM_CHECK(short_counts[i] == population && long_counts[i] == population);
        });
    }
    run_trial("storage grows with open lots", [&] {
        MEM_CHECK(short_counts[1] == 4 * short_counts[0] && short_counts[0] == small);
    });
    run_trial("empty terminal book", [] { gated(0, true, true); });
    run_trial("host does not own excursions", [] { gated(33, false, true); });
    run_trial("terminal reporting disabled", [] { gated(33, true, false); });
    std::printf("memory proof spill_range_end_excursions: %zu checks, %zu failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
