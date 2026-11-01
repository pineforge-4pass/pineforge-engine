// Command-container populations follow the submitted requests.
// Counts below come from the submitted commands;
// they are source-derived expectations, not measurements.
// Function-local storage and an unestablished overflow path are described below.
#include <pineforge/native_toolkit.hpp>

#include "../src/native_execution_consumer.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace {
using namespace pineforge;
namespace no = pineforge::native_order;
namespace tk = pineforge::native_toolkit;
using Book = tk::OrderBook<std::string>;
using Ranges = std::vector<std::pair<std::uint64_t, std::uint64_t>>;
using Bindings = std::map<std::string, no::RequestHandle>;

// Same explicit-instantiation seam as test_id_adapter_storage.cpp. These
// pointers read real owners; no test writes a container or adds a product hook.
template<class Tag, auto Member> struct Access {
    friend auto access(Tag) { return Member; }
};
struct Issued { friend auto access(Issued); };
struct Requests { friend auto access(Requests); };
struct Entries { friend auto access(Entries); };
template struct Access<Issued, &no::WorkingRequestCore::issued_>;
template struct Access<Requests, &NativeExecutionConsumer::requests_>;
template struct Access<Entries, &Book::entries_>;

unsigned checks = 0;
unsigned failures = 0;
std::string trial = "dispatch";
std::string phase = "start";

void check(const char* row, bool value, const char* expression, int line) {
    ++checks;
    if (value) return;
    ++failures;
    std::fprintf(stderr, "FAIL [%s] %s:%d: %s (trial=%s, phase=%s)\n",
                 row, __FILE__, line, expression, trial.c_str(), phase.c_str());
}
#define ROW_CHECK(row, expression) check(row, bool(expression), #expression, __LINE__)

template<class Function>
void run_trial(const char* row, const std::string& label, Function function) {
    trial = label;
    phase = "start";
    try {
        function();
    } catch (const std::exception& error) {
        ++checks;
        ++failures;
        std::fprintf(stderr, "FAIL [%s] %s:%d: exception: %s (trial=%s, phase=%s)\n",
                     row, __FILE__, __LINE__, error.what(), trial.c_str(), phase.c_str());
    } catch (...) {
        ROW_CHECK(row, false);
    }
    // Count/equality failures never throw. An unexpected exception ends only
    // this trial; the dispatch loop still visits the next trial and row.
}

NativeRunSpec specification(const char* key) {
    NativeRunSpec spec;
    spec.identity = {key, 1};
    spec.input_tf = "1";
    spec.script_tf = "1";
    spec.tickerid = "TEST:KEYS";
    spec.timezone = "UTC";
    spec.session = "24x7";
    spec.initial_capital = 1000000.0;
    spec.point_value = 1.0;
    spec.account_fx = 1.0;
    spec.price_tick = 0.25;
    spec.close_execution = NativeCloseExecution::AfterCalculation;
    return spec;
}

std::vector<Bar> tape(int count) {
    std::vector<Bar> result;
    for (int index = 0; index < count; ++index) {
        result.push_back({100.0, 101.0, 99.0, 100.0, 1.0,
                          1736121600000LL + std::int64_t(index) * 60000});
    }
    return result;
}

no::Request resting(const std::string& label, double price = 50.0) {
    no::Request request{no::Transact{1.0}, label, "resting request"};
    request.trigger = no::Limit{price};
    return request;
}

template<class Host>
void run_host(Host& host, int bars, const char* row, const char* key) {
    const auto configured = host.configure_native(specification(key));
    ROW_CHECK(row, configured.status == NativeSetupStatus::Applied);
    if (configured.status != NativeSetupStatus::Applied) return;
    const auto input = tape(bars);
    host.run(input.data(), static_cast<int>(input.size()));
    ROW_CHECK(row, host.last_error().empty());
    ROW_CHECK(row, host.native_state().kind == NativeLifecycleKind::Completed);
    ROW_CHECK(row, host.callbacks == bars);
}

