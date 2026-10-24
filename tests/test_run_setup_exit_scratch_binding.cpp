// LAB-O1SEAMS s1c: a real Pine run must provision its exit readers at begin.
// On d755bd69, prepare_script_run reaches exit_scratch_lots before any bar;
// it throws ExitScratchError for the unset reader. The engine records that
// coded exception; require_healthy rethrows its actual code and text. After
// binding, a real relative global exit also exercises the nonempty book.
// Round 6 also drives a bare NativeStrategyHost through reversal and a
// terminal dependency cascade, then reuses it. Its APIs exist on 461127bc65.
// This edit-only lane has not compiled or executed the test.
#include <pineforge/source/pine_strategy_host.hpp>
#include "../src/native_execution_consumer.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <dirent.h>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <unistd.h>
#include <utility>
#include <variant>
#include <vector>
#if defined(__APPLE__)
#include <fcntl.h>
#include <sys/stat.h>
#endif

namespace pineforge {
inline namespace engine_script_run_v19 {
// Observe the real run's workspace; never install or mutate it from the test.
// These members and this friend seam already exist on 461127bc65.
struct NativeExecutionConsumerProbe {
    static const detail::CommandScratchStore* dependency(const NativeExecutionConsumer& consumer) {
        return consumer.dependency_scratch_;
    }
    static std::uint64_t queued(const NativeExecutionConsumer& consumer) {
        return consumer.dependency_scratch_stats_.queued_rows;
    }
};
} // inline namespace engine_script_run_v19
} // namespace pineforge

