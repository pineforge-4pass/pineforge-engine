// Match notifications follow the submitted requests and fill schedule.
// Expected counts are derived from the inputs.
#include <pineforge/native_host.hpp>
#include "../src/native_execution_consumer.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <optional>
#include <variant>
#include <vector>

namespace {
using namespace pineforge;
namespace no = pineforge::native_order;
using Consumer = NativeExecutionConsumer;

template<class Tag, auto Member> struct Access {
    friend auto access(Tag) { return Member; }
};
struct Provenance { friend auto access(Provenance); };
struct Notifications { friend auto access(Notifications); };
struct NotificationHead { friend auto access(NotificationHead); };
struct Drivers { friend auto access(Drivers); };
template struct Access<Provenance, &Consumer::match_provenance_>;
template struct Access<Notifications, &Consumer::applied_notifications_>;
template struct Access<NotificationHead, &Consumer::notification_head_>;
template struct Access<Drivers, &Consumer::driver_log_>;

int checks = 0;
int failures = 0;
constexpr const char* kProvenance = "nativeexecutionconsumer_match_provenance";
constexpr const char* kNotifications = "nativeexecutionconsumer_applied_notifications";
constexpr const char* kDrivers = "nativeexecutionconsumer_driver_log";

// No failed check interrupts a callback, a scenario, or the next storage check.
#define MEM_CHECK(row, expression) do {                                    \
    ++checks;                                                            \
    if (!(expression)) {                                                 \
        ++failures;                                                      \
        std::fprintf(stderr, "FAIL [%s] %s:%d: %s\n",                     \
                     row, __FILE__, __LINE__, #expression);               \
    }                                                                    \
} while (0)

constexpr int kBars = 16;
constexpr std::int64_t kStart = 1736121600000LL;

NativeRunSpec configuration(const char* key) {
    NativeRunSpec spec;
    spec.identity = {key, 1};
    spec.input_tf = "1";
    spec.script_tf = "1";
    spec.tickerid = "TEST:FILLS";
    spec.timezone = "UTC";
    spec.session = "24x7";
    spec.initial_capital = 1000000;
    spec.point_value = 1;
    spec.account_fx = 1;
    spec.price_tick = 0.01;
    spec.fee_kind = NativeFeeKind::CashPerExecution;
    spec.fee_value = 0;
    spec.close_execution = NativeCloseExecution::NextEligiblePoint;
    return spec; // Window is deliberately left at its production default.
}

std::vector<Bar> tape(int count) {
    std::vector<Bar> result;
    result.reserve(static_cast<std::size_t>(count));
    for (int i = 0; i < count; ++i)
        result.push_back({100, 103, 99, 101, 1, kStart + i * 60000LL});
    return result;
}

struct ProbeHost : NativeStrategyHost {
    Consumer& consumer() { return as_native_consumer(execution_consumer()); }
    const Consumer& consumer() const { return as_native_consumer(execution_consumer()); }
};

struct Storage {
    std::size_t size = 0;
    std::size_t capacity = 0;
    std::size_t peak = 0;
};

// One ordinary market request at a time, alternating +1 and -1 units.
// Each fills at the NEXT bar's open. No request is issued on the final bar.
// Thus B bars produce B-1 fills, floor((B-1)/2) closed trades, and no final
// working request. During one match there is one candidate, hence one
// provenance row; after its request retires there are zero valid rows.
struct MatchedHost final : ProbeHost {
    explicit MatchedHost(int count, bool provenance)
        : total_bars(count), watch_provenance(provenance) {}
    int total_bars;
    bool watch_provenance;
    bool watch_drivers = false;
    NativeEventRetention expected_retention = NativeEventRetention::Window;
    int bars_seen = 0;
    int fills = 0;
    std::size_t provenance_peak = 0;
    std::optional<no::RequestHandle> outstanding;

    void on_native_run_begin() override {
        bars_seen = 0;
        fills = 0;
        provenance_peak = 0;
        outstanding.reset();
        if (watch_drivers && expected_retention != NativeEventRetention::Full) {
            const auto& rows = consumer().*access(Drivers{});
            // A prior Full run may have allocated a log. Begin must return
            // that storage before a non-Full run starts.
            MEM_CHECK(kDrivers, rows.size() == 0);
            MEM_CHECK(kDrivers, rows.capacity() == 0);
        }
    }