// Independent reference: partition the SET OF SCHEDULED issued numbers into
// maximal connected components on the integer line. No core counter, handle
// returned by the core, journal event, or issued_ element supplies the model.
Ranges integer_components(const std::set<std::uint64_t>& scheduled) {
    Ranges result;
    auto next = scheduled.begin();
    while (next != scheduled.end()) {
        const auto first = *next;
        auto last = first;
        ++next;
        while (next != scheduled.end() && *next - last == 1) {
            last = *next;
            ++next;
        }
        result.emplace_back(first, last);
    }
    return result;
}

class IssuedHost final : public NativeStrategyHost {
public:
    IssuedHost(int bars, bool gaps) : bars_(bars), gaps_(gaps) {}
    int callbacks = 0;
    std::size_t peak = 0;

    const Ranges& ranges() const {
        const auto& core = as_native_consumer(execution_consumer()).request_core();
        return core.*access(Issued{});
    }

    void begin_next_request_run() {
        auto& consumer = as_native_consumer(execution_consumer());
        auto& core = consumer.*access(Requests{});
        auto identity = core.identity();
        ++identity.run_number;
        // Call the real run-reset door after the host has completed. The
        // test never calls clear/erase/resize on issued_. No further host
        // execution is attempted after this core-only reset.
        core.reset(std::move(identity));
    }

    void on_native_bar(const Bar&, const NativeDecisionContext&) override {
        ++callbacks;
        phase = "issued bar " + std::to_string(callbacks);
        if (gaps_) {
            if (callbacks == 1) keeper_ = place("keeper");
            if (keeper_) {
                no::ReplaceOptions options;
                options.keep_handle = true;
                const auto changed = replace(*keeper_, resting("keeper", 49.0), options);
                ROW_CHECK("workingrequestcore_issued", changed.status == no::ReplaceStatus::Replaced);
                ROW_CHECK("workingrequestcore_issued", changed.successor == keeper_);
                // A kept-handle replacement spends one priority number but
                // issues no handle: the next submission leaves a one-number gap.
                ++next_scheduled_;
                compare();
            }
        }
        const auto satellite = place("satellite");
        if (satellite) {
            const auto cancelled = cancel(*satellite);
            ROW_CHECK("workingrequestcore_issued", cancelled.status == no::CancelStatus::Cancelled);
        }
        // Terminal commands retire live requests, not this run's issued ids.
        compare();
        if (callbacks == bars_ && keeper_) {
            const auto cancelled = cancel(*keeper_);
            ROW_CHECK("workingrequestcore_issued", cancelled.status == no::CancelStatus::Cancelled);
            compare();
        }
    }

private:
    std::optional<no::RequestHandle> place(const char* label) {
        const auto expected = next_scheduled_++;
        scheduled_.insert(expected);
        const auto accepted = submit(resting(label));
        ROW_CHECK("workingrequestcore_issued", accepted.status == no::SubmitStatus::Accepted);
        ROW_CHECK("workingrequestcore_issued", accepted.handle.has_value());
        if (accepted.handle) {
            ROW_CHECK("workingrequestcore_issued", accepted.handle->incarnation == expected);
        }
        compare();
        return accepted.handle;
    }

    void compare() {
        const auto expected = integer_components(scheduled_);
        ROW_CHECK("workingrequestcore_issued", ranges().size() == expected.size());
        ROW_CHECK("workingrequestcore_issued", ranges() == expected);
        peak = std::max(peak, ranges().size());
    }

    int bars_;
    bool gaps_;
    std::uint64_t next_scheduled_ = 1;
    std::set<std::uint64_t> scheduled_;
    std::optional<no::RequestHandle> keeper_;
};