namespace {
using namespace pineforge;
using Status = detail::ScratchStatus;
constexpr double kNa = std::numeric_limits<double>::quiet_NaN();

template<class Tag, typename Tag::type Member>
struct PrivateAccess {
    friend typename Tag::type access(Tag) { return Member; }
};
struct TradeScratchTag {
    using type = detail::CommandScratchStore* source::PineStrategyHost::*;
    friend type access(TradeScratchTag);
};
template struct PrivateAccess<TradeScratchTag, &source::PineStrategyHost::trade_scratch_>;

const detail::CommandScratchStore* trade_store(const source::PineStrategyHost& host) {
    return host.*access(TradeScratchTag{});
}

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

void require_healthy(const NativeStrategyHost& host) {
    if (host.last_run_status() != 0 || !host.last_error().empty())
        throw coded<std::logic_error>(run_failure_value_of(host), host.last_error());
}

void require_completed(const NativeStrategyHost& host) {
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

std::string descriptor_target(int descriptor) {
#if defined(__APPLE__)
    struct stat status{};
    require(::fstat(descriptor, &status) == 0, "inspect descriptor type");
    if (!S_ISREG(status.st_mode)) return "<non-regular descriptor>";
    std::array<char, 4096> target{};
    if (::fcntl(descriptor, F_GETPATH, target.data()) == 0) return target.data();
#else
    const std::string link = "/proc/self/fd/" + std::to_string(descriptor);
    std::vector<char> target(256);
    for (;;) {
        const auto size = ::readlink(link.c_str(), target.data(), target.size());
        if (size < 0) break;
        if (static_cast<std::size_t>(size) < target.size())
            return {target.data(), static_cast<std::size_t>(size)};
        target.resize(target.size() * 2);
    }
#endif
    const int error = errno;
    throw std::runtime_error("descriptor target unavailable: fd=" + std::to_string(descriptor)
                             + " errno=" + std::to_string(error));
}

using DescriptorSnapshot = std::vector<std::pair<int, std::string>>;

DescriptorSnapshot snapshot_descriptors(const std::vector<int>& descriptors) {
    DescriptorSnapshot result;
    for (const int descriptor : descriptors)
        result.emplace_back(descriptor, descriptor_target(descriptor));
    return result;
}

bool descriptor_has_prefix(const std::string& target, const std::string& prefix) {
    const auto slash = target.find_last_of('/');
    const auto name = slash == std::string::npos ? 0 : slash + 1;
    return target.compare(name, prefix.size(), prefix) == 0;
}

std::size_t command_scratch_descriptor_count(const DescriptorSnapshot& descriptors) {
    return static_cast<std::size_t>(std::count_if(descriptors.begin(), descriptors.end(),
        [](const auto& descriptor) {
            return descriptor_has_prefix(descriptor.second, "pineforge-command-");
        }));
}

bool descriptors_match_with_trade_ledger(const DescriptorSnapshot& before,
                                         const DescriptorSnapshot& after) {
    for (const auto& descriptor : before) {
        if (std::find(after.begin(), after.end(), descriptor) == after.end())
            return false;
    }
    bool extra_index = false;
    bool extra_payload = false;
    for (const auto& descriptor : after) {
        if (std::find(before.begin(), before.end(), descriptor) != before.end())
            continue;
        if (descriptor_has_prefix(descriptor.second, "pineforge-trade-index-")) {
            if (extra_index) return false;
            extra_index = true;
        } else if (descriptor_has_prefix(descriptor.second, "pineforge-trade-payload-")) {
            if (extra_payload) return false;
            extra_payload = true;
        } else {
            return false;
        }
    }
    return true;
}

const char* descriptor_kind(const std::string& target) {
    if (descriptor_has_prefix(target, "pineforge-command-")) return "command scratch";
    if (descriptor_has_prefix(target, "pineforge-trade-index-")) return "trade ledger index";
    if (descriptor_has_prefix(target, "pineforge-trade-payload-")) return "trade ledger payload";
    if (descriptor_has_prefix(target, "pineforge-roots-")) return "successor root index";
    if (descriptor_has_prefix(target, "pineforge-scratch-")) return "report scratch";
    if (descriptor_has_prefix(target, "pineforge-ids-")) return "ID table";
    if (descriptor_has_prefix(target, "pineforge-id-pairs-")) return "ID pair scratch";
    if (descriptor_has_prefix(target, "pineforge-sequence-")) return "scratch sequence";
    return "other / unclassified";
}

void report_descriptor_changes(const DescriptorSnapshot& before, const DescriptorSnapshot& after,
                               const char* phase) {
    if (before == after) return;
    std::fprintf(stderr, "test_run_setup_exit_scratch_binding: %s descriptor changes:\n", phase);
    for (const auto& descriptor : after) {
        if (std::find(before.begin(), before.end(), descriptor) == before.end())
            std::fprintf(stderr, "  extra fd=%d kind=%s target=%s\n", descriptor.first,
                         descriptor_kind(descriptor.second), descriptor.second.c_str());
    }
    for (const auto& descriptor : before) {
        if (std::find(after.begin(), after.end(), descriptor) == after.end())
            std::fprintf(stderr, "  removed fd=%d kind=%s target=%s\n", descriptor.first,
                         descriptor_kind(descriptor.second), descriptor.second.c_str());
    }
}

class ReversingNativeHost final : public NativeStrategyHost {
public:
    void on_native_run_begin() override {
        calculations = 0;
        require(physical_position().signed_units == 0.0 && native_open_lots(kNa).empty(),
                "a reused native host starts with no position or lots");
        require(native_working_requests().empty(), "a reused native host starts with no requests");
        ++beginnings;
        // Observe emptiness when provisioned, but let the unchanged head reach
        // the production invariant during the actual dependency drain.
        if (const auto* store = NativeExecutionConsumerProbe::dependency(consumer())) {
            require(store->record_count() == 0 && !store->transaction_active(),
                    "each native begin has empty, inactive dependency scratch");
            std::vector<std::byte> page(store->page_size());
            std::uint64_t digest = 0;
            require(store->state_digest({page.data(), page.size()}, digest) == Status::ok,
                    "read the beginning dependency state");
            if (beginnings == 1) initial_scratch_digest = digest;
            else require(digest == initial_scratch_digest,
                         "restart resets scratch handles as well as rows");
        }
    }

    void on_native_bar(const Bar&, const NativeDecisionContext&) override {
        namespace no = native_order;
        if (calculations++ == 0) {
            execute(no::Request{no::Transact{1.0}, "long", ""});
            require_healthy(*this);
            require(physical_position().signed_units > 0.0, "the first execution opens long");
            return;
        }
        if (calculations == 2) {
            execute(no::Request{no::Transact{-2.0}, "reverse", ""});
            require_healthy(*this);
            require(physical_position().signed_units < 0.0, "the next execution reverses to short");
            ++reversals;

            // The next bar matches this close-only parent. It opens no lot,
            // so its closing child retires UnsupportedRelation, and the
            // dependency queue must retire that child's waiting descendant.
            const auto parent = put(no::Request{no::Flatten{}, "parent", ""});
            no::Request child{no::Reduce{no::ExplicitUnits{1.0}}, "child", ""};
            child.owner = no::WaitForApplied{parent};
            const auto child_handle = put(child);
            no::Request grandchild{no::Transact{1.0}, "grandchild", ""};
            grandchild.owner = no::WaitForApplied{child_handle};
            put(grandchild);
            return;
        }
        if (calculations != 3) return;
        require_healthy(*this);
        require(physical_position().signed_units == 0.0, "the close-only parent executed");
        require(native_working_requests().empty(), "the terminal cascade retires both descendants");
        require(NativeExecutionConsumerProbe::queued(consumer()) != 0,
                "the real dependency queue processed waiting requests");
        const auto* store = NativeExecutionConsumerProbe::dependency(consumer());
        require(store != nullptr && store->record_count() == 0 && !store->transaction_active(),
                "the drain restores its scratch rows and transaction");
        std::vector<std::byte> page(store->page_size());
        std::uint64_t digest = 0;
        require(store->state_digest({page.data(), page.size()}, digest) == Status::ok
                && digest == initial_scratch_digest,
                "the drain restores the complete beginning scratch state");
        ++drains;
    }

    NativeExecutionConsumer& consumer() { return as_native_consumer(execution_consumer()); }
    std::size_t calculations = 0;
    std::size_t beginnings = 0;
    std::size_t reversals = 0;
    std::size_t drains = 0;
    std::uint64_t initial_scratch_digest = 0;

private:
    native_order::RequestHandle put(const native_order::Request& request) {
        const auto accepted = submit(request);
        require(accepted.status == native_order::SubmitStatus::Accepted && accepted.handle,
                "native request accepted");
        return *accepted.handle;
    }

    void execute(const native_order::Request& request) {
        const auto result = execute_current({put(request), NativeCurrentPriceRule::AsPresented});
        require_healthy(*this);
        require(std::holds_alternative<native_order::ExecutionAppliedEvent>(result),
                "the native current-price execution applies");
    }
};

void run_owner_reversal_without_adapter_starts_each_run_empty() {
    const std::array<Bar, 3> bars{{
        {100.0, 100.0, 100.0, 100.0, 1.0, 1704067200000LL},
        {101.0, 101.0, 101.0, 101.0, 1.0, 1704067260000LL},
        {102.0, 102.0, 102.0, 102.0, 1.0, 1704067320000LL},
    }};
    NativeRunSpec spec;
    spec.identity = {"run-owner-command-scratch", 1};
    spec.input_tf = spec.script_tf = "1";
    spec.tickerid = "TEST:SCRATCH";
    spec.timezone = "UTC";
    spec.session = "24x7";
    spec.price_tick = 0.25;
    spec.initial_capital = 100000.0;
    spec.point_value = 1.0;
    spec.account_fx = 1.0;
    spec.close_execution = NativeCloseExecution::NextEligiblePoint;
    spec.event_retention = NativeEventRetention::Full;
    spec.report_policy = NativeReportPolicy::KernelRecorded;

    // Warm calendar setup with a bare native host before observing teardown.
    {
        ReversingNativeHost warm;
        require(warm.configure_native(spec).status == NativeSetupStatus::Applied, "configure warmup");
        warm.run(bars.data(), 1);
        require_completed(warm);
    }
    const auto before = snapshot_descriptors(open_descriptors());
    const auto before_command_files = command_scratch_descriptor_count(before);
    if (before_command_files != 0) report_descriptor_changes({}, before, "warmup teardown");
    require(before_command_files == 0, "warmup teardown releases command scratch backing files");
    ReversingNativeHost host;
    require(host.configure_native(spec).status == NativeSetupStatus::Applied, "configure first run");
    host.run(bars.data(), static_cast<int>(bars.size()));
    // On 461127bc65 the automatic parent match fails in the real queue before
    // the third calculation. This rethrows its recorded invariant verbatim.
    require_completed(host);
    require(host.reversals != 0 && host.drains != 0, "the first run reversed and drained");
    require(NativeExecutionConsumerProbe::dependency(host.consumer()) == nullptr,
            "run completion clears the dependency borrower");
    const auto after_first = snapshot_descriptors(open_descriptors());
    report_descriptor_changes(before, after_first, "run completion");
    // Only one extra trade index and one extra payload may remain open.
    require(descriptors_match_with_trade_ledger(before, after_first),
            "run completion releases command scratch backing files");
    const auto first_events = host.native_events(0).size();
    const auto first_hash = host.native_continuation_hash();
    ReportC first{};
    host.fill_report(&first);
    const auto first_trade_count = first.trades_len;
    const double first_profit = first.net_profit;
    require(first_trade_count > 0, "the reversal produces real report trades");
    BacktestEngine::free_report(&first);

    const auto beginnings = host.beginnings;
    const auto reversals = host.reversals;
    const auto drains = host.drains;
    ++spec.identity.run_number;
    require(host.configure_native(spec).status == NativeSetupStatus::Applied, "configure second run");
    host.run(bars.data(), static_cast<int>(bars.size()));
    require_completed(host);
    require(host.beginnings > beginnings && host.reversals > reversals && host.drains > drains,
            "the same host executes the second real run");
    require(host.native_events(0).size() == first_events,
            "restart replaces the preceding event tape");
    require(host.native_continuation_hash() == first_hash,
            "an empty restart preserves the normalized native continuation");
    ReportC second{};
    host.fill_report(&second);
    require(second.trades_len == first_trade_count && second.net_profit == first_profit,
            "restart replaces the preceding trade tape and account");
    BacktestEngine::free_report(&second);
    require(NativeExecutionConsumerProbe::dependency(host.consumer()) == nullptr,
            "restart completion leaves no dangling dependency borrower");
    const auto after_restart = snapshot_descriptors(open_descriptors());
    report_descriptor_changes(before, after_restart, "restart completion");
    require(descriptors_match_with_trade_ledger(before, after_restart),
            "restart releases every command scratch backing file");
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
    std::uint64_t initial_trade_digest = 0;

protected:
    void prepare_script_run(const Bar*, int, bool) override {
        require(callbacks == 0, "run setup precedes every source bar");
        const auto* trade = trade_store(*this);
        require(trade != nullptr && trade->record_count() == 0 && !trade->transaction_active(),
                "the run owner provides empty trade scratch before script preparation");
        std::vector<std::byte> page(trade->page_size());
        std::uint64_t digest = 0;
        require(trade->state_digest({page.data(), page.size()}, digest) == Status::ok,
                "read the beginning trade scratch state");
        if (preparations == 0) initial_trade_digest = digest;
        else require(digest == initial_trade_digest,
                     "restart resets trade scratch handles as well as rows");
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
    require(trade_store(host) == nullptr, "run completion clears the trade borrower");
    require(NativeExecutionConsumerProbe::dependency(
                NativeExecutionConsumer::bound(host)) == nullptr,
            "source run completion clears the dependency borrower");
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
    require(trade_store(host) != nullptr, "a live stream retains its trade workspace");
    require(NativeExecutionConsumerProbe::dependency(
                NativeExecutionConsumer::bound(host)) != nullptr,
            "a live stream retains its dependency workspace");
    require(host.stream_end(), "stream teardown succeeds with its binding alive");
    require(host.last_error().empty(), "stream teardown records no scratch failure");
    require(trade_store(host) == nullptr, "stream end clears the trade borrower");
    require(NativeExecutionConsumerProbe::dependency(
                NativeExecutionConsumer::bound(host)) == nullptr,
            "stream end clears the dependency borrower");
}

// Uses only the pre-fix interface. On 85d1af77 the first versions lookup
// reaches "exit scratch operation failed: context versions null".
// Fail-before and pass-after execution belong to the supervisor's box.
void run_setup_binds_versions_and_round_trips_scoped_payloads() {
    ExitHost host;
    const Bar warmup{100.0, 101.0, 99.0, 100.0, 1.0, 1704067200000LL};
    const bool begun = host.stream_begin(&warmup, 1, "1", "1");
    require_healthy(host);
    require(begun, "version test starts a real command-scratch run");
    auto& context = host.scratch();
    auto& versions = source::detail::exit_scratch_versions(context);
    require(context.versions != nullptr, "run setup installs its version provider");
    const auto before = context.store.record_count();
    const auto resident = context.store.stats().resident_bytes;
    const std::string label = "frozen-" + std::to_string(context.run.run_number)
        + std::string(context.store.page_size() * 3, 'p');
    const std::string comment{"retained\0comment", 16};
    const std::string replacement_key = label + "\x1f" + comment;
    native_order::Owner frozen;
    {
        source::detail::ExitScratchScope owner(context);
        std::uint64_t placement_version = 0, request_version = 0, pending_version = 0;
        {
            source::PlacementSnapshot placed;
            placed.family = source::PineOrderFamily::ExitStop;
            placed.source_id = label;
            placed.from_entry = comment;
            placed.comment = comment;
            placed.exit_levels.stop = warmup.low;
            placed.requested_qty = warmup.volume;
            placed.paired_reversal_parent = {context.run, owner.scope()};
            placed.sizing.price = warmup.close;
            placed.birth = OrderBirth::chart_evaluation(2, warmup.timestamp);
            placed.legs.attach(owner.scope(), 1);
            exit_legs::Prices prices;
            prices.stop_price = warmup.low;
            placed.legs.set_prices(prices);
            const exit_legs::Action cancel{placed.legs.target(), placed.legs.revision(),
                {1, 2, exit_legs::Domain::Ordinary, exit_legs::Phase::Observation},
                exit_legs::Cancel{{exit_legs::Leg::Stop}}};
            require(placed.legs.apply(placed.legs.target(), cancel) == exit_legs::Result::Applied,
                    "fixture has real lifecycle retirement and replay state");
            auto observation = std::make_shared<admission::CommandObservation>();
            observation->command = owner.scope();
            observation->id = label;
            observation->birth = placed.birth;
            placed.market_admission.bind(observation);
            placement_version = versions.placement(owner.scope(), placed);

            native_order::Request request;
            request.intent = native_order::Sized{native_order::Side::Short,
                native_order::EquityFraction{0.25}, native_order::SizeTime::AtAcceptance,
                native_order::SizePrice::SignalOnTick,
                native_order::ExecutionGridPolicy::ExplicitUnits, true};
            request.label = label;
            request.comment = comment;
            request.trigger = native_order::Trail{0.5, warmup.high,
                native_order::TrailTicks{2.0}, warmup.close};
            request.capacity = native_order::PointBudget{warmup.volume};
            request.owner = native_order::BindOpenings{
                {{context.run, owner.scope()}, {context.run, owner.scope() + 1}}, 1};
            request.group = native_order::Member{owner.scope(), 1, native_order::GroupEffect::Reduce};
            request.anchor = native_order::FromOwnerFill{
                -0.5, true, native_order::NativeAnchorRounding::Directional};
            request_version = versions.request(owner.scope(), request);
            pending_version = versions.pending(owner.scope(), request, placed, replacement_key);
            // Retained values must not alias either the caller or its shared pointees.
            observation->id = "changed";
            placed.source_id.clear();
            placed.legs.set_stop_price(warmup.high);
            request.label.clear();
        }
        std::uint64_t nested_version = 0;
        {
            source::detail::ExitScratchRows<native_order::RequestHandle> nested(context);
            require(nested.scope() != owner.scope(), "round trip crosses a scratch scope change");
            nested.push_back({context.run, owner.scope()});
            nested.push_back({context.run, nested.scope()});
            frozen = versions.freeze_openings(context.run, 1, nested.size(), &nested,
                [](void* opaque, std::uint64_t index, native_order::RequestHandle& result) {
                    const auto& rows = *static_cast<const source::detail::ExitScratchRows<
                        native_order::RequestHandle>*>(opaque);
                    if (index >= rows.size()) return Status::not_found;
                    result = rows[index];
                    return Status::ok;
                });
            source::PlacementSnapshot temporary;
            temporary.source_id = "nested";
            nested_version = versions.placement(nested.scope(), temporary);
        }
        detail::ScratchRecordInfo info{};
        require(context.store.point(nested_version, info) == Status::not_found,
                "nested scope releases its versions");
        require(context.store.point(placement_version, info) == Status::ok,
                "outer version payload remains in the run store after nested release");
        require(info.value_bytes > label.size(), "stored payload includes the full wide source id");
        std::string bytes(static_cast<std::size_t>(info.value_bytes), '\0');
        require(context.store.read(placement_version, detail::ScratchField::value, 0,
                    {reinterpret_cast<std::byte*>(bytes.data()), bytes.size()}) == Status::ok,
                "payload bytes are readable directly from the run store");
        require(bytes.find(label) != std::string::npos && bytes.find(comment) != std::string::npos,
                "the store owns the frozen text, including embedded NULs");

        auto placed = versions.placement(placement_version);
        require(placed.source_id == label && placed.from_entry == comment
                && placed.comment == comment && placed.exit_levels.stop == warmup.low
                && placed.requested_qty == warmup.volume && placed.sizing.price == warmup.close
                && placed.paired_reversal_parent
                    == native_order::RequestHandle{context.run, owner.scope()},
                "placement provider reproduces text, prices, sizing and handle identity");
        require(placed.birth.cause() == OrderBirthCause::ChartEvaluation
                && placed.birth.timestamp() == warmup.timestamp && placed.legs.revision() == 2
                && placed.legs.retired(exit_legs::Leg::Stop) && placed.legs.last_action()
                && placed.legs.prices().stop_price == warmup.low
                && placed.market_admission.observation()
                && placed.market_admission.observation()->id == label,
                "placement provider reproduces private lifecycle and admission values");
        require(placed.legs.apply(placed.legs.target(), *placed.legs.last_action())
                    == exit_legs::Result::Replay,
                "restored lifecycle recognizes the exact original action replay");
        const auto request = versions.request(request_version);
        const auto& sized = std::get<native_order::Sized>(request.intent);
        const auto& trail = std::get<native_order::Trail>(request.trigger);
        const auto& openings = std::get<native_order::BindOpenings>(request.owner);
        require(request.label == label && request.comment == comment
                && sized.side == native_order::Side::Short
                && std::get<native_order::EquityFraction>(sized.basis).fraction == 0.25
                && sized.time == native_order::SizeTime::AtAcceptance
                && sized.price == native_order::SizePrice::SignalOnTick
                && sized.grid_policy == native_order::ExecutionGridPolicy::ExplicitUnits
                && sized.reserve_percent_fee && trail.arm_price == warmup.high
                && trail.ticks && trail.ticks->ticks == 2.0 && trail.best_seed == warmup.close
                && openings.cycle == 1 && openings.openings.size() == 2
                && openings.openings.front().run == context.run
                && std::get<native_order::PointBudget>(request.capacity).units == warmup.volume
                && std::get<native_order::Member>(request.group).group == owner.scope()
                && std::get<native_order::FromOwnerFill>(request.anchor).ticks,
                "request provider preserves nested variants, optional values and fixed owners");
        native_order::Request pending_request;
        source::PlacementSnapshot pending_placement;
        std::string pending_key;
        std::as_const(versions).pending(
            pending_version, pending_request, pending_placement, pending_key);
        require(pending_key == replacement_key && pending_request.label == label
                && pending_placement.source_id == label
                && pending_placement.legs.retired(exit_legs::Leg::Stop),
                "pending provider round trips the request, placement and replacement key together");
        require(context.store.stats().resident_bytes == resident,
                "wide retained versions do not grow the store's resident workspace");
    }
    require(context.store.record_count() == before && !context.store.transaction_active(),
            "scope teardown releases payloads and stored version counters");
    const auto& openings = std::get<native_order::BindOpenings>(frozen);
    require(openings.cycle == 1 && openings.openings.size() == 2
            && openings.openings.front().run == context.run
            && openings.openings.front().incarnation != openings.openings.back().incarnation,
            "frozen native membership survives destruction of its scratch scope");
    require(host.stream_end(), "version test ends its command-scratch run");
}
} // namespace

int main() {
    try {
        run_setup_binds_versions_and_round_trips_scoped_payloads();
        run_owner_reversal_without_adapter_starts_each_run_empty();
        stream_setup_binds_readers_before_input();
        run_setup_binds_exit_scratch_before_global_exit();
    } catch (const std::exception& error) {
        std::fprintf(stderr, "test_run_setup_exit_scratch_binding: FAIL: %s\n", error.what());
        return 1;
    }
    std::puts("test_run_setup_exit_scratch_binding: OK");
    return 0;
}