    void on_native_bar(const Bar&, const NativeDecisionContext&) override {
        ++bars_seen;
        const char* row = watch_provenance ? kProvenance : kDrivers;
        if (watch_provenance) {
            const auto& rows = consumer().*access(Provenance{});
            MEM_CHECK(kProvenance, rows.size() == 0);
        }
        if (watch_drivers && expected_retention != NativeEventRetention::Full) {
            const auto& rows = consumer().*access(Drivers{});
            MEM_CHECK(kDrivers, rows.size() == 0 && rows.capacity() == 0);
        }
        MEM_CHECK(row, !outstanding.has_value());
        if (bars_seen < total_bars) {
            const double units = bars_seen % 2 == 1 ? 1.0 : -1.0;
            const auto accepted = submit(no::Request{no::Transact{units}, "rolling", ""});
            MEM_CHECK(row, accepted.status == no::SubmitStatus::Accepted);
            MEM_CHECK(row, accepted.handle.has_value());
            outstanding = accepted.handle;
        }
    }

    void on_native_applied(const no::ExecutionAppliedEvent& event,
                           const NativeDecisionContext&) override {
        ++fills;
        const char* row = watch_provenance ? kProvenance : kDrivers;
        MEM_CHECK(row, outstanding.has_value() && *outstanding == event.handle());
        outstanding.reset();
        if (watch_provenance) {
            const auto& rows = consumer().*access(Provenance{});
            provenance_peak = std::max(provenance_peak, rows.size());
            MEM_CHECK(kProvenance, rows.size() == 1);
            if (!rows.empty())
                MEM_CHECK(kProvenance, rows.front().handle == event.handle());
        }
    }
};

Storage provenance_run(int count) {
    MatchedHost host(count, true);
    MEM_CHECK(kProvenance, host.configure_native(configuration(kProvenance)).status
                         == NativeSetupStatus::Applied);
    const auto bars = tape(count);
    host.run(bars.data(), count);
    MEM_CHECK(kProvenance, host.last_error().empty());
    MEM_CHECK(kProvenance, host.bars_seen == count && host.fills == count - 1);
    MEM_CHECK(kProvenance, host.trade_count() == (count - 1) / 2);
    MEM_CHECK(kProvenance, host.native_working_requests().size() == 0);
    const auto& rows = host.consumer().*access(Provenance{});
    MEM_CHECK(kProvenance, rows.size() == 0);
    MEM_CHECK(kProvenance, host.provenance_peak == 1);
    return {rows.size(), rows.capacity(), host.provenance_peak};
}

void provenance_retirement() {
    const auto small = provenance_run(kBars);
    const auto large = provenance_run(4 * kBars);
    MEM_CHECK(kProvenance, small.size == large.size && large.size == 0);
    MEM_CHECK(kProvenance, small.peak == large.peak && large.peak == 1);
    // No guessed std::vector growth factor: the same one-row peak must
    // leave the same capacity, regardless of the number of fills.
    MEM_CHECK(kProvenance, small.capacity == large.capacity);
}

// Each calculation executes two one-unit openings and one whole-book close
// synchronously. Notifications cannot drain inside that calculation.
// Independently: 1 + 1 + 1 = 3 applied fills, and 1 + 1 = 2 closed trades.
// The expected FIFO is built from execute_current's returned events, not
// from the queue under test.
struct NotificationHost final : ProbeHost {
    std::array<std::uint64_t, 3> expected_ordinals{};
    int bars_seen = 0;
    int delivered = 0;
    std::size_t delivered_this_bar = 0;
    std::size_t queue_peak = 0;