void issued_rows() {
    std::size_t dense_end[2]{};
    for (bool gaps : {false, true}) {
        for (int scale = 0; scale != 2; ++scale) {
            const int bars = scale == 0 ? 16 : 64;
            run_trial("workingrequestcore_issued", std::string(gaps ? "gapped/" : "dense/")
                      + std::to_string(bars), [&] {
                IssuedHost host(bars, gaps);
                run_host(host, bars, "workingrequestcore_issued", "command-issued");
                const auto count = static_cast<std::size_t>(bars);
                if (gaps) {
                    // Keeper 1; priority numbers 2,4,...,2B are NOT issued;
                    // satellites 3,5,...,2B+1 each occupy their own component.
                    Ranges expected{{1, 1}};
                    for (std::uint64_t i = 1; i <= count; ++i) {
                        expected.emplace_back(2 * i + 1, 2 * i + 1);
                    }
                    ROW_CHECK("workingrequestcore_issued", host.ranges().size() == count + 1);
                    ROW_CHECK("workingrequestcore_issued", host.ranges() == expected);
                } else {
                    // One submit per bar issues 1..B; one connected component.
                    const Ranges expected{{1, count}};
                    ROW_CHECK("workingrequestcore_issued", host.ranges().size() == 1);
                    ROW_CHECK("workingrequestcore_issued", host.ranges() == expected);
                    dense_end[scale] = host.ranges().size();
                }
                ROW_CHECK("workingrequestcore_issued", host.native_working_requests().empty());
                phase = "next request run: zero commands issued";
                host.begin_next_request_run();
                ROW_CHECK("workingrequestcore_issued", host.ranges().empty());
            });
        }
    }
    phase = "dense count with four times as many bars";
    ROW_CHECK("workingrequestcore_issued", dense_end[0] == 1 && dense_end[1] == dense_end[0]);
}

struct BookSummary {
    std::size_t peak = 0;
    std::size_t end = 0;
};

class BookHost final : public NativeStrategyHost {
public:
    explicit BookHost(int population) : population_(population), book_(*this) {}
    int callbacks = 0;
    BookSummary summary;

    std::size_t entries() const { return (book_.*access(Entries{})).size(); }

