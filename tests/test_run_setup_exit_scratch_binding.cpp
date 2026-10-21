// LAB-O1SEAMS s1c: a real Pine run must provision its exit readers at begin.
// On d755bd69, prepare_script_run reaches exit_scratch_lots before any bar;
// it throws ExitScratchError for the unset reader. The engine records that
// coded exception; require_healthy rethrows its actual code and text. After
// binding, a real relative global exit also exercises the nonempty book.
// This TU uses only APIs already present on that base.
// This edit-only lane has not compiled or executed the test.
#include <pineforge/source/pine_strategy_host.hpp>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <dirent.h>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <unistd.h>
#include <vector>

namespace {
using namespace pineforge;
using Status = detail::ScratchStatus;
constexpr double kNa = std::numeric_limits<double>::quiet_NaN();

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

void require_healthy(const source::PineStrategyHost& host) {
    if (host.last_run_status() != 0 || !host.last_error().empty())
        throw coded<std::logic_error>(run_failure_value_of(host), host.last_error());
}

void require_completed(const source::PineStrategyHost& host) {
    require_healthy(host);
    require(host.native_state().kind == NativeLifecycleKind::Completed,
            "the real batch must complete");
}

std::vector<int> open_descriptors() {
    std::unique_ptr<DIR, int (*)(DIR*)> directory(::opendir("/dev/fd"), ::closedir);
    require(directory != nullptr, "open descriptor directory");
    const int scanner = ::dirfd(directory.get());
    std::vector<int> result;
    for (;;) {
        errno = 0;
        const auto* entry = ::readdir(directory.get());
        if (!entry) {
            require(errno == 0, "read descriptor directory");
            break;
        }
        char* end = nullptr;
        const long descriptor = std::strtol(entry->d_name, &end, 10);
        if (end != entry->d_name && *end == '\0' && descriptor >= 0
            && descriptor != scanner)
            result.push_back(static_cast<int>(descriptor));
    }
    std::sort(result.begin(), result.end());
    return result;
}

class ExitHost final : public source::PineStrategyHost {
public:
    ExitHost() {
        source::PineStrategyConfig config;
        config.initial_capital = 100000.0;
        config.default_qty_type = static_cast<int>(QtyType::FIXED);
        config.default_qty_value = 1.0;
        config.margin_long = config.margin_short = 0.0;
        config.process_orders_on_close = false;
        config.calc_on_order_fills = false;
        config.close_entries_rule_any = false;
        configure_pine_strategy(config);
        set_syminfo_timezone("UTC");
        set_syminfo_session("24x7");
        set_syminfo_mintick(0.25);
    }

    void on_source_bar(const Bar&) override {
        ++callbacks;
        if (bar_index_ == 0)
            strategy_entry("entry", true, kNa, kNa, 1.0);
        if (bar_index_ == 1) {
            // One filled entry is enough to call the real lot reader before
            // the adapter chooses between whole-position and per-entry exits.
            // The distant target leaves a nonempty working book to inspect.
            strategy_exit("exit", "", kNa, kNa, kNa, kNa, kNa,
                          100.0, {}, kNa, {}, 10000.0, kNa);
            ++exits_returned;
        }
    }

    source::detail::ExitScratchContext& scratch() {
        return source::detail::exit_scratch(adapter_);
    }

