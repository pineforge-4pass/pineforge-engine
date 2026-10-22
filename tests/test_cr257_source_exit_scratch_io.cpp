// CR-257 T1. Edit-only RED specification; execution belongs to the lab.
// No fault injection, replacement factory, filename matching, or canned report.
#if !defined(__linux__)
#error "CR-257 syscall accounting requires the Linux linker-wrap target"
#endif

#include "cr257_io.hpp"
#include "../src/command_scratch_production.hpp"
#include "../src/source/exit_scratch_versions_production.hpp"

#include <pineforge/source/exit_scratch.hpp>
#include <pineforge/source/pine_strategy_host.hpp>

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <limits>
#include <string>
#include <sys/stat.h>
#include <sys/types.h>
#include <type_traits>
#include <unistd.h>
#include <vector>

namespace cr257 {
bool measuring = false;
std::size_t platform_page = 0;
Counters counters{};
int failures = 0;

namespace {
struct LastPageIo {
    bool valid = false;
    bool write = false;
    int fd = -1;
    off_t offset = 0;
};
LastPageIo last_page_io;

void record(Calls& call, std::size_t requested, ssize_t result) noexcept {
    ++call.calls;
    call.bytes += requested;
    if (result < 0) ++call.errors;
    else {
        call.transferred += static_cast<Count>(result);
        call.short_io += static_cast<std::size_t>(result) != requested;
    }
}
bool anonymous_page(int fd, std::size_t bytes) noexcept {
    if (bytes != platform_page) return false;
    struct stat status{};
    if (::fstat(fd, &status) != 0) {
        counters.classification_error = true;
        return false;
    }
    if (!S_ISREG(status.st_mode) || status.st_nlink != 0) return false;
    if (counters.page_fd < 0) counters.page_fd = fd;
    else if (counters.page_fd != fd) counters.multiple_page_fds = true;
    return true;
}
void data_io(int fd, std::size_t bytes, off_t offset, ssize_t result, bool write) noexcept {
    if (!measuring) return;
    const int saved = errno;
    record(write ? counters.total.pwrite : counters.total.pread, bytes, result);
    const bool page = anonymous_page(fd, bytes);
    if (page) record(write ? counters.pages.pwrite : counters.pages.pread, bytes, result);
    last_page_io = {page && result == static_cast<ssize_t>(bytes), write, fd, offset};
    errno = saved;
}
void advice(Io& io, off_t bytes, int kind, int result) noexcept {
    ++io.fadvise.calls;
    if (bytes > 0) io.fadvise.bytes += static_cast<Count>(bytes);
    io.fadvise.errors += result != 0;
    io.dontneed += kind == POSIX_FADV_DONTNEED;
    io.random += kind == POSIX_FADV_RANDOM;
    io.whole_file_advice += bytes == 0;
}
} // namespace

Measure::Measure() {
    require(!measuring && platform_page != 0, "CR-257 invalid/nested measurement");
    counters = {};
    last_page_io = {};
    measuring = true;
}
Measure::~Measure() { measuring = false; }
Counters Measure::finish() {
    require(measuring, "CR-257 measurement finished twice");
    measuring = false;
    return counters;
}
void validate(const Counters& io) {
    require(!io.classification_error && !io.multiple_page_fds,
            "CR-257 page I/O classification failed or found another backing");
    require(io.total.pread.errors == 0 && io.total.pwrite.errors == 0
            && io.total.pread.short_io == 0 && io.total.pwrite.short_io == 0
            && io.total.pread.bytes == io.total.pread.transferred
            && io.total.pwrite.bytes == io.total.pwrite.transferred
            && io.total.fadvise.errors == 0 && io.total.fdatasync.errors == 0,
            "CR-257 RUN encountered a real I/O error or short transfer");
}
void bound(const char* metric, Count actual, Count allowed, Context context) {
    require(context.n != 0, "CR-257 missing measured bar/chunk count");
    const bool ok = actual <= allowed;
    std::printf("X257 %s total=%llu per_bar=%.9Lf allowed=%llu"
                " extent_pages=%llu records_changed=%llu N=%llu %s\n",
        metric, static_cast<unsigned long long>(actual),
        static_cast<long double>(actual) / context.n,
        static_cast<unsigned long long>(allowed),
        static_cast<unsigned long long>(context.extent_pages),
        static_cast<unsigned long long>(context.records_changed),
        static_cast<unsigned long long>(context.n), ok ? "PASS" : "FAIL");
    if (!ok) ++failures;
}
} // namespace cr257