    void on_native_bar(const Bar&, const NativeDecisionContext&) override {
        ++callbacks;
        const auto window = std::to_string(callbacks) + "/";
        phase = "new window " + window;
        compare(0);

        // Two independently submitted sets of P keys. One set leaves through
        // cancel(key), the other through forget(key). Unique window prefixes
        // expose old keys that a broken release leaves behind.
        for (int i = 0; i < population_; ++i) {
            place(window + "cancel/" + std::to_string(i), resting("cancel"));
            compare(static_cast<std::size_t>(2 * i + 1));
            place(window + "forget/" + std::to_string(i), resting("forget"));
            compare(static_cast<std::size_t>(2 * i + 2));
        }
        summary.peak = std::max(summary.peak, entries());
        const auto full = static_cast<std::size_t>(2 * population_);
        const auto changed_key = window + "cancel/0";
        const auto changed = book_.submit_or_replace_outcome(changed_key, resting("new", 40.0));
        ROW_CHECK("orderbook_entries", changed.action == tk::OrderBookAction::Replaced);
        ROW_CHECK("orderbook_entries", changed.handle().has_value());
        model_[changed_key] = changed.handle().value_or(no::RequestHandle{});
        phase = "live replace keeps the number of keys";
        compare(full);

        const no::Request bad{no::Transact{0.0}, "invalid", ""};
        const auto refused = book_.submit_or_replace_outcome(changed_key, bad);
        ROW_CHECK("orderbook_entries", refused.action == tk::OrderBookAction::ReplaceRejected);
        phase = "rejected live replace keeps its binding";
        compare(full);
        const auto missing = book_.submit_or_replace_outcome(window + "bad", bad);
        ROW_CHECK("orderbook_entries", missing.action == tk::OrderBookAction::SubmitRejected);
        phase = "rejected fresh submit adds no binding";
        compare(full);

        for (int i = 0; i < population_; ++i) {
            const auto key = window + "cancel/" + std::to_string(i);
            const auto result = book_.cancel_outcome(key);
            ROW_CHECK("orderbook_entries", result.known && result.commanded());
            ROW_CHECK("orderbook_entries", result.status() == no::CancelStatus::Cancelled);
            model_.erase(key);
            phase = "cancel releases one key " + key;
            // 2P initial keys minus the i+1 cancel commands just issued.
            compare(full - static_cast<std::size_t>(i + 1));
        }
        const auto unknown = book_.cancel_outcome(window + "unknown");
        ROW_CHECK("orderbook_entries", !unknown.known && !unknown.commanded());
        compare(static_cast<std::size_t>(population_));

        for (int i = 0; i < population_; ++i) {
            const auto key = window + "forget/" + std::to_string(i);
            const auto handle = model_.at(key);
            book_.forget(key);
            model_.erase(key);
            phase = "forget releases one key " + key;
            // P keys after cancel, minus the i+1 explicit forget commands.
            compare(static_cast<std::size_t>(population_ - i - 1));
            // Forget does not command the kernel. Withdraw the still-working
            // request separately, after observing the book's release boundary.
            const auto result = cancel(handle);
            ROW_CHECK("orderbook_entries", result.status == no::CancelStatus::Cancelled);
        }
        ROW_CHECK("orderbook_entries", native_working_requests().empty());

        // Exercise terminal events as well as still-working cancellations.
        // A fill alone keeps its toolkit key. A stale-key re-submit first
        // releases that key; an invalid new request then leaves it absent.
        const auto opening_key = window + "filled";
        const auto opening = place(opening_key, no::Request{no::Transact{1.0}, "open", ""});
        phase = "one opening command bound";
        compare(1);
        apply(opening);
        ROW_CHECK("orderbook_entries", physical_position().signed_units == 1.0);
        phase = "terminal fill retains its key until an explicit book operation";
        compare(1);
        const auto stale = book_.submit_or_replace_outcome(opening_key, bad);
        ROW_CHECK("orderbook_entries", stale.action == tk::OrderBookAction::SubmitRejected);
        model_.erase(opening_key);
        phase = "stale-key retry rejected: zero bound keys";
        compare(0);

        const auto closing_key = window + "closed";
        const auto closing = place(closing_key, no::Request{no::Flatten{}, "close", ""});
        phase = "one close command bound";
        compare(1);
        apply(closing);
        ROW_CHECK("orderbook_entries", physical_position().signed_units == 0.0);
        phase = "terminal close retains its key";
        compare(1);
        book_.forget(closing_key);
        model_.erase(closing_key);
        phase = "close key explicitly released";
        compare(0);
        ROW_CHECK("orderbook_entries", native_working_requests().empty());
        summary.end = entries();
    }

private:
    std::optional<no::RequestHandle> place(const std::string& key, const no::Request& request) {
        const auto result = book_.submit_or_replace_outcome(key, request);
        ROW_CHECK("orderbook_entries", result.action == tk::OrderBookAction::SubmitAccepted);
        ROW_CHECK("orderbook_entries", result.handle().has_value());
        // Model population follows the input command, even if the fixture
        // unexpectedly rejects it. A missing handle cannot lower the expected
        // count and turn that failure into an empty-window success.
        model_[key] = result.handle().value_or(no::RequestHandle{});
        return result.handle();
    }

    void apply(const std::optional<no::RequestHandle>& handle) {
        if (!handle) return;  // The missing handle was already recorded as FAIL.
        const auto result = execute_current(
            NativeCurrentExecution{*handle, NativeCurrentPriceRule::AsPresented});
        ROW_CHECK("orderbook_entries", std::holds_alternative<no::ExecutionAppliedEvent>(result));
    }

    void compare(std::size_t expected_count) const {
        const auto& actual = book_.*access(Entries{});
        Bindings observed;
        // Every entry is projected, including any stale key from an earlier
        // window. No label/pending/terminal filter can conceal retained rows.
        for (const auto& entry : actual) observed.emplace(entry.first, entry.second.handle);
        ROW_CHECK("orderbook_entries", actual.size() == expected_count);
        ROW_CHECK("orderbook_entries", model_.size() == expected_count);
        ROW_CHECK("orderbook_entries", observed == model_);
    }

    int population_;
    Book book_;
    Bindings model_;
};