    std::size_t callbacks = 0;
    std::size_t exits_returned = 0;
    std::size_t preparations = 0;

protected:
    void prepare_script_run(const Bar*, int, bool) override {
        require(callbacks == 0, "run setup precedes every source bar");
        auto& context = scratch();
        const auto state = native_state();
        require(state.spec != nullptr && context.run == state.spec->identity,
                "setup binds the projected run identity");
        std::size_t visited = 0;
        // These are the real adapter readers, called during the real begin
        // hook. Without host binding, the first call throws ExitScratchError.
        source::detail::exit_scratch_lots(context, *this, kNa, [&](const NativeOpenLot&) {
            ++visited;
            return true;
        });
        source::detail::exit_scratch_working(context, *this, [&](const NativeWorkingRequest&) {
            ++visited;
            return true;
        });
        require(visited == native_open_lots(kNa).size() + native_working_requests().size(),
                "setup readers enumerate the actual book before any input");
        ++preparations;
    }
};

void check_bound_readers(ExitHost& host) {
    auto& scratch = host.scratch();
    require(scratch.store.status() == Status::ok, "run scratch is usable");
    require(!scratch.store.transaction_active(), "exit scopes close their transactions");
    require(scratch.store.record_count() == 0, "exit scopes release their rows");
    require(scratch.next_scope != 0, "the production exit actually used scratch");

    const double mark = host.position_avg_price();
    const auto lots = host.native_open_lots(mark);
    require(!lots.empty(), "the lot reader must exercise a real position");
    std::size_t lot_index = 0;
    source::detail::exit_scratch_lots(scratch, host, mark, [&](const NativeOpenLot& lot) {
        require(lot_index < lots.size(), "lot reader adds no rows");
        const auto& expected = lots[lot_index++];
        require(lot.entry_incarnation == expected.entry_incarnation
                && lot.cycle == expected.cycle && lot.side == expected.side
                && lot.entry_label == expected.entry_label
                && lot.entry_comment == expected.entry_comment
                && lot.entry_price == expected.entry_price
                && lot.signed_units == expected.signed_units
                && lot.entry_commission == expected.entry_commission
                && lot.unrealized_pnl == expected.unrealized_pnl,
                "bound lot reader preserves the native book");
        return true;
    });
    require(lot_index == lots.size(), "lot reader visits every native lot");

    const auto working = host.native_working_requests();
    require(!working.empty(), "the working reader must exercise a real exit");
    std::size_t working_index = 0;
    source::detail::exit_scratch_working(scratch, host, [&](const NativeWorkingRequest& row) {
        require(working_index < working.size(), "working reader adds no rows");
        const auto& expected = working[working_index++];
        require(row.definition == expected.definition,
                "bound working reader preserves native request definitions and order");
        return true;
    });
    require(working_index == working.size(), "working reader visits every native request");
    std::size_t stopped = 0;
    source::detail::exit_scratch_working(scratch, host, [&](const NativeWorkingRequest&) {
        ++stopped;
        return false;
    });
    require(stopped == std::min<std::size_t>(1, working.size()),
            "reader honors early termination");
}

void run_setup_binds_exit_scratch_before_global_exit() {
    const std::array<Bar, 3> bars{{
        {100.0, 101.0, 99.0, 100.0, 1.0, 1704067200000LL},
        {100.0, 101.0, 99.0, 100.0, 1.0, 1704067260000LL},
        {100.0, 101.0, 99.0, 100.0, 1.0, 1704067320000LL},
    }};
    // Warm process-wide timezone/calendar setup before measuring descriptors.
    { ExitHost warm; warm.run(bars.data(), 1); require_completed(warm); }
    const auto before = open_descriptors();
    {
        ExitHost host;
        host.run(bars.data(), static_cast<int>(bars.size()));
        require_completed(host);
        require(host.preparations != 0, "the begin hook checked its readers");
        require(host.callbacks == bars.size(), "all source bars execute");
        require(host.exits_returned != 0, "the relative exit returns successfully");
        check_bound_readers(host);
        const auto first_run = host.scratch().run;
        const auto first_preparations = host.preparations;
        const auto after_first = open_descriptors();
        require(after_first.size() > before.size(), "the run owns live scratch descriptors");
        // A second run releases the old owner after every borrower is rebound.
        host.callbacks = host.exits_returned = 0;
        host.run(bars.data(), static_cast<int>(bars.size()));
        require_completed(host);
        require(host.scratch().run != first_run, "restart binds the new projected run");
        require(host.callbacks == bars.size() && host.exits_returned != 0,
                "restart executes the same real strategy");
        require(host.preparations > first_preparations, "restart checks readers before input");
        check_bound_readers(host);
        require(open_descriptors().size() == after_first.size(),
                "restart releases the preceding run's backing files");
    }
    require(open_descriptors() == before, "host teardown releases all scratch backing files");
}

void stream_setup_binds_readers_before_input() {
    ExitHost host;
    const Bar warmup{100.0, 101.0, 99.0, 100.0, 1.0, 1704067200000LL};
    const bool begun = host.stream_begin(&warmup, 1, "1", "1");
    require_healthy(host);
    require(begun, "stream begins with a valid warmup bar");
    require(host.preparations != 0, "stream setup checked its readers before warmup");
    auto& scratch = host.scratch();
    const auto state = host.native_state();
    require(state.spec != nullptr && scratch.run == state.spec->identity,
            "stream retains the same projected run binding");
    require(host.stream_end(), "stream teardown succeeds with its binding alive");
    require(host.last_error().empty(), "stream teardown records no scratch failure");
}
} // namespace

int main() {
    try {
        stream_setup_binds_readers_before_input();
        run_setup_binds_exit_scratch_before_global_exit();
    } catch (const std::exception& error) {
        std::fprintf(stderr, "test_run_setup_exit_scratch_binding: FAIL: %s\n", error.what());
        return 1;
    }
    std::puts("test_run_setup_exit_scratch_binding: OK");
    return 0;
}