extern "C" ssize_t __real_pread(int, void*, std::size_t, off_t);
extern "C" ssize_t __real_pwrite(int, const void*, std::size_t, off_t);
extern "C" int __real_posix_fadvise(int, off_t, off_t, int);
extern "C" int __real_fdatasync(int);

extern "C" ssize_t __wrap_pread(int fd, void* data, std::size_t bytes, off_t offset) {
    const auto result = __real_pread(fd, data, bytes, offset);
    cr257::data_io(fd, bytes, offset, result, false);
    return result;
}
extern "C" ssize_t __wrap_pwrite(int fd, const void* data, std::size_t bytes, off_t offset) {
    const auto result = __real_pwrite(fd, data, bytes, offset);
    cr257::data_io(fd, bytes, offset, result, true);
    return result;
}
extern "C" int __wrap_posix_fadvise(int fd, off_t offset, off_t bytes, int kind) {
    const auto result = __real_posix_fadvise(fd, offset, bytes, kind);
    if (cr257::measuring) {
        cr257::advice(cr257::counters.total, bytes, kind, result);
        if (fd == cr257::counters.page_fd)
            cr257::advice(cr257::counters.pages, bytes, kind, result);
    }
    return result;
}
extern "C" int __wrap_fdatasync(int fd) {
    const auto result = __real_fdatasync(fd);
    if (cr257::measuring) {
        ++cr257::counters.total.fdatasync.calls;
        cr257::counters.total.fdatasync.errors += result != 0;
        if (fd == cr257::counters.page_fd) {
            ++cr257::counters.pages.fdatasync.calls;
            cr257::counters.pages.fdatasync.errors += result != 0;
        }
    }
    return result;
}

namespace cr257 {
namespace {
using namespace pineforge;
namespace pd = pineforge::detail;
namespace sd = pineforge::source::detail;

// Typed, read-only access, as in the adapter and payload-codec fixtures.
// No layout cast, production hook, extra I/O, or mutation of the measured store.
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
    require(state != nullptr, "CR-257 command store has no workspace");
    return *state;
}

struct CommandObserver {
    pd::scratch_impl::Core* store = nullptr;
    Count reads = 0, writes = 0, syncs = 0, extent_pages = 0;
    Count commits = 0, inserts = 0, created = 0, erased = 0;
    Count writes_at_commit = 0, maximum_commit_writes = 0;
    bool invalid = false;
    pd::ScratchIoObserver previous;