void book_rows() {
    for (int population : {3, 11}) {
        BookSummary result[2]{};
        for (int scale = 0; scale != 2; ++scale) {
            const int bars = scale == 0 ? 16 : 64;
            run_trial("orderbook_entries", "book/" + std::to_string(population) + "/"
                      + std::to_string(bars), [&] {
                BookHost host(population);
                run_host(host, bars, "orderbook_entries", "command-book");
                result[scale] = host.summary;
                ROW_CHECK("orderbook_entries", host.entries() == 0);
                ROW_CHECK("orderbook_entries", host.summary.peak == static_cast<std::size_t>(2 * population));
            });
        }
        phase = "same command window on N and 4N bars";
        ROW_CHECK("orderbook_entries", result[0].end == 0 && result[1].end == result[0].end);
        ROW_CHECK("orderbook_entries", result[0].peak == static_cast<std::size_t>(2 * population)
                  && result[1].peak == result[0].peak);
    }
}

struct ListedRow { const char* row; const char* reason; };
constexpr ListedRow listed[] = {
    {"pineexecutionadapter_apply_terminal_explicit_market_policy_terminal_review_handles", "apply_terminal_explicit_market_policy local terminal_review_handles"},
    {"pineexecutionadapter_fill_pooc_close_entries_marketable", "fill_pooc_close_entries local marketable"},
    {"pineexecutionadapter_fill_pooc_close_pair_legs", "fill_pooc_close_pair local legs; Leg is also function-local"},
    {"pineexecutionadapter_order_same_point_entries_candidates", "order_same_point_entries local candidates; Candidate is function-local"},
    {"pineexecutionadapter_hash_state_named_cancel_keys", "hash_state local named_cancel_keys"},
    {"pineexecutionadapter_close_opposite_entries", "close local opposite_entries"},
    {"pineexecutionadapter_close_preceding_fifo_closes", "close local preceding_fifo_closes"},
    {"pineexecutionadapter_close_all_newborns", "close_all local newborns"},
    {"pineexecutionadapter_close_all_opposite_entries", "close_all local opposite_entries"},
    {"pineexecutionadapter_withdraw_unplaced_cap_adds_unplaced", "withdraw_unplaced_cap_adds local unplaced"},
    {"pineexecutionadapter_rearm_id_bound_exits_rearm", "rearm_id_bound_exits local rearm"},
    {"pineexecutionadapter_materialize_pending_bracket_legs_ready", "materialize_pending_bracket_legs local ready"},
    {"pineexecutionadapter_flush_pending_same_bar_commands_flat_pair_member", "flush_pending_same_bar_commands local flat_pair_member"},
    {"pineexecutionadapter_materialize_relative_exits_pending", "materialize_relative_exits local pending"},
    {"audit_placement_scan_copy", "anonymous-namespace audit_placement_scan local copy; audit-only build"},
    {"pineexecutionadapter_reconcile_deferred_exit_reservations_family_delayed", "reconcile_deferred_exit_reservations function-local Family::delayed"},
    {"pineexecutionadapter_stage_flat_children_before_parent_children", "stage_flat_children_before_parent local children"},
    {"pineexecutionadapter_place_entry_superseded_buy_replacements", "place_entry block-local superseded_buy_replacements"},
    {"pineexecutionadapter_place_entry_superseded_siblings", "place_entry block-local superseded_siblings"},
    {"pineexecutionadapter_flush_pending_closes_sites", "flush_pending_closes local sites"},
    {"ledger_command_selected_ids", "ledger_command local selected_ids in runner/main.cpp"},
    {"inlinecommandevents_overflow", "populated plan behind cpp-only Prepared*::Impl or a local plan; no real overflow insertion established"},
};
}  // namespace

int main() {
    for (const auto& row : listed) {
        (void)row;
    }
    struct Row { const char* id; void (*run)(); };
    const Row rows[] = {{"workingrequestcore_issued", issued_rows}, {"orderbook_entries", book_rows}};
    for (const auto& row : rows) run_trial(row.id, row.id, row.run);
    std::printf("memory proof command_keys: %u checks, %u failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