    void on_native_bar(const Bar&, const NativeDecisionContext&) override {
        ++bars_seen;
        auto& core = consumer();
        const auto& queue = core.*access(Notifications{});
        MEM_CHECK(kNotifications, queue.size() == 0);
        MEM_CHECK(kNotifications, (core.*access(NotificationHead{})) == 0);
        delivered_this_bar = 0;
        for (std::size_t slot = 0; slot < expected_ordinals.size(); ++slot) {
            const no::Request request = slot < 2
                ? no::Request{no::Transact{1.0}, "open", ""}
                : no::Request{no::Flatten{}, "close", ""};
            const auto accepted = submit(request);
            MEM_CHECK(kNotifications, accepted.status == no::SubmitStatus::Accepted);
            MEM_CHECK(kNotifications, accepted.handle.has_value());
            if (accepted.handle) {
                const auto result = execute_current(
                    {*accepted.handle, NativeCurrentPriceRule::AsPresented});
                const auto* applied = std::get_if<no::ExecutionAppliedEvent>(&result);
                MEM_CHECK(kNotifications, applied != nullptr);
                if (applied) expected_ordinals[slot] = applied->ordinal;
            }
            // Observe the actual insert path at EACH append, not just a
            // callback counter that would miss an accumulating queue.
            MEM_CHECK(kNotifications, queue.size() == slot + 1);
            MEM_CHECK(kNotifications, delivered_this_bar == 0);
        }
        queue_peak = std::max(queue_peak, queue.size());
        MEM_CHECK(kNotifications, queue.size() == expected_ordinals.size());
        MEM_CHECK(kNotifications, physical_position().signed_units == 0);
    }

