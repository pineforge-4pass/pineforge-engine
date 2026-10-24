// Frozen2 RED: compile this unit with its original header closure. The
// feature guard selects configuration and adds new-API geometry checks.
// Trading, digest, logical-I/O and physical acceptance assertions always run.
// No linker-wrap options: these executable symbols forward the real Linux
// positional syscalls, including their exact return values and errno.
#if !defined(__linux__)
#error "The physical-I/O witness requires the Linux lab"
#endif

#include "../src/command_scratch_production.hpp"
#include <pineforge/bar.hpp>
#include <pineforge/source/exit_scratch.hpp>
#include <pineforge/source/pine_strategy_host.hpp>

#include <array>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <type_traits>
#include <unistd.h>
#include <vector>

namespace witness {
using namespace pineforge;
namespace pd = pineforge::detail;
namespace sd = pineforge::source::detail;
using Count = std::uint64_t;

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

struct Physical {
    Count reads = 0, writes = 0, read_bytes = 0, write_bytes = 0, errors = 0;
    bool classification_failed = false;
};
struct Window {
    bool active = false;
    Physical physical;
    std::array<Count, 3> logical_calls{}, logical_bytes{};
    bool logical_failed = false;
} window;

void positional(int fd, std::size_t bytes, ssize_t result, bool write) noexcept {
    if (!window.active) return;
    const int saved = errno;
    char path[64]{}, target[4096]{};
    const int length = std::snprintf(path, sizeof(path), "/proc/self/fd/%d", fd);
    struct stat info{};
    const auto count = length > 0 && static_cast<std::size_t>(length) < sizeof(path)
        ? ::readlink(path, target, sizeof(target) - 1) : -1;
    if (count < 0 || static_cast<std::size_t>(count) == sizeof(target) - 1
            || ::fstat(fd, &info) != 0) {
        window.physical.classification_failed = true;
    } else {
        target[static_cast<std::size_t>(count)] = '\0';
        const char* slash = std::strrchr(target, '/');
        const char* name = slash ? slash + 1 : target;
        constexpr char prefix[] = "pineforge-command-";
        if (std::strncmp(name, prefix, sizeof(prefix) - 1) == 0) {
            window.physical.classification_failed |= !S_ISREG(info.st_mode) || info.st_nlink != 0;
            if (write) {
                ++window.physical.writes;
                window.physical.write_bytes += bytes;
            } else {
                ++window.physical.reads;
                window.physical.read_bytes += bytes;
            }
            window.physical.errors += result < 0 || static_cast<std::size_t>(result) != bytes;
        }
    }
    errno = saved;
}

struct Observer {
    pd::ScratchIoObserver previous;
    Observer() : previous(pd::set_command_scratch_io_observer({nullptr, &receive})) {}
    ~Observer() { pd::set_command_scratch_io_observer(previous); }
    static void receive(void*, const pd::ScratchIoCharge& charge) noexcept {
        if (!window.active) return;
        const auto op = static_cast<std::size_t>(charge.operation);
        if (op >= window.logical_calls.size()) {
            window.logical_failed = true;
            return;
        }
        ++window.logical_calls[op];
        window.logical_bytes[op] += charge.bytes;
        window.logical_failed |= charge.kind != pd::ScratchBackingKind::disk
            || charge.status != pd::ScratchStatus::ok;
    }
};

// Match CR-257's typed, observation-only member access. Never seed or mutate
// an adapter/store, and never infer layout with a cast.
template<class Tag, auto Member> struct MemberAccess {
    friend auto member(Tag) { return Member; }
};
struct AdapterMember { friend auto member(AdapterMember); };
template struct MemberAccess<AdapterMember, &source::PineStrategyHost::adapter_>;
struct StoreMember { friend auto member(StoreMember); };
template struct MemberAccess<StoreMember, &pd::CommandScratchStore::state_>;
source::PineExecutionAdapter& adapter(source::PineStrategyHost& host) {
    return host.*member(AdapterMember{});
}
pd::scratch_impl::Core& core(pd::CommandScratchStore& store) {
    auto* state = store.*member(StoreMember{});
    require(state != nullptr, "exit store has no workspace");
    return *state;
}

enum class Mode { resident, write_through, tiny };
struct Selection {
#if defined(PINEFORGE_COMMAND_SCRATCH_WRITE_BEHIND_V1)
    std::optional<std::size_t> old_placements, old_journal;
    bool old_write_through;
    explicit Selection(Mode mode)
        : old_placements(pd::set_command_scratch_resident_entries(
              pd::CommandScratchResidentKind::placement_snapshots,
              mode == Mode::tiny ? std::optional<std::size_t>{2} : std::nullopt)),
          old_journal(pd::set_command_scratch_resident_entries(
              pd::CommandScratchResidentKind::journal_pages,
              mode == Mode::tiny ? std::optional<std::size_t>{2} : std::nullopt)),
          old_write_through(pd::set_command_scratch_write_through(mode == Mode::write_through)) {}
    ~Selection() {
        pd::set_command_scratch_write_through(old_write_through);
        pd::set_command_scratch_resident_entries(
            pd::CommandScratchResidentKind::journal_pages, old_journal);
        pd::set_command_scratch_resident_entries(
            pd::CommandScratchResidentKind::placement_snapshots, old_placements);
    }
#else
    // Frozen2 has only the unmodified write-through path. Run it, and fail
    // the physical-zero assertion below; do not replace this with a skip.
    explicit Selection(Mode) {}
#endif
};

constexpr Count warmup = 8;
constexpr std::int64_t start = INT64_C(1736121600000), minute = 60000;
constexpr double na = std::numeric_limits<double>::quiet_NaN();
constexpr const char* entry_id = "perffix-entry";
constexpr const char* exit_id = "perffix-held";

struct Driver {
    std::vector<Bar> bars;
    explicit Driver(Count n) {
        for (Count i = 0; i <= warmup + n; ++i) {
            Bar bar{};
            bar.timestamp = start + static_cast<std::int64_t>(i) * minute;
            bar.open = 100.0;
            bar.close = 100.0 + (i % 2 ? 0.25 : 0.0);
            bar.high = 100.5;
            bar.low = 99.5;
            bar.volume = static_cast<double>(i + 1);
            if (i == warmup + n) {
                bar.high = 161.0;
                bar.close = 160.0; // Finally execute the held input-derived limit.
            }
            bars.push_back(bar);
        }
    }
    double level(Count i) const { return bars.at(static_cast<std::size_t>(i)).close + 50.0; }
};

class Host final : public source::PineStrategyHost {
public:
    const Driver& driver;
    Count callbacks = 0, reissues = 0, opens = 0, closes = 0;
    explicit Host(const Driver& input) : driver(input) {
        source::PineStrategyConfig config;
        config.initial_capital = 1000000.0;
        config.default_qty_type = static_cast<int>(QtyType::FIXED);
        config.default_qty_value = 1.0;
        config.process_orders_on_close = true;
        configure_pine_strategy(config);
        set_trade_history_streamable(false);
        set_syminfo_timezone("UTC");
        set_syminfo_session("24x7");
        syminfo_mintick_ = 0.25;
    }
    void prepare_script_run(const Bar*, int, bool) override {
        trades_.set_scratch_directory(".");
    }
    void on_source_bar(const Bar& bar) override {
        const Count i = callbacks++;
        require(i < driver.bars.size() && pine_bar_index() == static_cast<int>(i)
                    && bar.timestamp == driver.bars[static_cast<std::size_t>(i)].timestamp
                    && bar.volume == static_cast<double>(i + 1),
                "host skipped, duplicated or ignored an input bar");
        if (i == 0) strategy_entry(entry_id, true, na, na, 1.0, "entry");
        if (i + 1 < driver.bars.size()) {
            strategy_exit(exit_id, entry_id, driver.level(i), na,
                          na, na, na, 100.0, "held POOC limit");
            ++reissues;
        }
    }
    void on_native_applied_event(const native_order::ExecutionAppliedEvent& event,
                                 const NativeDecisionContext&) override {
        opens += event.opened_units != 0.0;
        closes += event.closed_trade_count;
    }
};

struct Canonical {
    std::vector<unsigned char> bytes;
    template<class T> void add(const T& value) {
        static_assert(std::is_arithmetic_v<T> || std::is_enum_v<T>);
        const auto* first = reinterpret_cast<const unsigned char*>(&value);
        bytes.insert(bytes.end(), first, first + sizeof(value));
    }
    void add(const std::string& text) {
        add(static_cast<Count>(text.size()));
        bytes.insert(bytes.end(), text.begin(), text.end());
    }
    void trade(const Trade& t) {
        add(t.entry_time); add(t.exit_time); add(t.entry_price); add(t.exit_price);
        add(t.qty); add(t.pnl); add(t.pnl_pct); add(t.is_long);
        add(t.entry_bar_index); add(t.exit_bar_index); add(t.entry_id); add(t.entry_comment);
        add(t.exit_comment); add(t.exit_id); add(t.exit_from_bracket);
        add(t.max_runup); add(t.max_drawdown); add(t.commission);
        add(t.entry_incarnation); add(t.open_at_end); add(t.close_cause);
    }
};
struct Result {
    Physical physical;
    std::array<Count, 3> logical_calls{}, logical_bytes{};
    Canonical transcript;
};

Result run(Count n, Mode mode) {
    const Selection selection(mode); // BEFORE hosts/backings; restored AFTER destruction.
    const Observer observer;
    const Driver driver(n);
    Host host(driver);
    require(host.stream_begin(driver.bars.data(), static_cast<int>(warmup), "1", "1"),
            "POOC warmup failed");
    require(adapter(host).process_orders_on_close(), "the test must reach the POOC pass");
    auto& context = sd::exit_scratch(adapter(host));
    auto& store = context.store;
    auto& actual = core(store);
    const auto extent = store.stats().backing_pages;
    require(store.record_count() == 0 && !store.transaction_active()
                && host.physical_position().signed_units == 1.0
                && host.pending_order_count() == 1 && context.versions,
            "warmup did not leave one real held exit and a settled version store");
#if defined(PINEFORGE_COMMAND_SCRATCH_WRITE_BEHIND_V1)
    if (mode == Mode::resident)
        require(extent <= pd::command_scratch_default_resident_entries(store.page_size()),
                "derived live pages must fit the fixed resident budget");
    else if (mode == Mode::tiny)
        require(extent > 2, "tiny run must actually collide and spill");
#endif
    const auto before = store.stats();
    const Count handles_before = actual.cur_.next_handle, sequence_before = actual.cur_.seq;
    std::vector<std::byte> page(store.page_size());
    Result result;
    window = {};
    window.active = true;
    struct StopWindow { ~StopWindow() { window.active = false; } } stop;
    for (Count i = warmup; i < warmup + n; ++i) {
        const Count handles = actual.cur_.next_handle, sequence = actual.cur_.seq;
        require(host.stream_push_bar(driver.bars[static_cast<std::size_t>(i)]),
                "held-exit input bar failed");
        require(host.closed_trade_count() == 0 && host.pending_order_count() == 1
                    && host.physical_position().signed_units == 1.0
                    && actual.cur_.next_handle >= handles + 4
                    && actual.cur_.seq >= sequence + 7 && store.record_count() == 0,
                "bar missed the real snapshot allocation/commit/erase path");
        Count digest = 0;
        require(store.state_digest({page.data(), page.size()}, digest) == pd::ScratchStatus::ok,
                "real store state_digest failed");
        result.transcript.add(digest);
        result.transcript.add(host.broker_state_hash());
        result.transcript.add(actual.cur_.next_handle);
        result.transcript.add(actual.cur_.seq);
        result.transcript.add(context.next_scope);
        const auto stats = store.stats();
        result.transcript.add(stats.page_reads - before.page_reads);
        result.transcript.add(stats.page_writes - before.page_writes);
        result.transcript.add(stats.syncs - before.syncs);
        for (const auto count : window.logical_calls) result.transcript.add(count);
        for (const auto bytes : window.logical_bytes) result.transcript.add(bytes);
    }
    window.active = false; // End/report/destruction are outside the per-bar counters.
    result.physical = window.physical;
    result.logical_calls = window.logical_calls;
    result.logical_bytes = window.logical_bytes;
    const auto after = store.stats();
    require(!window.logical_failed && !result.physical.classification_failed
                && result.physical.errors == 0 && result.logical_calls[1] > 0
                && actual.cur_.next_handle >= handles_before + 4 * n
                && actual.cur_.seq >= sequence_before + 7 * n
                && after.page_writes > before.page_writes,
            "counter/snapshot-path premise failed or the workload was vacuous");
#if defined(PINEFORGE_COMMAND_SCRATCH_WRITE_BEHIND_V1)
    if (mode == Mode::resident)
        require(after.backing_pages <= pd::command_scratch_default_resident_entries(store.page_size()),
                "all admitted pages, including handle-height growth, must stay resident");
#endif

    require(host.stream_push_bar(driver.bars.back()) && host.stream_end(),
            "final real bracket fill/end failed");
    require(host.callbacks == driver.bars.size() && host.reissues == warmup + n
                && host.opens == 1 && host.closes == 1 && host.closed_trade_count() == 1
                && host.physical_position().signed_units == 0.0
                && host.last_error().empty() && host.last_run_status() == 0,
            "driver and actual trading outcomes disagree");
    const Trade trade = host.closed_trade(0);
    require(trade.entry_time == driver.bars.front().timestamp
                && trade.exit_time == driver.bars.back().timestamp
                && trade.entry_price == driver.bars.front().close
                && trade.exit_price == driver.level(warmup + n - 1)
                && trade.qty == 1.0 && trade.entry_id == entry_id && trade.exit_id == exit_id
                && trade.exit_from_bracket && !trade.open_at_end,
            "the final trade did not execute the held input-derived limit");
    result.transcript.trade(trade);
    result.transcript.add(host.broker_state_hash());
    return result;
}

void compare(const Result& resident, const Result& oracle) {
    require(resident.transcript.bytes == oracle.transcript.bytes
                && resident.logical_calls == oracle.logical_calls
                && resident.logical_bytes == oracle.logical_bytes,
            "ZERO-DIFF: trades/digests/handles/scopes/sequences/logical I/O changed");
}

} // namespace witness