    // Backings snapshot this registration. Construct BEFORE the host and all
    // its stores, and keep the context alive until AFTER host destruction.
    CommandObserver()
        : previous(pd::set_command_scratch_io_observer({this, &receive})) {}
    ~CommandObserver() { pd::set_command_scratch_io_observer(previous); }
    static void receive(void* opaque, const pd::ScratchIoCharge& charge) noexcept {
        if (!measuring) return;
        auto& self = *static_cast<CommandObserver*>(opaque);
        if (!self.store || charge.kind != pd::ScratchBackingKind::disk
            || charge.status != pd::ScratchStatus::ok) {
            self.invalid = true;
            return;
        }
        if (charge.operation == pd::ScratchIoOperation::sync) {
            ++self.syncs;
            self.invalid |= charge.page != pd::scratch_no_page || charge.bytes != 0;
            return;
        }
        const bool write = charge.operation == pd::ScratchIoOperation::write_page;
        self.invalid |= charge.bytes != platform_page || !last_page_io.valid
            || last_page_io.write != write || last_page_io.fd != counters.page_fd
            || last_page_io.offset < 0
            || static_cast<Count>(last_page_io.offset) != charge.page * platform_page;
        last_page_io.valid = false;
        if (!write) {
            ++self.reads;
            return;
        }
        ++self.writes;
        self.extent_pages = std::max(self.extent_pages, charge.page + 1);
        if (charge.page >= 2) return;

        // Buffered commit_impl on THIS head makes NO sync call. Its exactly
        // one publication is the alternating superblock write (pages 0/1).
        // Read the in-flight logical counters only; doing a store read here
        // would contaminate the measurement and recurse into this observer.
        const auto& now = self.store->cur_;
        const auto& before = self.store->snap_;
        self.invalid |= !self.store->active_ || self.store->poisoned_
            || charge.page != ((before.seq + 1) & 1)
            || now.next_handle < before.next_handle;
        const Count made = now.next_handle - before.next_handle;
        if (before.record_count + made < now.record_count) {
            self.invalid = true;
            return;
        }
        const Count removed = before.record_count + made - now.record_count;
        // The planned change batches pure scope erasures. Mixed insert/erase
        // transactions would require a separately stated accounting contract.
        self.invalid |= made != 0 && removed != 0;
        // An empty commit cannot inflate the insertion allowance. A genuine
        // replacement has touched/journalled existing storage even though it
        // allocates no new handle (the adapter's groups.set() path).
        self.invalid |= made == 0 && removed == 0 && self.store->used_count_ == 0;
        self.created += made;
        self.erased += removed;
        self.inserts += removed == 0; // Includes a genuine upsert transaction.
        ++self.commits;
        self.maximum_commit_writes = std::max(self.maximum_commit_writes,
                                              self.writes - self.writes_at_commit);
        self.writes_at_commit = self.writes;
    }
};

constexpr Count kPeriod = 20;
constexpr Count kExitPhase = kPeriod - 2;
constexpr Count kWarmup = 2 * kPeriod;
constexpr std::int64_t kStart = INT64_C(1736121600000);
constexpr std::int64_t kMinute = 60000;
constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();
constexpr const char* kEntryId = "X257-entry";
constexpr const char* kExitId = "X257-bracket";

struct Signal {
    bool entry = false, bracket = false, is_long = true;
    double limit = kNaN, stop = kNaN;
};
struct ExpectedTrade {
    Count entry = 0, exit = 0;
    bool is_long = true;
    double entry_price = 0.0, exit_price = 0.0;
};
struct Driver {
    std::vector<Bar> bars;
    std::vector<Signal> signals;
    std::vector<ExpectedTrade> trades;
    Count pending_bars = 0;