    void on_native_applied(const no::ExecutionAppliedEvent& event,
                           const NativeDecisionContext&) override {
        const auto& core = consumer();
        const auto& queue = core.*access(Notifications{});
        MEM_CHECK(kNotifications, queue.size() == expected_ordinals.size());
        MEM_CHECK(kNotifications, delivered_this_bar < expected_ordinals.size());
        if (delivered_this_bar < expected_ordinals.size())
            MEM_CHECK(kNotifications, event.ordinal == expected_ordinals[delivered_this_bar]);
        // All three rows remain until the drain finishes. Even while a row
        // is being delivered the vector must not contain a prior bar's tail.
        for (std::size_t i = 0; i < queue.size() && i < expected_ordinals.size(); ++i)
            MEM_CHECK(kNotifications, queue[i].ordinal == expected_ordinals[i]);
        ++delivered_this_bar;
        ++delivered;
        MEM_CHECK(kNotifications, (core.*access(NotificationHead{})) == delivered_this_bar);
    }
};

Storage notification_run(int count) {
    NotificationHost host;
    MEM_CHECK(kNotifications, host.configure_native(configuration(kNotifications)).status
                            == NativeSetupStatus::Applied);
    const auto bars = tape(count);
    host.run(bars.data(), count);
    MEM_CHECK(kNotifications, host.last_error().empty());
    MEM_CHECK(kNotifications, host.bars_seen == count && host.delivered == 3 * count);
    MEM_CHECK(kNotifications, host.trade_count() == 2 * count);
    MEM_CHECK(kNotifications, host.native_working_requests().size() == 0);
    const auto& core = host.consumer();
    const auto& queue = core.*access(Notifications{});
    MEM_CHECK(kNotifications, queue.size() == 0);
    MEM_CHECK(kNotifications, (core.*access(NotificationHead{})) == 0);
    MEM_CHECK(kNotifications, host.queue_peak == 3);
    return {queue.size(), queue.capacity(), host.queue_peak};
}

void notification_retirement() {
    const auto small = notification_run(kBars);
    const auto large = notification_run(4 * kBars);
    MEM_CHECK(kNotifications, small.size == large.size && large.size == 0);
    MEM_CHECK(kNotifications, small.peak == large.peak && large.peak == 3);
    MEM_CHECK(kNotifications, small.capacity == large.capacity);
}

Storage driver_run(MatchedHost& host, NativeEventRetention retention,
                   std::uint64_t run_number) {
    // NativeStrategyHost::run refuses a Completed host with
    // "native run requires configure_native" and completes each successful
    // batch. configure_native returns EACH run to Ready, keeping the same
    // consumer and session key. Reuse requires a strictly larger run number,
    // so the caller counts 1,2,3,4
    // independently of the engine's observed high-water.
    auto spec = configuration(kDrivers);
    spec.identity.run_number = run_number;
    MEM_CHECK(kDrivers, host.configure_native(spec).status == NativeSetupStatus::Applied);
    host.expected_retention = retention;
    host.consumer().set_retention_override(retention);
    const auto bars = tape(host.total_bars);
    host.run(bars.data(), host.total_bars);
    if (!host.last_error().empty())
        std::fprintf(stderr, "[%s] run %llu: %s\n", kDrivers,
                     static_cast<unsigned long long>(run_number),
                     host.last_error().c_str());
    MEM_CHECK(kDrivers, host.last_error().empty());
    // NativeExecutionConsumer::begin_ready consumes this generation.
    // Refused replays leave the old
    // counters/log intact; require evidence that THIS begin actually ran.
    MEM_CHECK(kDrivers, host.native_consumed_high_water() == run_number);
    MEM_CHECK(kDrivers, host.bars_seen == host.total_bars);
    MEM_CHECK(kDrivers, host.fills == host.total_bars - 1);
    MEM_CHECK(kDrivers, host.trade_count() == (host.total_bars - 1) / 2);
    const auto& rows = host.consumer().*access(Drivers{});
    // No intrabar source, current execution or after-calculation point:
    // exactly Open, first extremum, second extremum, Close = 4 per bar.
    // The calculation itself is not recorded as a driver point.
    const std::size_t expected = retention == NativeEventRetention::Full
        ? 4 * static_cast<std::size_t>(host.total_bars) : 0;
    MEM_CHECK(kDrivers, rows.size() == expected);
    if (retention != NativeEventRetention::Full)
        MEM_CHECK(kDrivers, rows.capacity() == 0);
    std::size_t exposed = 0;
    for (const auto& event : host.native_events(0))
        if (event.driver) ++exposed;
    MEM_CHECK(kDrivers, exposed == expected);
    return {rows.size(), rows.capacity(), 0};
}

void driver_retention() {
    std::array<Storage, 2> window{}, commands{}, full{};
    const std::array<int, 2> lengths{{kBars, 4 * kBars}};
    for (std::size_t i = 0; i < lengths.size(); ++i) {
        MatchedHost host(lengths[i], false);
        host.watch_drivers = true;
        std::uint64_t next_run = 1;
        // Full really inserts into this member; reuse the SAME consumer so
        // the following run must relinquish that allocation at begin.
        full[i] = driver_run(host, NativeEventRetention::Full, next_run++);
        window[i] = driver_run(host, NativeEventRetention::Window, next_run++);
        (void)driver_run(host, NativeEventRetention::Full, next_run++);
        commands[i] = driver_run(host, NativeEventRetention::Commands, next_run++);
    }
    MEM_CHECK(kDrivers, window[0].size == window[1].size && window[1].size == 0);
    MEM_CHECK(kDrivers, window[0].capacity == window[1].capacity && window[1].capacity == 0);
    MEM_CHECK(kDrivers, commands[0].size == commands[1].size && commands[1].size == 0);
    MEM_CHECK(kDrivers, commands[0].capacity == commands[1].capacity && commands[1].capacity == 0);
    // Full is requested history, explicitly O(bars); never claim it is flat.
    MEM_CHECK(kDrivers, full[0].size == 4 * kBars && full[1].size == 4 * full[0].size);
}
} // namespace

int main() {
    struct Row { const char* id; void (*run)(); };
    const Row rows[] = {
        {kProvenance, provenance_retirement},
        {kNotifications, notification_retirement},
        {kDrivers, driver_retention},
    };
    for (const auto& row : rows) {
        std::printf("match notifications [%s]\n", row.id);
        try {
            row.run();
        } catch (const std::exception& error) {
            MEM_CHECK(row.id, false);
            std::fprintf(stderr, "[%s] exception: %s\n", row.id, error.what());
        } catch (...) {
            MEM_CHECK(row.id, false);
            std::fprintf(stderr, "[%s] unknown exception\n", row.id);
        }
    }
    std::printf("memory proof match_notifications: %d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
