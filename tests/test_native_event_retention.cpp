// R5 lane V19-B: what a run keeps of its event record (NativeRunSpec::
// event_retention) and how a host that polls it says what it has read
// (NativeStrategyHost::native_acknowledge_events). Source-free, so it also
// runs in the kernel-only profile.
//
//   1. the spec: Window is the default and folds nothing into the spec
//      digest -- the digest 6211dc94 computed for the same spec -- while Full
//      and Commands each fold; a word outside the enumeration is refused as
//      UnknownEventRetention at EventRetention.
//   2. the readbacks, over three randomized books (tests/native_match_book_
//      fixture.hpp: every intent and trigger, OCA groups, brackets, cohorts,
//      replaces, an intrabar path, fill recalculation, the price grid):
//      Full is 6211dc94's record row for row (pinned); Commands is Full's
//      command and account rows and no driver row; Window keeps no driver or
//      account row, a host that never acknowledges holds at every script bar
//      exactly the commands since the last boundary, and one that polls reads
//      every command exactly once and is left exactly the unacknowledged
//      suffix; a read from below the window starts at it.
//   3. the acknowledgement: before begin and after the end it records
//      nothing, one above the high water acknowledges the high water, a
//      second smaller one lowers nothing, and Full / Commands keep everything
//      whatever is acknowledged.
//   4. the stress switch (set_retire_every_point): the same books retiring
//      at every driver point book the trades Full books and fold the
//      continuation the per-bar window folds -- no kernel read touches a
//      retired event.
//   5. state the journal used to answer: a trail's TrailArm ordinal and a
//      margin call's receipt, under an FX curve, after the window retired
//      their events.
#include <pineforge/native_host.hpp>

#include "../src/native_execution_consumer.hpp"
#include "native_event_rows_fixture.hpp"
#include "native_match_book_fixture.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <limits>
#include <optional>
#include <string>
#include <vector>

using namespace pineforge;
namespace no = pineforge::native_order;

namespace {
int checks = 0;
int failures = 0;
#define CHECK(x)                                                        \
    do {                                                                \
        ++checks;                                                       \
        if (!(x)) {                                                     \
            ++failures;                                                 \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #x);    \
        }                                                               \
    } while (0)

NativeRunSpec configuration(const char* key, uint64_t run = 1) {
    NativeRunSpec s;
    s.identity = {key, run};
    s.input_tf = "1";
    s.script_tf = "1";
    s.ticker = "N";
    s.tickerid = "TEST:N";
    s.type = "crypto";
    s.currency = "USD";
    s.basecurrency = "USD";
    s.description = "V19-B retention";
    s.volumetype = "base";
    s.timezone = "UTC";
    s.session = "24x7";
    s.initial_capital = 10000;
    s.point_value = 1;
    s.account_fx = 1;
    s.price_tick = .01;
    s.fee_kind = NativeFeeKind::CashPerExecution;
    s.fee_value = 0;
    return s;
}

// native_run_spec_digest(configuration("event-retention")) on 6211dc94,
// before the field existed (V19-B-scratch/failbefore/retention_spec_digest_base.cpp).
constexpr std::uint64_t kDigestBeforeTheField = 12094809940376900009ULL;

struct IdleHost final : NativeStrategyHost {
    void on_native_bar(const Bar&, const NativeDecisionContext&) override {}
};

// ── 1. the spec ───────────────────────────────────────────────────────────
void the_spec_names_a_retention() {
    NativeRunSpec window = configuration("event-retention");
    CHECK(window.event_retention == NativeEventRetention::Window);
    CHECK(native_run_spec_digest(window) == kDigestBeforeTheField);
    NativeRunSpec full = window;
    full.event_retention = NativeEventRetention::Full;
    NativeRunSpec commands = window;
    commands.event_retention = NativeEventRetention::Commands;
    CHECK(validate_native_run_spec(full).ok());
    CHECK(validate_native_run_spec(commands).ok());
    CHECK(native_run_spec_digest(full) != kDigestBeforeTheField);
    CHECK(native_run_spec_digest(commands) != kDigestBeforeTheField);
    CHECK(native_run_spec_digest(full) != native_run_spec_digest(commands));

    NativeRunSpec unknown = window;
    unknown.event_retention = static_cast<NativeEventRetention>(3);
    const auto refused = validate_native_run_spec(unknown);
    CHECK(refused.error == NativeRunSpecError::UnknownEventRetention);
    CHECK(refused.field == NativeRunSpecField::EventRetention);
    IdleHost host;
    const auto setup = host.configure_native(unknown);
    CHECK(setup.status == NativeSetupStatus::Failed);
    CHECK(setup.validation.error == NativeRunSpecError::UnknownEventRetention);
    CHECK(setup.validation.field == NativeRunSpecField::EventRetention);
    IdleHost fresh;
    CHECK(fresh.configure_native(full).status == NativeSetupStatus::Applied);
}

// ── 2. the readbacks over randomized books ────────────────────────────────
k3_book::BookConfig book(int index) {
    k3_book::BookConfig c;
    switch (index) {
    case 0:
        c.seed = 101; c.live = 5; c.bars = 60; c.path = k3_book::Path::Lower;
        break;
    case 1:
        c.seed = 303; c.live = 40; c.bars = 40; c.calc_on_fills = true;
        break;
    default:
        c.seed = 404; c.live = 20; c.bars = 40; c.path = k3_book::Path::Synthesized;
        c.quantize = true;
        break;
    }
    return c;
}
constexpr int kBooks = 3;

// Each book's whole record under Full, harvested on 6211dc94 -- before the
// window, when every run kept everything -- by the same fold
// (V19-B-scratch/failbefore/event_rows_harvest.cpp, event_rows_harvest_base.txt).
const event_rows::Record kFullRecord[kBooks] = {
    {889u, 564u, 240u, 85u, 0x1f9ed57a1e3b3980ull},
    {7928u, 6910u, 160u, 858u, 0x6dcae2e18cbfc033ull},
    {1162u, 833u, 160u, 169u, 0x53aa0ab250ce515eull},
};

enum class Reader { None, Poll };

// K-IDX follow-up: re-pins v19 event-record digests after the coordinate fold.
// The book, a chosen retention, and optionally a reader that polls
// native_events() above its own cursor at every script bar, acknowledging
// what it read until bar `acknowledge_until`.
class RetentionBook final : public k3_book::BookHost {
public:
    RetentionBook(const k3_book::BookConfig& config, Reader reader, int acknowledge_until)
        : BookHost(config), reader_(reader), acknowledge_until_(acknowledge_until) {}

