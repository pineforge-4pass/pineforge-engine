// CR150-CX hl2rev. Compile the real adapter, observing only the reversal's
// exit-cancellation window. Uses the base's APIs: its full-id call is counted
// at the call site for the intended baseline RED (lab execution still owed).
#include <pineforge/detail/disk_resident_id_table.hpp>
#include <pineforge/source/pine_strategy_host.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <limits>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <tuple>
#include <type_traits>
#include <unistd.h>
#include <utility>
#include <variant>
#include <vector>

namespace hl2rev_fixture {
using namespace pineforge;
using Adapter = source::PineExecutionAdapter;
using Snapshot = source::PlacementSnapshot;
using Id = source::SourceId;
namespace no = native_order;

void require(bool value, const char* why) {
    if (!value) throw std::runtime_error(why);
}

class DefaultBudget {
public:
    DefaultBudget() {
        if (const char* old = std::getenv(kName)) old_ = old;
        require(::unsetenv(kName) == 0, "select default cohort budget");
    }
    ~DefaultBudget() {
        if ((old_ ? ::setenv(kName, old_->c_str(), 1) : ::unsetenv(kName)) != 0)
            std::abort();
    }
    DefaultBudget(const DefaultBudget&) = delete;
    DefaultBudget& operator=(const DefaultBudget&) = delete;
private:
    static constexpr const char* kName = "PF_RESIDENT_IDTABLE_ENTRIES";
    std::optional<std::string> old_;
};

struct Io { std::uint64_t reads = 0, writes = 0; };
class ProcessIo {
public:
    ProcessIo() {
#if defined(__linux__)
        fd_ = ::open("/proc/self/io", O_RDONLY | O_CLOEXEC);
        require(fd_ >= 0, "open /proc/self/io");
        const auto before = sample(), after = sample();
        const auto control = difference(before, after);
        require(control.reads == 0 && control.writes == 0,
                "zero-control /proc/self/io calibration failed");
#else
        std::puts("SKIP hl2rev syscall counters: /proc/self/io requires Linux");
#endif
    }
    ~ProcessIo() {
#if defined(__linux__)
        if (fd_ >= 0) ::close(fd_);
#endif
    }
    ProcessIo(const ProcessIo&) = delete;
    ProcessIo& operator=(const ProcessIo&) = delete;
    static constexpr bool enabled() noexcept {
#if defined(__linux__)
        return true;
#else
        return false;
#endif
    }
    Io sample() const {
#if defined(__linux__)
        char bytes[1024];
        const auto n = ::pread(fd_, bytes, sizeof(bytes) - 1, 0);
        require(n > 0 && static_cast<std::size_t>(n) < sizeof(bytes) - 1,
                "read /proc/self/io in one syscall");
        bytes[n] = '\0';
        const char* reads = std::strstr(bytes, "syscr: ");
        const char* writes = std::strstr(bytes, "syscw: ");
        require(reads && writes, "/proc/self/io lacks syscr/syscw");
        return {std::strtoull(reads + 7, nullptr, 10),
                std::strtoull(writes + 7, nullptr, 10)};
#else
        return {};
#endif
    }
    static Io difference(Io before, Io after) {
#if defined(__linux__)
        require(after.reads > before.reads && after.writes >= before.writes,
                "process I/O counters moved backwards");
        // Linux counts the starting snapshot's pread after generating its
        // contents. There are no nested snapshots inside these windows.
        return {after.reads - before.reads - 1, after.writes - before.writes};
#else
        (void)before; (void)after;
        return {};
#endif
    }
private:
#if defined(__linux__)
    int fd_ = -1;
#endif
};

// The explicit-instantiation friend idiom used by
// test_adapter_live_state_equivalence. No access level or class layout changes.
template<class Tag, auto Member> struct PrivateAccess {
    friend auto access(Tag) { return Member; }
};
struct Cohorts { friend auto access(Cohorts); };
struct Placements { friend auto access(Placements); };
struct Live { friend auto access(Live); };
struct Brackets { friend auto access(Brackets); };
struct Coof { friend auto access(Coof); };
struct Shadows { friend auto access(Shadows); };
struct Relative { friend auto access(Relative); };
struct Anchored { friend auto access(Anchored); };
struct CoofReaches { friend auto access(CoofReaches); };
struct Cancel { friend auto access(Cancel); };
template struct PrivateAccess<Cohorts, &Adapter::cohorts_by_id_>;
template struct PrivateAccess<Placements, &Adapter::placement_>;
template struct PrivateAccess<Live, &Adapter::live_handles_>;
template struct PrivateAccess<Brackets, &Adapter::pending_bracket_legs_>;
template struct PrivateAccess<Coof, &Adapter::pending_coof_requests_>;
template struct PrivateAccess<Shadows, &Adapter::source_shadow_pending_>;
template struct PrivateAccess<Relative, &Adapter::pending_relative_exits_>;
template struct PrivateAccess<Anchored, &Adapter::anchored_relative_legs_>;
template struct PrivateAccess<CoofReaches, &Adapter::coof_leg_reaches_exit>;
template struct PrivateAccess<Cancel,
    static_cast<void (Adapter::*)(const Id&)>(&Adapter::cancel_exit_orders_for_full_close)>;

// One row per cancellation command, including failed cancellation attempts.
// Run identity is deliberately omitted, as it is in broker continuation hashes.
using Cancellation = std::tuple<std::uint64_t, std::uint64_t, int, std::string>;
struct Probe {
    ProcessIo* counter = nullptr;
    source::PineStrategyHost* host = nullptr;
    bool equivalence = false, reference = false, inside = false, measuring = false;
    int bar = -1;
    std::size_t full_walks = 0, full_walk_ids = 0, windows = 0;
    std::size_t spilled_owners = 0, relative_owners = 0, preserved_owners = 0;
    std::size_t global_owners = 0;
    std::uint64_t scope_before = 0, cursor = 0;
    Io before{};
    std::vector<Io> measured;
    std::vector<std::vector<Id>> expected, selected;
    std::vector<Cancellation> cancellations;
};
inline Probe* active = nullptr;
struct ProbeScope {
    explicit ProbeScope(Probe& probe) { require(!active, "nested fixture probe"); active = &probe; }
    ~ProbeScope() { active = nullptr; }
};

template<class Visit> auto observed_full_walk(Visit visit) {
    if (active) ++active->full_walks;
    return [visit = std::move(visit)](const auto& id) {
        if (active) ++active->full_walk_ids;
        visit(id);
    };
}
void observe(bool before, Adapter&, const std::optional<Snapshot>&, double);
void selected_owner(Adapter&, const Id&);
} // namespace hl2rev_fixture