extern "C" ssize_t pread(int fd, void* data, std::size_t bytes, off_t offset) {
    static_assert(sizeof(off_t) == 8 && sizeof(long) == 8, "64-bit Linux syscall witness");
    const auto result = static_cast<ssize_t>(::syscall(SYS_pread64, fd, data, bytes, offset));
    witness::positional(fd, bytes, result, false);
    return result;
}
extern "C" ssize_t pwrite(int fd, const void* data, std::size_t bytes, off_t offset) {
    const auto result = static_cast<ssize_t>(::syscall(SYS_pwrite64, fd, data, bytes, offset));
    witness::positional(fd, bytes, result, true);
    return result;
}

int main() {
    try {
        const char* selected = std::getenv("PINEFORGE_COMMAND_SCRATCH_DIR");
        const bool had_directory = selected != nullptr;
        const std::string previous = selected ? selected : "";
        witness::require(::setenv("PINEFORGE_COMMAND_SCRATCH_DIR", ".", 1) == 0,
                         "select the actual lab disk directory");
        struct RestoreDirectory {
            bool had;
            const std::string& previous;
            ~RestoreDirectory() {
                if (had) ::setenv("PINEFORGE_COMMAND_SCRATCH_DIR", previous.c_str(), 1);
                else ::unsetenv("PINEFORGE_COMMAND_SCRATCH_DIR");
            }
        } restore{had_directory, previous};
        witness::Physical first;
        bool have_first = false;
        for (const witness::Count n : {1000, 10000}) {
            const auto resident = witness::run(n, witness::Mode::resident);
            const auto oracle = witness::run(n, witness::Mode::write_through);
            const auto tiny = witness::run(n, witness::Mode::tiny);
            witness::compare(resident, oracle);
            witness::compare(resident, tiny);
            // Bound is derived: all live page ids fit distinct resident slots;
            // no explicit sync/end occurs in RUN. Therefore zero inner I/O.
            witness::require(resident.physical.writes == 0 && resident.physical.reads == 0
                                 && resident.physical.write_bytes == 0
                                 && resident.physical.read_bytes == 0,
                             "RED: held POOC exit serialized to disk on a non-spilling bar");
            witness::require(oracle.physical.writes > 0 && tiny.physical.writes > 0
                                 && tiny.physical.reads > 0,
                             "both oracle paths must perform real physical I/O");
            if (!have_first) {
                first = resident.physical;
                have_first = true;
            } else {
                witness::require(first.reads == resident.physical.reads
                                     && first.writes == resident.physical.writes,
                                 "physical I/O grew with held-bar population");
            }
            std::printf("POOC resident N=%llu physical_read=%llu physical_write=%llu ZERO-DIFF\n",
                        static_cast<unsigned long long>(n),
                        static_cast<unsigned long long>(resident.physical.reads),
                        static_cast<unsigned long long>(resident.physical.writes));
        }
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "FAIL POOC write-behind: %s\n", error.what());
        return 1;
    }
}