    explicit Driver(Count n) {
        require(n > 0 && n % kPeriod == 0 && n + kWarmup
                <= static_cast<Count>(std::numeric_limits<int>::max()),
                "CR-257 invalid driver population");
        bool held = false, armed = false, long_side = true;
        Count opened = 0;
        double limit = 0.0, stop = 0.0;
        for (Count i = 0; i < kWarmup + n; ++i) {
            const Count phase = i % kPeriod;
            const Count cycle = i / kPeriod;
            Bar bar{};
            bar.timestamp = kStart + static_cast<std::int64_t>(i) * kMinute;
            bar.open = 100.0;
            bar.close = 100.0 + (phase % 4 == 1 ? 0.25 : phase % 4 == 3 ? -0.25 : 0.0);
            bar.high = 100.5;
            bar.low = 99.5;
            bar.volume = static_cast<double>(i + 1);
            // Alternate up/down wicks independently of the long/short signal:
            // all four combinations repeat, including both wins and stops.
            if (phase == kExitPhase) {
                if (cycle % 4 < 2) bar.high = 103.0;
                else bar.low = 97.0;
            }
            Signal signal;
            if (held && armed) {
                const bool limit_hit = long_side ? bar.high >= limit : bar.low <= limit;
                const bool stop_hit = long_side ? bar.low <= stop : bar.high >= stop;
                require(!(limit_hit && stop_hit), "CR-257 driver has an ambiguous bracket bar");
                if (limit_hit || stop_hit) {
                    trades.push_back({opened, i, long_side, 0.0,
                                      limit_hit ? limit : stop});
                    held = armed = false;
                } else {
                    ++pending_bars;
                }
            }
            if (phase == 0) {
                require(!held, "CR-257 signal sequence entered before its prior bracket closed");
                held = true;
                // Market entries submitted by the source calculation fill
                // at the next open. Brackets rest through the intervening
                // bars; every generated cycle contains that opening bar.
                opened = i + 1;
                long_side = cycle % 2 == 0;
                limit = long_side ? 102.0 : 98.0;
                stop = long_side ? 98.0 : 102.0;
                signal.entry = true;
            }
            signal.is_long = long_side;
            if (held) {
                signal.bracket = true;
                signal.limit = limit;
                signal.stop = stop;
                armed = true;
            }
            bars.push_back(bar);
            signals.push_back(signal);
        }
        require(!held && !trades.empty() && pending_bars > 0,
                "CR-257 driver is flat, non-trading, or ends before its exit");
        for (auto& trade : trades)
            trade.entry_price = bars[static_cast<std::size_t>(trade.entry)].open;
    }
};

// Independent reference: batch ingress and caller-owned memory backing with
// a full fixed-window store cache, versus streaming ingress, production disk
// backing and the host's two-page store cache. Both execute the SAME signals.
struct ReferenceScratch {
    std::vector<std::byte> bytes;
    pd::FixedMemoryScratchBacking backing;
    pd::CommandScratchStore store;
    sd::ExitScratchVersionsProduction versions;
    sd::ExitScratchContext context;
    ReferenceScratch()
        : bytes(2 * kCacheBytes),
          backing({bytes.data(), bytes.size()}, platform_page),
          store({kCacheBytes / platform_page, platform_page, 1, 1,
                 sd::exit_scratch_key_order(), pd::ScratchOpenMode::create_empty}, &backing),
          versions(store), context{store, {}} {
        require(backing.status() == pd::ScratchStatus::ok
                && store.status() == pd::ScratchStatus::ok,
                "CR-257 reference scratch geometry failed");
        context.versions = &versions;
    }
};

class TradingHost final : public source::PineStrategyHost {
public:
    const Driver& driver;
    ReferenceScratch* reference;
    Count callbacks = 0, opens = 0, closes = 0, pending_observations = 0;
    explicit TradingHost(const Driver& input, ReferenceScratch* ref = nullptr)
        : driver(input), reference(ref) {
        source::PineStrategyConfig config;
        config.initial_capital = 1000000.0;
        config.default_qty_type = static_cast<int>(QtyType::FIXED);
        config.default_qty_value = 1.0;
        config.process_orders_on_close = false;
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
        const Count index = callbacks++;
        require(index < driver.signals.size()
                && pine_bar_index() == static_cast<int>(index)
                && bar.timestamp == driver.bars[static_cast<std::size_t>(index)].timestamp,
                "CR-257 strategy skipped, duplicated or ignored an input bar");
        if (index == 0 && reference) {
            auto& execution = adapter(*this);
            const auto& original = sd::exit_scratch(execution);
            reference->context.run = original.run;
            reference->context.lots = original.lots;
            reference->context.working = original.working;
            execution.bind_exit_scratch(reference->context);
        }
        const auto& signal = driver.signals[static_cast<std::size_t>(index)];
        if (signal.bracket && !signal.entry && pending_order_count() > 0)
            ++pending_observations;
        if (signal.entry)
            strategy_entry(kEntryId, signal.is_long, kNaN, kNaN, 1.0, "X257 entry");
        if (signal.bracket)
            strategy_exit(kExitId, kEntryId, signal.limit, signal.stop,
                          kNaN, kNaN, kNaN, 100.0, "X257 exit");
    }
    void on_native_applied_event(const native_order::ExecutionAppliedEvent& event,
                                 const NativeDecisionContext&) override {
        opens += event.opened_units != 0.0;
        closes += event.closed_trade_count;
    }
};

// Encode every semantic scalar explicitly: no pointers or struct padding,
// no rounded numbers, and no digest that could conceal a differing result.
struct Canonical {
    std::vector<unsigned char> bytes;
    template<class T> void add(const T& value) {
        static_assert(std::is_arithmetic_v<T> || std::is_enum_v<T>);
        const auto* first = reinterpret_cast<const unsigned char*>(&value);
        bytes.insert(bytes.end(), first, first + sizeof(value));
    }
    void add(const std::string& value) {
        add(static_cast<Count>(value.size()));
        bytes.insert(bytes.end(), value.begin(), value.end());
    }
    void stats(const pf_trade_stats_t& s) {
#define X257_STAT(field) add(s.field)
        X257_STAT(num_trades); X257_STAT(num_wins); X257_STAT(num_losses); X257_STAT(num_even);
        X257_STAT(percent_profitable); X257_STAT(net_profit); X257_STAT(net_profit_pct);
        X257_STAT(gross_profit); X257_STAT(gross_profit_pct); X257_STAT(gross_loss);
        X257_STAT(gross_loss_pct); X257_STAT(profit_factor); X257_STAT(avg_trade);
        X257_STAT(avg_trade_pct); X257_STAT(avg_win); X257_STAT(avg_win_pct);
        X257_STAT(avg_loss); X257_STAT(avg_loss_pct); X257_STAT(ratio_avg_win_avg_loss);
        X257_STAT(largest_win); X257_STAT(largest_win_pct); X257_STAT(largest_loss);
        X257_STAT(largest_loss_pct); X257_STAT(commission_paid); X257_STAT(expectancy);
        X257_STAT(max_consecutive_wins); X257_STAT(max_consecutive_losses);
        X257_STAT(avg_bars_in_trade); X257_STAT(avg_bars_in_wins); X257_STAT(avg_bars_in_losses);
#undef X257_STAT
    }
    void equity_stats(const pf_equity_stats_t& s) {
#define X257_EQUITY(field) add(s.field)
        X257_EQUITY(max_equity_drawdown); X257_EQUITY(max_equity_drawdown_pct);
        X257_EQUITY(max_equity_runup); X257_EQUITY(max_equity_runup_pct);
        X257_EQUITY(buy_hold_return); X257_EQUITY(buy_hold_return_pct);
        X257_EQUITY(sharpe_monthly); X257_EQUITY(sortino_monthly);
        X257_EQUITY(sharpe_bar); X257_EQUITY(sortino_bar); X257_EQUITY(cagr);
        X257_EQUITY(calmar); X257_EQUITY(recovery_factor);
        X257_EQUITY(time_in_market_pct); X257_EQUITY(open_pl);
#undef X257_EQUITY
    }
    void trade(const TradeC& t) {
        add(t.entry_time); add(t.exit_time); add(t.entry_price); add(t.exit_price);
        add(t.pnl); add(t.pnl_pct); add(t.is_long); add(t.max_runup); add(t.max_drawdown);
        add(t.qty); add(t.commission); add(t.entry_bar_index); add(t.exit_bar_index);
        add(t.open_at_end);
    }
    void source_trade(const Trade& t) {
        add(t.entry_time); add(t.exit_time); add(t.entry_price); add(t.exit_price);
        add(t.qty); add(t.pnl); add(t.pnl_pct); add(t.is_long);
        add(t.entry_bar_index); add(t.exit_bar_index); add(t.entry_id); add(t.entry_comment);
        add(t.exit_comment); add(t.exit_id); add(t.exit_from_bracket); add(t.max_runup);
        add(t.max_drawdown); add(t.commission); add(t.entry_incarnation);
        add(t.open_at_end); add(t.close_cause);
    }
};
struct ReportOwner {
    ReportC value{};
    explicit ReportOwner(const TradingHost& host) { host.fill_report(&value); }
    ~ReportOwner() { BacktestEngine::free_report(&value); }
};
Canonical canonical(const TradingHost& host) {
    ReportOwner owner(host);
    const auto& r = owner.value;
    require(r.trades_len > 0 && r.trades && r.equity_curve_len > 0 && r.equity_curve,
            "CR-257 missing full trade/equity report");
    Canonical out;
    out.add(r.total_trades); out.add(r.trades_len); out.add(r.net_profit);
    out.add(r.input_bars_processed); out.add(r.script_bars_processed);
    out.add(r.security_feeds_total); out.add(r.security_eval_complete_total);
    out.add(r.security_eval_partial_total); out.add(r.magnifier_sub_bars_total);
    out.add(r.magnifier_sample_ticks_total); out.add(r.input_tf_seconds);
    out.add(r.script_tf_seconds); out.add(r.script_tf_ratio); out.add(r.needs_aggregation);
    out.add(r.bar_magnifier_enabled);
    for (int i = 0; i < r.trades_len; ++i) out.trade(r.trades[i]);
    out.add(r.equity_curve_len);
    for (std::int64_t i = 0; i < r.equity_curve_len; ++i) {
        const auto& point = r.equity_curve[i];
        out.add(point.time_ms); out.add(point.equity); out.add(point.open_profit);
    }
    out.stats(r.metrics.all); out.stats(r.metrics.longs); out.stats(r.metrics.shorts);
    out.equity_stats(r.metrics.equity);
    out.add(r.security_diag_len);
    for (int i = 0; i < r.security_diag_len; ++i) {
        const auto& d = r.security_diag[i];
        out.add(d.sec_id); out.add(d.feed_count); out.add(d.eval_complete_count);
        out.add(d.eval_partial_count);
    }
    // These optional outputs are deliberately disabled in BOTH independent
    // runs. A new nonempty output cannot silently escape the comparison.
    require(r.trace_len == 0 && r.trace_names_len == 0 && r.broker_state_hash_len == 0,
            "CR-257 unexpected optional output requires canonical encoding");
    out.add(r.trace_len); out.add(r.trace_names_len); out.add(r.broker_state_hash_len);
    out.add(static_cast<Count>(host.closed_trade_count()));
    for (Count i = 0; i < host.closed_trade_count(); ++i)
        out.source_trade(host.closed_trade(static_cast<std::size_t>(i)));
    return out;
}
void validate_trading(const TradingHost& host, const Driver& driver) {
    require(host.last_error().empty() && host.last_run_status() == 0,
            "CR-257 engine run failed");
    require(host.callbacks == driver.bars.size()
            && host.script_bars_processed() == static_cast<std::int64_t>(driver.bars.size())
            && host.closed_trade_count() > 0
            && host.closed_trade_count() == driver.trades.size()
            && host.closes == driver.trades.size() && host.opens == driver.trades.size()
            && host.pending_observations > 0 && host.physical_position().signed_units == 0.0,
            "CR-257 driver/engine trade count, pending brackets or bar count disagree");
    for (Count i = 0; i < driver.trades.size(); ++i) {
        const auto row = host.closed_trade(static_cast<std::size_t>(i));
        const auto& expected = driver.trades[static_cast<std::size_t>(i)];
        require(row.entry_time == driver.bars[expected.entry].timestamp
                && row.exit_time == driver.bars[expected.exit].timestamp
                && row.entry_price == expected.entry_price && row.exit_price == expected.exit_price
                && row.entry_bar_index == static_cast<int>(expected.entry)
                && row.exit_bar_index == static_cast<int>(expected.exit)
                && row.is_long == expected.is_long && row.qty == 1.0
                && row.entry_id == kEntryId && row.exit_from_bracket && !row.open_at_end,
                "CR-257 trade departed from the independent signal/OHLC driver");
    }
}
} // namespace