    struct Snapshot {
        std::uint64_t start = 0;
        std::vector<pineforge::NativeMarketEvent> rows;
    };
    std::vector<Snapshot> snapshots;       // native_events(0) at every script bar
    std::vector<pineforge::NativeMarketEvent> polled;  // every row the reader read
    std::uint64_t cursor = 0;
    std::uint64_t acknowledged = 0;

    void retire_every_point() {
        as_native_consumer(execution_consumer()).set_retire_every_point(true);
    }
    void on_native_run_begin() override {
        BookHost::on_native_run_begin();
        if (reader_ == Reader::Poll) native_acknowledge_events(0);
    }
    void on_native_bar(const Bar& bar, const NativeDecisionContext& context) override {
        BookHost::on_native_bar(bar, context);
        ++bars_;
        snapshots.push_back(Snapshot{native_event_window_start(), native_events(0)});
        if (reader_ != Reader::Poll) return;
        for (const auto& row : native_events(cursor)) {
            polled.push_back(row);
            cursor = std::max(cursor, row.ordinal);
        }
        if (bars_ <= acknowledge_until_) {
            native_acknowledge_events(cursor);
            acknowledged = cursor;
        }
    }

private:
    Reader reader_;
    int acknowledge_until_;
    int bars_ = 0;
};

struct BookRun {
    k3_book::Outcome outcome;
    std::vector<pineforge::NativeMarketEvent> rows;  // native_events(0) after the run
    std::vector<pineforge::NativeMarketEvent> below; // native_events(1)
    std::vector<pineforge::NativeMarketEvent> at_start;  // native_events(window_start - 1)
    std::uint64_t window_start = 0;
    std::vector<RetentionBook::Snapshot> snapshots;
    std::vector<pineforge::NativeMarketEvent> polled;
    std::uint64_t acknowledged = 0;
};

BookRun run_book(const k3_book::BookConfig& config, NativeEventRetention retention,
                 bool stress = false, Reader reader = Reader::None,
                 int acknowledge_until = 0) {
    RetentionBook host(config, reader, acknowledge_until);
    if (stress) host.retire_every_point();
    const k3_book::Tape tape = k3_book::make_tape(config);
    NativeRunSpec spec = k3_book::make_spec(config, tape);
    spec.event_retention = retention;
    BookRun out;
    if (host.configure_native(spec).status != NativeSetupStatus::Applied) {
        CHECK(!"the book's spec was refused");
        return out;
    }
    host.run(tape.bars.data(), static_cast<int>(tape.bars.size()));
    host.finish();
    out.outcome = host.outcome;
    out.rows = host.native_events(0);
    out.below = host.native_events(1);
    out.window_start = host.native_event_window_start();
    out.at_start = host.native_events(out.window_start - 1);
    out.snapshots = std::move(host.snapshots);
    out.polled = std::move(host.polled);
    out.acknowledged = host.acknowledged;
    return out;
}

std::vector<std::uint64_t> values(const std::vector<pineforge::NativeMarketEvent>& rows) {
    std::vector<std::uint64_t> out;
    out.reserve(rows.size());
    for (const auto& row : rows) out.push_back(event_rows::row_value(row));
    return out;
}

template <class Keep>
std::vector<std::uint64_t> values_where(const std::vector<pineforge::NativeMarketEvent>& rows,
                                        Keep keep) {
    std::vector<std::uint64_t> out;
    for (const auto& row : rows) {
        if (keep(row)) out.push_back(event_rows::row_value(row));
    }
    return out;
}

bool same_trades(const k3_book::Outcome& a, const k3_book::Outcome& b) {
    return a.completed && b.completed && a.error == b.error && a.trades == b.trades
        && a.trades_digest == b.trades_digest && a.position == b.position
        && a.accepted == b.accepted && a.rejected == b.rejected && a.replaced == b.replaced
        && a.cancelled == b.cancelled && a.applied == b.applied;
}

struct BookRuns {
    BookRun full, commands, window, polled;
};

BookRuns the_readbacks(int index) {
    const auto config = book(index);
    BookRuns runs;
    runs.full = run_book(config, NativeEventRetention::Full);
    runs.commands = run_book(config, NativeEventRetention::Commands);
    runs.window = run_book(config, NativeEventRetention::Window);
    runs.polled = run_book(config, NativeEventRetention::Window, false, Reader::Poll,
                           config.bars / 2);
    const auto& full = runs.full;

    // Full: 6211dc94's record, row for row; nothing is ever retired.
    const auto record = event_rows::record(full.rows);
    const auto& pin = kFullRecord[index];
    CHECK(record.rows == pin.rows && record.commands == pin.commands
          && record.drivers == pin.drivers && record.accounts == pin.accounts);
    CHECK(record.digest == pin.digest);
    CHECK(full.window_start == 1);
    const auto is_command = [](const pineforge::NativeMarketEvent& row) { return row.command.has_value(); };
    const auto is_account = [](const pineforge::NativeMarketEvent& row) { return row.account.has_value(); };
    const auto full_commands = values_where(full.rows, is_command);

    // Commands: the command journal and the account rows, whole; no driver row.
    const auto commands = event_rows::record(runs.commands.rows);
    CHECK(commands.drivers == 0);
    CHECK(values_where(runs.commands.rows, is_command) == full_commands);
    CHECK(values_where(runs.commands.rows, is_account) == values_where(full.rows, is_account));
    CHECK(runs.commands.window_start == 1);

    // Every retention books the same run.
    CHECK(same_trades(full.outcome, runs.commands.outcome));
    CHECK(same_trades(full.outcome, runs.window.outcome));
    CHECK(same_trades(full.outcome, runs.polled.outcome));

    // Window, served by callbacks alone: no driver or account row ever; at
    // every script bar the window is exactly the commands since the last
    // boundary -- a contiguous run of Full's journal from the window start --
    // and after the run the same holds of what the last bar left.
    const auto full_commands_from = [&](std::uint64_t start) {
        return values_where(full.rows, [&](const pineforge::NativeMarketEvent& row) {
            return row.command && row.ordinal >= start;
        });
    };
    std::size_t retired_bars = 0;
    for (const auto& snapshot : runs.window.snapshots) {
        const auto tail = full_commands_from(snapshot.start);
        const auto held = values(snapshot.rows);
        bool commands_only = true;
        for (const auto& row : snapshot.rows) {
            commands_only = commands_only && row.command && row.ordinal >= snapshot.start;
        }
        CHECK(commands_only);
        CHECK(held.size() <= tail.size()
              && std::equal(held.begin(), held.end(), tail.begin()));
        if (snapshot.start > 1) ++retired_bars;
    }
    CHECK(retired_bars + 1 >= runs.window.snapshots.size());
    const auto window = event_rows::record(runs.window.rows);
    CHECK(window.drivers == 0 && window.accounts == 0);
    CHECK(runs.window.window_start > 1);
    CHECK(values(runs.window.rows) == full_commands_from(runs.window.window_start));
    // A read from below the window starts at it.
    CHECK(values(runs.window.below) == values(runs.window.rows));
    CHECK(values(runs.window.at_start) == values(runs.window.rows));

    // Window, a polling host: it read every command exactly once, in order,
    // and after it stopped acknowledging it is left exactly the suffix it did
    // not acknowledge.
    const auto& polled = runs.polled;
    const auto polled_values = values(polled.polled);
    CHECK(!polled_values.empty());
    CHECK(polled_values.size() <= full_commands.size()
          && std::equal(polled_values.begin(), polled_values.end(), full_commands.begin()));
    CHECK(polled.acknowledged > 0);
    const auto unacknowledged = values_where(full.rows, [&](const pineforge::NativeMarketEvent& row) {
        return row.command && row.ordinal > polled.acknowledged;
    });
    CHECK(values(polled.rows) == unacknowledged);
    CHECK(!unacknowledged.empty());
    CHECK(polled.window_start > 1 && polled.window_start <= polled.acknowledged + 1);
    CHECK(values(polled.below) == values(polled.rows));
    std::printf("  book %d: Full %llu rows (%llu commands, %llu driver, %llu account) = 6211dc94; "
                "Window kept %zu, polled %zu, left %zu unacknowledged, window start %llu\n",
                index, static_cast<unsigned long long>(record.rows),
                static_cast<unsigned long long>(record.commands),
                static_cast<unsigned long long>(record.drivers),
                static_cast<unsigned long long>(record.accounts), runs.window.rows.size(),
                polled.polled.size(), polled.rows.size(),
                static_cast<unsigned long long>(runs.window.window_start));
    return runs;
}

// ── 3. the acknowledgement's contract ─────────────────────────────────────
// A market order every bar, alternating sides, so every script bar commits.
struct AckHost final : NativeStrategyHost {
    std::function<void(AckHost&, int)> at_bar;
    bool poll = false;  // acknowledges "nothing read yet" at its run begin
    int bars = 0;
    void on_native_run_begin() override {
        if (poll) native_acknowledge_events(0);
    }
    void on_native_bar(const Bar&, const NativeDecisionContext&) override {
        ++bars;
        (void)submit(no::Request{no::Transact{bars % 2 ? 1.0 : -1.0}, "m", ""});
        if (at_bar) at_bar(*this, bars);
    }
    std::uint64_t last_command_seen() const {
        std::uint64_t last = 0;
        for (const auto& row : native_events(0)) {
            if (row.command) last = std::max(last, row.ordinal);
        }
        return last;
    }
};

std::vector<Bar> tape(int n) {
    std::vector<Bar> bars;
    for (int i = 0; i < n; ++i) {
        const double p = 100.0 + (i % 5);
        bars.push_back(Bar{p, p + 1, p - 1, p + 0.5, 1.0, 1736121600000LL + i * 60000LL});
    }
    return bars;
}

struct AckRun {
    std::vector<pineforge::NativeMarketEvent> rows;
    std::uint64_t window_start = 0;
    std::uint64_t mark = 0;  // what the scenario recorded
    int trades = 0;
};

AckRun ack_run(NativeEventRetention retention,
               const std::function<void(AckHost&)>& before,
               const std::function<void(AckHost&, int, std::uint64_t&)>& at_bar,
               const std::function<void(AckHost&)>& after = {}) {
    AckHost host;
    AckRun out;
    NativeRunSpec spec = configuration("event-retention-ack");
    spec.event_retention = retention;
    CHECK(host.configure_native(spec).status == NativeSetupStatus::Applied);
    if (before) before(host);
    host.at_bar = [&](AckHost& h, int bar) {
        if (at_bar) at_bar(h, bar, out.mark);
    };
    const auto bars = tape(12);
    host.run(bars.data(), static_cast<int>(bars.size()));
    CHECK(host.last_error().empty());
    if (after) after(host);
    out.rows = host.native_events(0);
    out.window_start = host.native_event_window_start();
    out.trades = host.trade_count();
    return out;
}

void the_acknowledgement_contract() {
    const auto full = ack_run(NativeEventRetention::Full, {}, {});
    const auto full_commands_after = [&](std::uint64_t after) {
        return values_where(full.rows, [&](const pineforge::NativeMarketEvent& row) {
            return row.command && row.ordinal > after;
        });
    };
    const auto never = ack_run(NativeEventRetention::Window, {}, {});
    CHECK(never.window_start > 1);
    CHECK(values(never.rows) == full_commands_after(never.window_start - 1));
    CHECK(never.trades == full.trades);

    // Before begin: recorded nowhere. The run is the one a host that never
    // acknowledged makes, window for window.
    const auto early = ack_run(NativeEventRetention::Window,
                               [](AckHost& h) { h.native_acknowledge_events(3); }, {});
    CHECK(early.window_start == never.window_start);
    CHECK(values(early.rows) == values(never.rows));

    // Above the high water: the high water is acknowledged, never an event
    // that has not happened. The host polls from its begin, acknowledges
    // "everything" once, at bar 4, and never again: every command after the
    // ones it could see then is kept to the end.
    const auto beyond = ack_run(
        NativeEventRetention::Window, [](AckHost& h) { h.poll = true; },
        [](AckHost& h, int bar, std::uint64_t& mark) {
            if (bar != 4) return;
            mark = h.last_command_seen();
            h.native_acknowledge_events(std::numeric_limits<std::uint64_t>::max());
        });
    CHECK(beyond.mark > 0);
    CHECK(values(beyond.rows) == full_commands_after(beyond.mark));
    CHECK(beyond.window_start <= beyond.mark + 1);

    // Twice, the second smaller: nothing is lowered.
    const auto twice = ack_run(
        NativeEventRetention::Window, [](AckHost& h) { h.poll = true; },
        [](AckHost& h, int bar, std::uint64_t& mark) {
            if (bar != 4) return;
            mark = h.last_command_seen();
            h.native_acknowledge_events(mark);
            h.native_acknowledge_events(1);
        });
    CHECK(values(twice.rows) == full_commands_after(twice.mark));
    CHECK(values(twice.rows) == values(beyond.rows));

    // A host that polls from its run begin and reads nothing keeps
    // everything; after the end an acknowledgement moves nothing.
    const auto late = ack_run(
        NativeEventRetention::Window, [](AckHost& h) { h.poll = true; }, {},
        [](AckHost& h) { h.native_acknowledge_events(std::numeric_limits<std::uint64_t>::max()); });
    CHECK(late.window_start == 1);
    CHECK(values(late.rows) == full_commands_after(0));

    // Full and Commands keep the whole journal, whatever is acknowledged.
    for (const auto retention : {NativeEventRetention::Full, NativeEventRetention::Commands}) {
        const auto kept = ack_run(retention, [](AckHost& h) { h.poll = true; },
                                  [](AckHost& h, int, std::uint64_t&) {
                                      h.native_acknowledge_events(
                                          std::numeric_limits<std::uint64_t>::max());
                                  });
        CHECK(kept.window_start == 1);
        CHECK(values_where(kept.rows, [](const pineforge::NativeMarketEvent& row) {
                  return row.command.has_value();
              }) == full_commands_after(0));
    }
    std::printf("  acknowledgement: never -> start %llu, before begin -> start %llu, "
                "beyond the high water at bar 4 keeps %zu commands, after the end keeps %zu\n",
                static_cast<unsigned long long>(never.window_start),
                static_cast<unsigned long long>(early.window_start), beyond.rows.size(),
                late.rows.size());
}

// ── 4. the stress switch ──────────────────────────────────────────────────
void no_read_touches_a_retired_event(int index, const BookRuns& runs) {
    const auto config = book(index);
    const auto stressed = run_book(config, NativeEventRetention::Window, true);
    const auto stressed_polled = run_book(config, NativeEventRetention::Window, true, Reader::Poll,
                                          config.bars / 2);
    CHECK(same_trades(runs.full.outcome, stressed.outcome));
    CHECK(same_trades(runs.full.outcome, stressed_polled.outcome));
    // The continuation at every bar and every fill: the window's retirement
    // cadence is not state, so the per-point window folds what the per-bar one
    // does (and Full differs from both only by its folded spec).
    CHECK(stressed.outcome.trace == runs.window.outcome.trace);
    CHECK(stressed.outcome.broker == runs.window.outcome.broker);
    CHECK(stressed_polled.outcome.trace == runs.polled.outcome.trace);
    // A reader still reads every command exactly once under the stress.
    const auto polled_values = values(stressed_polled.polled);
    const auto full_commands = values_where(runs.full.rows, [](const pineforge::NativeMarketEvent& row) {
        return row.command.has_value();
    });
    CHECK(polled_values.size() <= full_commands.size()
          && std::equal(polled_values.begin(), polled_values.end(), full_commands.begin()));
    CHECK(values(stressed_polled.polled) == values(runs.polled.polled));
    std::printf("  book %d under the stress switch: %d trades, %ld fills, %zu continuation reads "
                "equal to the per-bar window\n", index, stressed.outcome.trades,
                stressed.outcome.applied, stressed.outcome.trace.size());
}

// ── 5. state the journal used to answer ───────────────────────────────────
// A long position, then a sell trail far behind it: it arms on bar 3 and
// rides for the rest of the run, long after the window retired its arm.
struct TrailHost final : NativeStrategyHost {
    std::optional<no::RequestHandle> trail;
    std::optional<NativeTrailState> late;
    int bars = 0;
    void on_native_bar(const Bar&, const NativeDecisionContext&) override {
        ++bars;
        if (bars == 1) (void)submit(no::Request{no::Transact{1.0}, "long", ""});
        if (bars == 2) {
            no::Request exit{no::Reduce{no::ExplicitUnits{1.0}}, "trail", ""};
            exit.trigger = no::Trail{50.0, 100.5};
            trail = submit(exit).handle;
        }
        if (bars == 10 && trail) late = trail_state(*trail);
    }
};

void a_trail_keeps_its_arm_ordinal() {
    std::uint64_t arm_event = 0;
    std::optional<NativeTrailState> states[2];
    std::size_t retained_arms[2] = {0, 0};
    const NativeEventRetention retentions[2] = {NativeEventRetention::Full,
                                                NativeEventRetention::Window};
    for (int i = 0; i < 2; ++i) {
        TrailHost host;
        NativeRunSpec spec = configuration("event-retention-trail");
        spec.event_retention = retentions[i];
        CHECK(host.configure_native(spec).status == NativeSetupStatus::Applied);
        const auto bars = tape(12);
        host.run(bars.data(), static_cast<int>(bars.size()));
        CHECK(host.last_error().empty());
        states[i] = host.late;
        for (const auto& row : host.native_events(0)) {
            const auto* activated = row.command
                ? std::get_if<no::ActivatedEvent>(&*row.command) : nullptr;
            if (!activated || activated->kind != no::ActivationKind::TrailArm) continue;
            ++retained_arms[i];
            if (i == 0) arm_event = activated->ordinal;
        }
    }
    CHECK(arm_event != 0);
    CHECK(retained_arms[0] == 1 && retained_arms[1] == 0);
    CHECK(states[0] && states[1]);
    if (states[0] && states[1]) {
        CHECK(states[0]->activated && states[1]->activated);
        CHECK(states[0]->activation_ordinal == arm_event);
        CHECK(states[1]->activation_ordinal == arm_event);
        CHECK(states[0]->best_price == states[1]->best_price);
        CHECK(states[0]->current_level == states[1]->current_level);
    }
    std::printf("  trail: arm %llu read back at bar 10 under Window, its event retired\n",
                static_cast<unsigned long long>(arm_event));
}

// A maintenance-only margin model under a declared FX curve: the position is
// liquidated after the rate steps, so the FX-roll check reads its previous
// driver point and the applied notification reads its margin receipt back.
struct MarginHost final : NativeStrategyHost {
    std::vector<no::MarginCallEvent> calls;
    int bars = 0;
    void retire_every_point() {
        as_native_consumer(execution_consumer()).set_retire_every_point(true);
    }
    void on_native_bar(const Bar&, const NativeDecisionContext&) override {
        if (++bars == 1) (void)submit(no::Request{no::Transact{18.0}, "long", ""});
    }
    void on_native_margin_call(const no::MarginCallEvent& event) override {
        calls.push_back(event);
    }
};

struct MarginRun {
    std::vector<no::MarginCallEvent> calls;
    std::vector<Trade> trades;
    double position = 0.0;
};

MarginRun margin_run(NativeEventRetention retention, bool stress) {
    MarginHost host;
    if (stress) host.retire_every_point();
    NativeRunSpec spec = configuration("event-retention-margin");
    spec.initial_capital = 1000.0;
    spec.event_retention = retention;
    NativeMarginModel model;
    model.initial_long = 0.0;
    model.initial_short = 0.0;
    model.maintenance_long = 0.5;
    model.maintenance_short = 0.5;
    model.sizing = NativeLiquidationSizing::RestoreMinimum;
    spec.margin = model;
    CHECK(host.configure_native(spec).status == NativeSetupStatus::Applied);
    const std::int64_t t0 = 1736121600000LL;
    const NativeFxCurve curve{{t0 + 4 * 60000LL, t0 + 7 * 60000LL}, {1.3, 1.6}};
    CHECK(host.configure_native_fx_curve(curve).status == NativeSetupStatus::Applied);
    std::vector<Bar> bars;
    for (int i = 0; i < 12; ++i) {
        const double p = 100.0 - 1.5 * i;
        bars.push_back(Bar{p, p + 0.5, p - 1.0, p - 0.5, 1.0, t0 + i * 60000LL});
    }
    host.run(bars.data(), static_cast<int>(bars.size()));
    CHECK(host.last_error().empty());
    MarginRun out;
    out.calls = host.calls;
    for (int i = 0; i < host.trade_count(); ++i) out.trades.push_back(host.get_trade(i));
    out.position = host.physical_position().signed_units;
    return out;
}

bool same_calls(const std::vector<no::MarginCallEvent>& a, const std::vector<no::MarginCallEvent>& b) {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (a[i].ordinal != b[i].ordinal || a[i].applied.ordinal != b[i].applied.ordinal
            || a[i].mark != b[i].mark || a[i].equity != b[i].equity
            || a[i].required != b[i].required || a[i].units != b[i].units
            || a[i].liquidation_price != b[i].liquidation_price) {
            return false;
        }
    }
    return true;
}