// Headers are already loaded. This call-site substitution observes the base's
// REAL walk without replacing the table or changing the callback's result.
#define for_each_sorted_id(...) \
    for_each_sorted_id(::hl2rev_fixture::observed_full_walk(__VA_ARGS__))
#define PINEFORGE_REVERSAL_EXITS_OBSERVE(before, owner, snapshot, position) \
    ::hl2rev_fixture::observe(before, owner, snapshot, position)
#define PINEFORGE_REVERSAL_EXIT_OWNER(owner, id) \
    ::hl2rev_fixture::selected_owner(owner, id)
#define PINEFORGE_REVERSAL_EXITS_REFERENCE(owner) \
    (::hl2rev_fixture::active && ::hl2rev_fixture::active->reference)
#include "../src/source/pine_adapter.cpp"
#undef PINEFORGE_REVERSAL_EXITS_REFERENCE
#undef PINEFORGE_REVERSAL_EXIT_OWNER
#undef PINEFORGE_REVERSAL_EXITS_OBSERVE
#undef for_each_sorted_id
#include "../src/source/pine_state_hash.cpp"

namespace hl2rev_fixture {
constexpr double kNa = std::numeric_limits<double>::quiet_NaN();
constexpr std::int64_t kStart = 1736121600000LL;
constexpr int kBars = 384, kWarmup = 24;

bool exit_snapshot(const Snapshot& row) {
    return row.family == source::PineOrderFamily::ExitLimit
        || row.family == source::PineOrderFamily::ExitStop
        || row.family == source::PineOrderFamily::ExitTrail;
}
bool eligible(const Snapshot& row, const Id& id) {
    return exit_snapshot(row) && row.from_entry == id
        && !(source::detail::exit_binding_rule_switches().pending_bound_exit_survives_flat
             && row.pending_bound_exit);
}

// Deliberately slow: start with EVERY logical cohort id, then independently
// ask whether the old cancellation routine has work for that id. In
// particular, no resident-only cohort walk or production owner selector is used.
std::vector<Id> slow_ids(Adapter& adapter, const std::optional<Snapshot>& opening,
                         double position, std::vector<Id>& all_unpreserved) {
    auto& table = adapter.*access(Cohorts{});
    const auto& placements = adapter.*access(Placements{});
    const auto& live = adapter.*access(Live{});
    std::vector<Id> result;
    table.for_each_sorted_id([&](const Id& id) {
        if (opening && opening->source_id == id) return;
        for (const auto& handle : live) {
            const auto found = placements.find(handle.incarnation);
            if (found != placements.end() && found->second.opening
                && found->second.family == source::PineOrderFamily::Entry
                && found->second.is_long == (position > 0.0)
                && found->second.source_id == id) {
                return;
            }
        }
        all_unpreserved.push_back(id);
        bool work = false;
        for (const auto& handle : live) {
            const auto found = placements.find(handle.incarnation);
            if (found != placements.end() && eligible(found->second, id)
                && !(adapter.*access(CoofReaches{}))(found->second)) work = true;
        }
        for (const auto& row : adapter.*access(Brackets{}))
            if (eligible(row.snapshot, id)) work = true;
        for (const auto& row : adapter.*access(Coof{}))
            if (eligible(row.snapshot, id)) work = true;
        for (const auto& row : adapter.*access(Shadows{}))
            if (eligible(row.snapshot, id)) work = true;
        for (const auto& row : adapter.*access(Relative{}))
            if (row.from_entry == id) work = true;
        for (const auto& row : adapter.*access(Anchored{}))
            if (!id.empty() && row.from_entry == id) work = true;
        if (work) result.push_back(id);
    });
    return result;
}

void capture_cancellations(Probe& probe, bool save) {
    for (const auto& row : probe.host->native_events(probe.cursor)) {
        probe.cursor = std::max(probe.cursor, row.ordinal);
        if (!save || !row.command) continue;
        std::visit([&](const auto& event) {
            using Event = std::decay_t<decltype(event)>;
            if constexpr (std::is_same_v<Event, no::CancelledEvent>) {
                probe.cancellations.emplace_back(event.ordinal, event.handle().incarnation,
                    static_cast<int>(event.reason), event.request().label);
            } else if constexpr (std::is_same_v<Event, no::NotWorkingEvent>) {
                probe.cancellations.emplace_back(event.ordinal, event.target.incarnation,
                    -1, event.attempted ? event.attempted->label : std::string{});
            }
        }, *row.command);
    }
}

void observe(bool before, Adapter& adapter, const std::optional<Snapshot>& opening,
             double position) {
    if (!active) return;
    auto& probe = *active;
    if (before) {
        require(!probe.inside, "reversal window reentered");
        probe.inside = true;
        if (probe.equivalence) {
            capture_cancellations(probe, false);
            auto& table = adapter.*access(Cohorts{});
            // Real public strategy_exit calls created these empty cohorts and
            // queued orders. Explicitly spill their real production rows,
            // before either algorithm, with the DEFAULT resident budget.
            for (const char* id : {"z-empty", "a-empty", "m-empty"}) {
                {
                    const auto found = table.find(id);
                    require(found != table.end() && found->second.spillable(),
                            "orphan exit cohort is not genuinely empty");
                }
            }
            table.spill();
            for (const char* id : {"z-empty", "a-empty", "m-empty"}) {
                require(table.find_resident(id) == table.end(),
                        "orphan cohort did not really spill");
            }
            std::vector<Id> all;
            probe.expected.push_back(slow_ids(adapter, opening, position, all));
            probe.selected.emplace_back();
            for (const auto& id : probe.expected.back()) {
                if (id == "z-empty" || id == "a-empty" || id == "m-empty")
                    ++probe.spilled_owners;
                if (id == "relative-parent") ++probe.relative_owners;
            }
            // Check both preservation and the old cohort-domain exclusion
            // are populated, whether a leg is live or still queued.
            const auto inspect = [&](const Snapshot& row) {
                if (!exit_snapshot(row)) return;
                if (row.from_entry == "keep-next") ++probe.preserved_owners;
                if (row.from_entry.empty()) ++probe.global_owners;
            };
            for (const auto& handle : adapter.*access(Live{})) {
                const auto& rows = adapter.*access(Placements{});
                const auto row = rows.find(handle.incarnation);
                if (row != rows.end()) inspect(row->second);
            }
            for (const auto& row : adapter.*access(Brackets{})) inspect(row.snapshot);
            for (const auto& row : adapter.*access(Coof{})) inspect(row.snapshot);
            for (const auto& row : adapter.*access(Shadows{})) inspect(row.snapshot);
            require(table.find("") == table.end(), "global exit unexpectedly has a cohort");
            if (probe.reference) {
                probe.selected.back() = probe.expected.back();
                // Execute the old full walk, including its idle calls, using
                // the real cancellation routine and native host. The fixture
                // skips the new traversal: a terminal handle can remain in
                // live_handles_, and cancelling it twice records NotWorking.
                for (const auto& id : all) (adapter.*access(Cancel{}))(id);
            }
        }
        probe.scope_before = source::detail::exit_scratch(adapter).next_scope;
        probe.measuring = !probe.equivalence && probe.bar >= kWarmup;
        if (probe.measuring) probe.before = probe.counter->sample();
    } else {
        const auto after = probe.measuring ? probe.counter->sample() : Io{};
        require(probe.inside, "unpaired reversal window");
        if (!probe.equivalence) {
            require(source::detail::exit_scratch(adapter).next_scope == probe.scope_before,
                    "empty exit-owner selection constructed an ExitScratchScope");
            if (probe.measuring)
                probe.measured.push_back(ProcessIo::difference(probe.before, after));
        } else {
            require(probe.selected.back() == probe.expected.back(),
                    "restricted owner set/order differs from independent full walk");
            capture_cancellations(probe, true);
        }
        ++probe.windows;
        probe.inside = false;
    }
}
void selected_owner(Adapter&, const Id& id) {
    if (active && active->inside && active->equivalence)
        active->selected.back().push_back(id);
}

std::vector<Bar> tape(int count) {
    std::vector<Bar> bars;
    for (int i = 0; i < count; ++i)
        bars.push_back({100.0, 101.0, 99.0, 100.0, 1.0, kStart + i * 60000LL});
    return bars;
}
class Host : public source::PineStrategyHost {
public:
    explicit Host(Probe& probe) : probe_(probe) {
        source::PineStrategyConfig config;
        config.initial_capital = 100000.0;
        config.default_qty_type = static_cast<int>(QtyType::FIXED);
        config.default_qty_value = 1.0;
        config.pyramiding = 4;
        configure_pine_strategy(config);
        set_syminfo_timezone("UTC");
        set_syminfo_session("24x7");
        set_syminfo_mintick(0.25);
        set_broker_state_hash_recording(true);
        probe_.host = this;
    }
    void seen_position() {
        const double units = live_position_size();
        if (units != 0.0) {
            const int sign = units > 0.0 ? 1 : -1;
            if (last_sign_ != 0 && sign != last_sign_) ++reversals;
            last_sign_ = sign;
        }
        logical_cohorts = std::max(logical_cohorts,
            static_cast<std::size_t>((adapter_.*access(Cohorts{})).size()));
    }
    std::size_t reversals = 0, logical_cohorts = 0;
    std::vector<std::uint64_t> reads;
protected:
    Probe& probe_;
private:
    int last_sign_ = 0;
};
class NoExitHost final : public Host {
public:
    explicit NoExitHost(Probe& probe) : Host(probe) {}
    void on_source_bar(const Bar&) override {
        const int bar = pine_bar_index();
        probe_.bar = bar;
        seen_position();
        if (bar % 3 == 0) {
            const int turn = bar / 3;
            const bool is_long = turn % 2 == 0;
            const auto id = std::string(is_long ? "L/" : "S/") + std::to_string(turn % 24);
            ids.insert(id);
            strategy_entry(id, is_long);
        }
    }
    std::set<std::string> ids;
};

void no_exit_reversal_io(ProcessIo& counter) {
    DefaultBudget budget;
    Probe probe;
    probe.counter = &counter;
    NoExitHost host(probe);
    const auto bars = tape(kBars);
    {
        ProbeScope scope(probe);
        host.run(bars.data(), static_cast<int>(bars.size()));
    }
    require(host.last_error().empty(), host.last_error().c_str());
    require(host.reversals >= 100 && host.trade_count() >= 100,
            "fixture failed to execute real position reversals");
    require(host.ids.size() == 24 && host.logical_cohorts >= 24,
            "fixture failed to populate multiple distinct logical cohort ids");
    // First baseline failure: the original on_applied call is observed even
    // though the new narrow-window hook does not exist in that tree.
    require(probe.full_walks == 0 && probe.full_walk_ids == 0,
            "on_applied still walks historical cohort ids on reversals");
    require(probe.windows == host.reversals && probe.measured.size() >= 100
            && !probe.inside, "reversal counter windows were not actually reached");
    Io total;
    for (const auto delta : probe.measured) {
        require(delta.reads == 0 && delta.writes == 0,
                "no-exit reversal selection issued scratch I/O");
        total.reads += delta.reads;
        total.writes += delta.writes;
    }
    std::printf("[hl2rev-no-exit] bars=%d ids=%zu reversals=%zu windows=%zu measured=%zu "
                "walks=%zu syscr=%llu syscw=%llu\n",
                kBars, host.ids.size(), host.reversals, probe.windows, probe.measured.size(),
                probe.full_walks, static_cast<unsigned long long>(total.reads),
                static_cast<unsigned long long>(total.writes));
}

// A table-level positive control for the historical walk, independently of
// adapter hooks. All rows and backing reads are real, including on the base.
struct Facts {
    struct Record { std::uint64_t handle, cycle, count, digest, sides; };
    Record value{};
    Record retired_record() const noexcept { return value; }
    static Facts restore_record(const Record& value) { return {value}; }
    bool spillable() const noexcept { return true; }
};
void historical_walk_control(ProcessIo& counter) {
    std::string directory = "/tmp/pineforge-hl2rev-XXXXXX";
    require(::mkdtemp(directory.data()) != nullptr, "create table control directory");
    {
        pineforge::detail::DiskResidentIdTable<Facts> table;
        table.set_directory(directory);
        std::vector<Id> expected, actual;
        for (std::uint64_t i = 0; i < 32; ++i) {
            const auto id = "cohort/" + std::to_string(i);
            table.emplace(id, Facts{{i + 1, i, 0, 0, 0}});
            expected.push_back(id);
        }
        std::sort(expected.begin(), expected.end());
        actual.reserve(expected.size());
        const auto before = counter.sample();
        table.for_each_sorted_id([&](const Id& id) { actual.push_back(id); });
        const auto after = counter.sample();
        const auto delta = ProcessIo::difference(before, after);
        require(actual == expected, "full-key walk did not visit the real table");
        if (ProcessIo::enabled())
            require(delta.reads > 0, "historical disk-key walk positive control did no I/O");
        std::printf("[hl2rev-full-walk-control] ids=%zu syscr=%llu syscw=%llu\n",
                    actual.size(), static_cast<unsigned long long>(delta.reads),
                    static_cast<unsigned long long>(delta.writes));
    }
    require(::rmdir(directory.c_str()) == 0, "remove table control directory");
}

class ExitHost final : public Host {
public:
    ExitHost(Probe& probe, bool relative) : Host(probe), relative_(relative) {}
    void on_source_bar(const Bar&) override {
        const int bar = pine_bar_index();
        probe_.bar = bar;
        seen_position();
        if (bar == 0) {
            // Creation order differs from lexical cancellation order. None
            // of these cohorts ever opens, but its real exit stays queued.
            strategy_exit("xz", "z-empty", 200.0, 0.5);
            strategy_exit("xa", "a-empty", 200.0, 0.5);
            strategy_exit("xm", "m-empty", 200.0, 0.5);
            strategy_entry("held", true);
            if (relative_) {
                strategy_entry("relative-parent", true, 50.0);
                strategy_exit("relative-x", "relative-parent", kNa, kNa,
                              kNa, kNa, kNa, 100.0, {}, kNa, {}, 40.0);
            }
        }
        if (bar == 1) {
            strategy_exit("held-x", "held", 200.0, 0.5, kNa, kNa, kNa, 50.0);
            strategy_entry("keep-next", false, 200.0);
            strategy_exit("keep-x", "keep-next", 0.5, 200.0);
            strategy_entry("reversal", false);
            strategy_exit("global-x", "", 200.0, 0.5, kNa, kNa, kNa, 25.0);
        }
        reads.push_back(broker_state_hash());
    }
private:
    bool relative_;
};

bool same_bits(double a, double b) { return std::memcmp(&a, &b, sizeof(a)) == 0; }
void same_trade(const Trade& a, const Trade& b) {
    require(a.entry_time == b.entry_time && a.exit_time == b.exit_time
            && a.entry_bar_index == b.entry_bar_index && a.exit_bar_index == b.exit_bar_index
            && a.entry_id == b.entry_id && a.exit_id == b.exit_id
            && a.entry_comment == b.entry_comment && a.exit_comment == b.exit_comment
            && a.entry_incarnation == b.entry_incarnation && a.is_long == b.is_long
            && a.open_at_end == b.open_at_end && a.exit_from_bracket == b.exit_from_bracket
            && a.close_cause == b.close_cause, "full-walk/reference trade identity");
    require(same_bits(a.entry_price, b.entry_price) && same_bits(a.exit_price, b.exit_price)
            && same_bits(a.qty, b.qty) && same_bits(a.pnl, b.pnl)
            && same_bits(a.pnl_pct, b.pnl_pct) && same_bits(a.commission, b.commission)
            && same_bits(a.max_runup, b.max_runup) && same_bits(a.max_drawdown, b.max_drawdown),
            "full-walk/reference trade numeric bits");
}
void exit_equivalence(bool relative) {
    DefaultBudget budget;
    Probe actual, slow;
    actual.equivalence = slow.equivalence = true;
    slow.reference = true;
    ExitHost actual_host(actual, relative), slow_host(slow, relative);
    const auto bars = tape(5);
    { ProbeScope scope(actual); actual_host.run(bars.data(), static_cast<int>(bars.size())); }
    { ProbeScope scope(slow); slow_host.run(bars.data(), static_cast<int>(bars.size())); }
    require(actual_host.last_error().empty(), actual_host.last_error().c_str());
    require(slow_host.last_error().empty(), slow_host.last_error().c_str());
    require(actual_host.reversals == 1 && slow_host.reversals == 1
            && actual.windows == 1 && slow.windows == 1,
            "exit equivalence did not reach the actual reversal branch");
    require(actual.spilled_owners == 3 && slow.spilled_owners == 3,
            "equivalence missed an exit of an empty spilled cohort");
    require(actual.preserved_owners > 0 && slow.preserved_owners > 0,
            "equivalence did not populate the preserved pending-parent case");
    require(actual.global_owners > 0 && slow.global_owners > 0,
            "equivalence did not populate the absent-cohort global-exit case");
    require(!relative || (actual.relative_owners > 0 && slow.relative_owners > 0),
            "relative-only pending exit did not reach the reversal");
    require(actual.expected == slow.expected && actual.selected == slow.selected,
            "cancelled source-id set or sorted order differs from the old full walk");
    require(!actual.selected.empty() && actual.selected.front().size() >= 4,
            "equivalence cancelled too few independent source owners");
    require(!actual.cancellations.empty() && actual.cancellations == slow.cancellations,
            "native cancellation attempts, ordinals or order differ from the full walk");
    require(actual_host.reads == slow_host.reads
            && actual_host.broker_state_hash() == slow_host.broker_state_hash(),
            "full-walk/reference broker hashes or receipt/journal state differ");
    require(actual_host.trade_count() > 0
            && actual_host.trade_count() == slow_host.trade_count(),
            "full-walk/reference trade populations differ");
    for (int i = 0; i < actual_host.trade_count(); ++i)
        same_trade(actual_host.get_trade(i), slow_host.get_trade(i));
    std::printf("[hl2rev-equivalence] relative=%d owners=%zu spilled=%zu cancellations=%zu\n",
                relative, actual.selected.front().size(), actual.spilled_owners,
                actual.cancellations.size());
}
} // namespace hl2rev_fixture

int main() {
    try {
        hl2rev_fixture::ProcessIo counter;
        hl2rev_fixture::historical_walk_control(counter);
        hl2rev_fixture::no_exit_reversal_io(counter);
        hl2rev_fixture::exit_equivalence(false);
        hl2rev_fixture::exit_equivalence(true);
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "hl2rev reversal I/O: %s\n", error.what());
        return 1;
    }
}