SourceRun source_run(Count n) {
    Driver driver(n);
    SourceRun result;
    result.context.n = n;
    Canonical measured;
    {
        CommandObserver observer; // BEFORE stores; AFTER host on destruction.
        TradingHost host(driver);
        require(host.stream_begin(driver.bars.data(), static_cast<int>(kWarmup), "1", "1"),
                "CR-257 trading warmup failed");
        auto& context = sd::exit_scratch(adapter(host));
        observer.store = &core(context.store);
        const auto before = context.store.stats();
        const auto sequence_before = observer.store->cur_.seq;
        const auto handles_before = observer.store->cur_.next_handle;
        const auto closes_before = host.closed_trade_count();
        require(context.store.record_count() == 0 && !context.store.transaction_active()
                && closes_before > 0 && host.pending_order_count() == 0,
                "CR-257 warmup did not finish with a settled, trading source store");
        Measure run;
        for (Count i = kWarmup; i < driver.bars.size(); ++i)
            require(host.stream_push_bar(driver.bars[static_cast<std::size_t>(i)]),
                    "CR-257 trading RUN failed");
        result.io = run.finish(); // ONLY push loop: no setup, end, report, destructor.
        const auto after = context.store.stats();
        result.page_reads = observer.reads;
        result.page_writes = observer.writes;
        result.sync_charges = observer.syncs;
        result.commits = observer.commits;
        result.insert_commits = observer.inserts;
        result.records_created = observer.created;
        result.context.extent_pages = observer.extent_pages;
        result.context.records_changed = observer.erased;
        validate(result.io);
        require(!observer.invalid && observer.extent_pages > 0 && observer.writes > 0
                && result.io.page_fd >= 0
                && observer.writes == result.io.pages.pwrite.calls
                && observer.reads == result.io.pages.pread.calls
                && observer.writes == after.page_writes - before.page_writes
                // A cache ABOVE ScratchBacking can serve a logical store read
                // without issuing a backing read/charge. Physical reads still
                // match the wrapper exactly; store attempts may exceed them.
                && observer.reads <= after.page_reads - before.page_reads
                && observer.syncs == after.syncs - before.syncs
                && observer.commits == observer.store->cur_.seq - sequence_before
                && observer.created == observer.store->cur_.next_handle - handles_before,
                "CR-257 observer/wrappers/actual source store disagree or missed RUN I/O");
        require(observer.commits > 0 && observer.created > 0 && observer.erased == observer.created
                && observer.inserts > 0 && context.store.record_count() == 0
                && !context.store.transaction_active() && host.closed_trade_count() > closes_before
                && observer.extent_pages <= after.backing_pages,
                "CR-257 temporary records/commits or measured trades are vacuous");
        // This is the geometric premise of the cold-once read bound, not a
        // population-tuned allowance. Check the actual backing as well as the
        // max page+1 of write charges produced INSIDE RUN.
        require(after.backing_pages * platform_page <= kCacheBytes,
                "CR-257 fixture working set does not fit the fixed read cache");
        bound("command.pages_per_commit", observer.maximum_commit_writes,
              pd::kPagesPerCommit, result.context);
        require(host.stream_end(), "CR-257 trading stream end failed");
        validate_trading(host, driver);
        measured = canonical(host);
    }
    require(!measuring, "CR-257 reference accidentally measured");
    ReferenceScratch memory; // Independent bytes/cache, fixed size, unrelated to N.
    TradingHost reference(driver, &memory);
    reference.run(driver.bars.data(), static_cast<int>(driver.bars.size()), "1", "1", false);
    validate_trading(reference, driver);
    const Canonical expected = canonical(reference);
    require(measured.bytes == expected.bytes,
            "CR-257 ZERO-DIFF: disk/stream full report differs from independent memory/batch report");
    std::printf("X257 zero_diff bytes=%zu trades=%zu pending_bars=%llu N=%llu PASS\n",
        measured.bytes.size(), driver.trades.size(),
        static_cast<unsigned long long>(driver.pending_bars),
        static_cast<unsigned long long>(n));
    return result;
}