bool same_rows(const std::vector<Trade>& a, const std::vector<Trade>& b) {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (a[i].entry_time != b[i].entry_time || a[i].exit_time != b[i].exit_time
            || a[i].entry_price != b[i].entry_price || a[i].exit_price != b[i].exit_price
            || a[i].qty != b[i].qty || a[i].pnl != b[i].pnl) {
            return false;
        }
    }
    return true;
}

void a_margin_call_reads_its_receipt_back() {
    const auto full = margin_run(NativeEventRetention::Full, false);
    const auto window = margin_run(NativeEventRetention::Window, false);
    const auto stressed = margin_run(NativeEventRetention::Window, true);
    CHECK(!full.calls.empty());
    CHECK(same_calls(full.calls, window.calls));
    CHECK(same_calls(full.calls, stressed.calls));
    CHECK(same_rows(full.trades, window.trades));
    CHECK(same_rows(full.trades, stressed.trades));
    CHECK(full.position == window.position && full.position == stressed.position);
    std::printf("  margin: %zu margin calls and %zu closed rows under Full, Window and the stress\n",
                full.calls.size(), full.trades.size());
}
}  // namespace

int main() {
    the_spec_names_a_retention();
    for (int index = 0; index < kBooks; ++index) {
        const auto runs = the_readbacks(index);
        no_read_touches_a_retired_event(index, runs);
    }
    the_acknowledgement_contract();
    a_trail_keeps_its_arm_ordinal();
    a_margin_call_reads_its_receipt_back();
    if (failures != 0) {
        std::printf("test_native_event_retention: %d of %d checks failed\n", failures, checks);
        return 1;
    }
    std::printf("test_native_event_retention: ok (%d checks)\n", checks);
    return 0;
}