SourceRun scope_erase_run(Count records) {
    require(records > sd::kExitScopeEraseBatch,
            "CR-257 erase fixture must exercise a full batch and a partial batch");
    CommandObserver observer; // Register before creating the actual backing.
    auto backing = pd::make_command_scratch_backing(platform_page);
    pd::CommandScratchStore store({2, platform_page, 1, 1, sd::exit_scratch_key_order(),
                                   pd::ScratchOpenMode::create_empty}, backing.get());
    require(store.status() == pd::ScratchStatus::ok, "CR-257 erase fixture store failed");
    sd::ExitScratchContext context{store, {}};
    sd::ExitScratchRows<Count> rows(context);
    for (Count i = 0; i < records; ++i) rows.push_back(i * 17 + 3);
    require(rows.size() == records && store.record_count() == records,
            "CR-257 erase fixture did not insert its real temporary records");
    for (Count i = 0; i < records; ++i)
        require(rows[static_cast<std::size_t>(i)] == i * 17 + 3,
                "CR-257 erase fixture ignored its stored payloads");
    observer.store = &core(store);
    const auto before = store.stats();
    const auto sequence_before = observer.store->cur_.seq;
    SourceRun result;
    result.context.n = records;
    Measure run;
    rows.clear(); // The actual production helper, with no local imitation.
    result.io = run.finish();
    const auto after = store.stats();
    validate(result.io);
    require(!observer.invalid && observer.erased == records && observer.created == 0
            && observer.inserts == 0 && observer.commits > 0
            && observer.extent_pages > 0 && observer.writes > 0
            && observer.writes == result.io.pages.pwrite.calls
            && observer.reads == result.io.pages.pread.calls
            && observer.writes == after.page_writes - before.page_writes
            && observer.reads <= after.page_reads - before.page_reads
            && observer.syncs == after.syncs - before.syncs
            && observer.commits == observer.store->cur_.seq - sequence_before
            && store.record_count() == 0 && rows.empty() && !store.transaction_active(),
            "CR-257 actual scope clear missed records, publications or wrapped I/O");
    result.context.extent_pages = observer.extent_pages;
    result.context.records_changed = observer.erased;
    result.page_reads = observer.reads;
    result.page_writes = observer.writes;
    result.sync_charges = observer.syncs;
    result.commits = observer.commits;
    // Clear must leave the store reusable, including its free lists. These
    // writes/readbacks are outside RUN and use different values and a new scope.
    sd::ExitScratchRows<Count> reused(context);
    for (Count i = 0; i < records; ++i) reused.push_back(i * 29 + 7);
    for (Count i = 0; i < records; ++i)
        require(reused[static_cast<std::size_t>(i)] == i * 29 + 7,
                "CR-257 batched erase damaged subsequent scope reuse");
    require(store.record_count() == records && rows.empty(),
            "CR-257 cleared scope retained new scope membership");
    return result;
}

void source_bounds(const SourceRun& run) {
    const auto& io = run.io.total;
    const auto& pages = run.io.pages;
    const auto context = run.context;
    const Count read_calls = kReadFactor * context.extent_pages;
    bound("command.page_pread_calls", pages.pread.calls, read_calls, context);
    bound("command.total_pread_calls", io.pread.calls, read_calls, context);
    bound("command.page_pread_bytes", pages.pread.bytes, read_calls * platform_page, context);
    bound("command.total_pread_bytes", io.pread.bytes, read_calls * platform_page, context);
    const Count erase_commits = ceil_div(context.records_changed, sd::kExitScopeEraseBatch);
    const Count allowed_commits = erase_commits + run.insert_commits;
    const Count write_calls = allowed_commits * pd::kPagesPerCommit;
    bound("command.commits", run.commits, allowed_commits, context);
    bound("command.page_pwrite_calls", pages.pwrite.calls, write_calls, context);
    bound("command.total_pwrite_calls", io.pwrite.calls, write_calls, context);
    bound("command.page_pwrite_bytes", pages.pwrite.bytes, write_calls * platform_page, context);
    bound("command.total_pwrite_bytes", io.pwrite.bytes, write_calls * platform_page, context);
    bound("command.fadvise_calls", io.fadvise.calls,
          ceil_div(io.pread.bytes, kCacheBytes) + 1, context);
    bound("command.fdatasync_calls", io.fdatasync.calls,
          ceil_div(io.pwrite.bytes, kCacheBytes) + 1, context);
    std::printf("X257 command.charges read=%llu write=%llu sync=%llu commits=%llu"
                " insert_commits=%llu records_created=%llu N=%llu\n",
        static_cast<unsigned long long>(run.page_reads),
        static_cast<unsigned long long>(run.page_writes),
        static_cast<unsigned long long>(run.sync_charges),
        static_cast<unsigned long long>(run.commits),
        static_cast<unsigned long long>(run.insert_commits),
        static_cast<unsigned long long>(run.records_created),
        static_cast<unsigned long long>(context.n));
}
} // namespace cr257

int main(int argc, char** argv) {
    try {
        const long page = ::sysconf(_SC_PAGESIZE);
        cr257::require(page > 0, "CR-257 platform page size query failed");
        cr257::platform_page = static_cast<std::size_t>(page);
        cr257::require(cr257::kCacheBytes % cr257::platform_page == 0
                       && cr257::kCacheBytes / cr257::platform_page >= 2,
                       "CR-257 lab geometry needs at least two pages per read window");
        const std::string mode = argc == 1 ? "source" : argv[1];
        cr257::require(argc <= 2, "CR-257 unexpected command-line arguments");
        if (mode == "source") cr257::source_bounds(cr257::source_run(cr257::kSmallN));
        else if (mode == "source-100k") cr257::source_100k();
        else if (mode == "dontneed") cr257::scratch_file_io();
        else if (mode == "erase-scope") cr257::exit_scope_io();
        else throw std::runtime_error("CR-257 unknown test mode");
        return cr257::failures == 0 ? 0 : 1;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "CR-257 setup/result-integrity failure: %s\n", error.what());
        return 1;
    }
}
