// memory proof pending_state: pending source keys, admission drafts, native book and roster roots.
// The tests observe real submission and cancellation paths;
// the roster pass is forced at chosen bars.
// No byte count below is an allocation measurement.
#include <pineforge/source/pine_strategy_host.hpp>

#include "../src/native_execution_consumer.hpp"
#include "../src/source/pine_position_tail_summary.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <limits>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace {
using namespace pineforge;
namespace no = pineforge::native_order;
using Adapter = source::PineExecutionAdapter;
using SourceHost = source::PineStrategyHost;
using Core = no::WorkingRequestCore;

// The explicit-instantiation seam from test_id_adapter_storage.cpp. All
// product declarations, access specifiers and implementation units stay intact.
template<class Tag, auto Member> struct Access {
    friend auto access(Tag) { return Member; }
};
struct Keys { friend auto access(Keys); };
struct Placements { friend auto access(Placements); };
struct Spares { friend auto access(Spares); };
struct PendingCache { friend auto access(PendingCache); };
struct Tail { friend auto access(Tail); };
struct Retirement { friend auto access(Retirement); };
struct ReleaseOrigins { friend auto access(ReleaseOrigins); };
struct ResidentBook { friend auto access(ResidentBook); };
struct Repriced { friend auto access(Repriced); };
struct SpilledCount { friend auto access(SpilledCount); };
template struct Access<Keys, &Adapter::live_by_source_key_>;
template struct Access<Placements, &Adapter::placement_>;
template struct Access<Spares, &source::PlacementTable::spare_>;
template struct Access<PendingCache, &SourceHost::source_pending_view_cache_>;
template struct Access<Tail, &Adapter::position_tail_summary_>;
template struct Access<Retirement, &Adapter::retired_row_scratch_>;
template struct Access<ReleaseOrigins, &Adapter::release_closed_cohort_origins>;
template struct Access<ResidentBook, &Core::live_>;
template struct Access<Repriced, &Core::repriced_>;
template struct Access<SpilledCount, &Core::spilled_count>;

constexpr double kNa = std::numeric_limits<double>::quiet_NaN();
constexpr std::int64_t kStart = 1736121600000LL;
// Read from PlacementTable::kSpareNodes and native_order.cpp's kBookMax.
// These are row counts, not byte budgets.
// Independently: a spare insert consumes one of 64 slots; an incoming book
// row spills one resident predecessor whenever all 2^12 slots are occupied.
constexpr std::size_t kSpareSlots = 64;
constexpr std::size_t kResidentSlots = std::size_t{1} << 12;

std::size_t checks = 0;
std::size_t failures = 0;
std::map<std::string, std::size_t> positive;
std::string trial;
const char* dispatched_row = "setup";

void check(const char* row, bool value, const char* expression,
           const char* explanation, int line, std::size_t population = 0) {
    ++checks;
    if (value) {
        if (population != 0) ++positive[row];
        return;
    }
    ++failures;
    std::fprintf(stderr, "FAIL [%s] %s:%d: %s (%s; %s)\n",
                 row, __FILE__, line, expression, explanation, trial.c_str());
}
#define ROW_CHECK(row, value, why) check(row, (value), #value, why, __LINE__)
#define ROW_POP(row, value, why, population) \
    check(row, (value), #value, why, __LINE__, (population))
#define FIXTURE(value) ROW_CHECK(dispatched_row, value, "fixture prerequisite")

template<class Function> void attempt(const std::string& label, Function function) {
    trial = label;
    try {
        function();
    } catch (const std::exception& error) {
        check(dispatched_row, false, "no exception", error.what(), __LINE__);
    } catch (...) {
        check(dispatched_row, false, "no exception", "non-standard exception", __LINE__);
    }
    // A failed check never throws: every later callback, trial and row runs.
}

std::vector<Bar> tape(int count) {
    std::vector<Bar> bars;
    for (int i = 0; i < count; ++i)
        bars.push_back({100.0, 101.0, 99.0, 100.0, 1.0, kStart + i * 60000LL});
    return bars;
}

template<class T> std::vector<T> sorted(std::vector<T> values) {
    std::sort(values.begin(), values.end());
    return values;
}

class Host : public SourceHost {
public:
    explicit Host(int horizon) {
        source::PineStrategyConfig config;
        config.initial_capital = 1.0e9;
        config.margin_long = config.margin_short = 0.0;
        config.default_qty_type = static_cast<int>(QtyType::FIXED);
        config.default_qty_value = 2.0;
        config.pyramiding = 100000;
        configure_pine_strategy(config);
        set_syminfo_timezone("UTC");
        set_syminfo_session("24x7");
        set_syminfo_mintick(0.25);
        FIXTURE(set_realtime_tail(true, horizon));
    }
    NativeExecutionConsumer& consumer() {
        return as_native_consumer(execution_consumer());
    }
    const Core& core() { return consumer().request_core(); }
    const Adapter& adapter() const { return adapter_; }
    const source::PlacementTable& placements() const {
        return adapter().*access(Placements{});
    }
    const std::vector<FixtureIntentRow>& cache() const {
        return *this.*access(PendingCache{});
    }
    const std::vector<FixtureIntentRow>& query() const { return source_pending_view(); }
    std::size_t callbacks = 0;
};

enum class SourceRow { Keys, PlacementDraft, FixtureDraft };

class PendingHost final : public Host {
public:
    PendingHost(SourceRow row, std::size_t population, int horizon,
                bool explicit_quantity, bool query_enabled = true)
        : Host(horizon), row_(row), population_(population), horizon_(horizon),
          explicit_quantity_(explicit_quantity), query_enabled_(query_enabled) {}

    std::size_t held = 0, retired = 0, retired_spares = 0;
    bool saw_hold = false, saw_retirement = false;

    void on_source_bar(const Bar&) override {
        ++callbacks;
        const int bar = pine_bar_index();
        const int first_cancel = horizon_ / 2;
        const int second_cancel = horizon_ - 6;
        if (bar == 0) place(0);
        // The key row also walks real same-key replacement, with changing
        // levels, before cancellation. Its model still has one key per id.
        if (row_ == SourceRow::Keys && bar >= 4 && bar <= 7)
            for (const auto& id : expected_ids_)
                strategy_entry(id, true, 50.0 - bar, kNa, quantity());
        if (bar == first_cancel || bar == second_cancel) {
            strategy_cancel_all();
            expected_ids_.clear();
        }
        if (bar == first_cancel + 5) place(1);

        FIXTURE(native_working_requests().size() == expected_ids_.size());
        FIXTURE(live_position_size() == 0.0); // no bar reaches either limit
        if (row_ == SourceRow::Keys) check_keys();
        if (row_ == SourceRow::FixtureDraft) check_fixture_drafts();
        // K5 keeps a just-retired source row through the broker-open boundary.
        // Four later opens exceed that current/previous-script-bar window.
        const bool cancellation_window = (bar >= first_cancel && bar < first_cancel + 4)
            || (bar >= second_cancel && bar < second_cancel + 4);
        if (row_ == SourceRow::PlacementDraft && !cancellation_window)
            check_placement_drafts();

        if (bar == first_cancel - 1) {
            saw_hold = true;
            held = observed_count();
        }
        if (bar == horizon_ - 2) {
            saw_retirement = true;
            retired = observed_count();
            retired_spares = (placements().*access(Spares{})).size();
        }
    }

private:
    SourceRow row_;
    std::size_t population_;
    int horizon_;
    bool explicit_quantity_, query_enabled_;
    std::set<std::string> expected_ids_;
    double quantity() const { return explicit_quantity_ ? 2.0 : kNa; }

    void place(int wave) {
        for (std::size_t i = 0; i < population_; ++i) {
            const std::string id = "pending-wave-" + std::to_string(wave) + "-entry-" + std::to_string(i);
            expected_ids_.insert(id); // one script key, independent of storage
            strategy_entry(id, true, 50.0, kNa, quantity());
        }
    }

    std::size_t observed_count() const {
        if (row_ == SourceRow::Keys) return (adapter().*access(Keys{})).size();
        if (row_ == SourceRow::FixtureDraft) return cache().size();
        return placements().size();
    }

    void check_keys() {
        const auto& keys = adapter().*access(Keys{});
        std::vector<std::string> ids;
        std::vector<std::uint64_t> incarnations, native_incarnations;
        bool all_have_placement = true;
        for (const auto& item : keys) {
            incarnations.push_back(item.second.incarnation);
            const auto row = placements().find(item.second.incarnation);
            if (row == placements().end()) all_have_placement = false;
            else ids.push_back(row->second.source_id);
        }
        // Handles are opaque results of the real acceptance door; only their
        // values come from this public query. Expected membership/count is the
        // script's distinct id set, including zero immediately after cancel.
        for (const auto& working : native_working_requests())
            native_incarnations.push_back(working.definition->handle.incarnation);
        const std::vector<std::string> expected(expected_ids_.begin(), expected_ids_.end());
        ROW_POP("pineexecutionadapter_live_by_source_key", keys.size() == expected.size()
                    && sorted(ids) == expected && all_have_placement
                    && sorted(incarnations) == sorted(native_incarnations),
                "one current native handle per distinct source replacement key; cancelled keys retire",
                keys.size());
    }

    void check_placement_drafts() {
        std::vector<std::string> observed_ids;
        std::set<const admission::CommandObservation*> observations;
        std::size_t bound = 0;
        bool correctly_bound = true;
        for (const auto& item : placements()) {
            const auto& observation = item.second.market_admission.observation();
            if (!observation) { correctly_bound = false; continue; }
            ++bound;
            observations.insert(observation.get());
            observed_ids.push_back(observation->id);
            correctly_bound = correctly_bound && observation->id == item.second.source_id;
        }
        const auto& spares = placements().*access(Spares{});
        std::size_t spare_bound = 0;
        for (const auto& node : spares)
            if (node && node->market_admission.observation()) ++spare_bound;
        const std::vector<std::string> expected(expected_ids_.begin(), expected_ids_.end());
        const std::size_t expected_spares = expected.empty()
            ? std::min(population_, kSpareSlots) : 0;
        // Each of N entry commands binds one shared observation. After all N
        // cancels, no active row is askable. Erasure puts min(N,64) nodes in
        // the spare pool; a second N-row wave consumes that entire pool.
        ROW_POP("placementsnapshot_market_admission", placements().size() == expected.size()
                    && bound == expected.size() && observations.size() == expected.size()
                    && sorted(observed_ids) == expected && correctly_bound,
                "active placement admission owners equal the independent pending command set",
                bound);
        ROW_CHECK("placementsnapshot_market_admission", spares.size() == expected_spares
                    && spare_bound == expected_spares
                    && bound + spare_bound == expected.size() + expected_spares,
                  "all retained admission owners include exactly the bounded erased-node pool");
    }

    void check_fixture_drafts() {
        if (!query_enabled_) {
            ROW_CHECK("fixtureintentrow_market_admission", cache().empty(),
                      "without the diagnostic query no FixtureIntentRow admission owners are inserted");
            return;
        }
        // Two consecutive projections are independent reads of the same
        // pending set. Retaining the old cache instead of clearing it doubles
        // this count on the second call, even before a cancel occurs.
        for (int repeat = 0; repeat < 2; ++repeat) {
            const auto& rows = query();
            std::vector<std::string> ids;
            std::set<const admission::CommandObservation*> observations;
            std::size_t bound = 0;
            bool correct = &rows == &cache();
            for (const auto& row : rows) {
                const auto& observation = row.market_admission.observation();
                if (!observation) { correct = false; continue; }
                ++bound;
                observations.insert(observation.get());
                ids.push_back(observation->id);
                correct = correct && row.id == observation->id;
            }
            const std::vector<std::string> expected(expected_ids_.begin(), expected_ids_.end());
            ROW_POP("fixtureintentrow_market_admission", rows.size() == expected.size()
                        && bound == expected.size() && observations.size() == expected.size()
                        && sorted(ids) == expected && correct,
                    "diagnostic draft population is N current commands per query, then zero after cancellation",
                    bound);
        }
    }
};

void source_row(SourceRow row) {
    for (const std::size_t population : {17U, 68U}) {
        for (const bool explicit_quantity : {false, true}) {
            // The identical script shape runs on N bars and 4N bars. Only the
            // hold interval changes; both observed populations must match.
            for (const bool query_enabled : {true, false}) {
                if (row != SourceRow::FixtureDraft && !query_enabled) continue;
                std::vector<std::pair<std::size_t, std::size_t>> counts;
                for (const int bars : {32, 128}) {
                    attempt("Q=" + std::to_string(population) + " bars=" + std::to_string(bars)
                                + (explicit_quantity ? " explicit" : " default")
                                + (query_enabled ? " queried" : " unqueried"), [&] {
                        PendingHost host(row, population, bars, explicit_quantity, query_enabled);
                        const auto input = tape(bars);
                        host.run(input.data(), bars, "1", "1");
                        FIXTURE(host.last_error().empty() && host.callbacks == static_cast<std::size_t>(bars));
                        FIXTURE(host.saw_hold && host.saw_retirement);
                        const auto expected_hold = row == SourceRow::FixtureDraft && !query_enabled
                            ? 0 : population;
                        ROW_CHECK(dispatched_row, host.held == expected_hold && host.retired == 0,
                                  "N pending owners during the hold; zero active owners after final retirement");
                        counts.emplace_back(host.held, host.retired);
                    });
                }
                ROW_CHECK(dispatched_row, counts.size() == 2 && counts[0] == counts[1],
                          "same pending-container counts on N bars and 4N bars");
            }
        }
    }
}

enum class NativeRow { Book, Conversion };

class BookHost final : public Host {
public:
    BookHost(NativeRow row, std::size_t population, int horizon)
        : Host(horizon), row_(row), population_(population), horizon_(horizon) {}
    std::size_t resident_held = 0;
    bool saw_held = false, saw_empty = false;
    void on_source_bar(const Bar&) override {
        ++callbacks;
        const int bar = pine_bar_index();
        if (bar == 0) {
            for (std::size_t i = 0; i < population_; ++i) {
                const auto id = "pending-native-pending-" + std::to_string(i);
                expected_ids_.insert(id);
                // strategy.order reaches submit_or_replace -> host.submit,
                // with an explicit Transact independent of default sizing.
                strategy_order(id, true, 1.0, 50.0);
            }
        }
        if (bar == horizon_ - 4) {
            strategy_cancel_all();
            expected_ids_.clear();
        }
        if (bar != 0 && bar != 1 && bar != horizon_ - 5
            && bar != horizon_ - 4 && bar != horizon_ - 1) return;
        FIXTURE(live_position_size() == 0.0);
        if (row_ == NativeRow::Book) check_book();
        else check_conversion();
        if (bar == 1) {
            resident_held = (core().*access(ResidentBook{})).size();
            saw_held = true;
        }
        if (bar == horizon_ - 1) saw_empty = core().live_size() == 0;
    }
private:
    NativeRow row_;
    std::size_t population_;
    int horizon_;
    std::set<std::string> expected_ids_;

    void check_book() {
        const auto& book = core();
        const auto resident = (book.*access(ResidentBook{})).size();
        const auto spilled = (book.*access(SpilledCount{}))();
        const auto expected_resident = std::min(expected_ids_.size(), kResidentSlots);
        std::vector<std::string> ids;
        for (const auto& row : book.live()) ids.push_back(row.request().label);
        const std::vector<std::string> expected(expected_ids_.begin(), expected_ids_.end());
        // For Q commands, filling all resident slots then spilling one before
        // every later append gives resident=min(Q,2^12), external=Q-resident.
        ROW_POP("delegated_native_request_allocation", book.live_size() == expected.size()
                    && sorted(ids) == expected && resident == expected_resident
                    && spilled == expected.size() - expected_resident
                    && book.book_spilled() == (expected.size() > kResidentSlots)
                    && (book.*access(Repriced{})).empty(),
                "real adapter submits preserve Q live requests with a fixed resident book and zero after cancels",
                book.live_size());
    }

    void check_conversion() {
        for (int repeat = 0; repeat < 2; ++repeat) {
            // This is the actual public conversion's returned owning object,
            // not a reconstruction from iterators in the test. Both reads
            // must be fresh, including the zero-size read after cancellation.
            const std::vector<no::LiveRequest> rows = core().live();
            std::vector<std::string> ids;
            bool terms_match = true;
            for (const auto& row : rows) {
                ids.push_back(row.request().label);
                const auto* units = std::get_if<no::Transact>(&row.request().intent);
                const auto* limit = std::get_if<no::Limit>(&row.request().trigger);
                terms_match = terms_match && units && units->signed_units == 1.0
                    && limit;
            }
            const std::vector<std::string> expected(expected_ids_.begin(), expected_ids_.end());
            ROW_POP("workingrequestcore_liveview_conversion_to_std_vector_liverequest_rows", rows.size() == expected.size()
                        && sorted(ids) == expected && terms_match,
                    "returned LiveView vector equals the independent pending request model on every conversion",
                    rows.size());
        }
    }
};

void native_row(NativeRow row) {
    // The book row crosses its resident threshold in both trials, by a factor
    // of four in total pending requests. The conversion also spans that spill.
    const std::vector<std::size_t> populations = row == NativeRow::Book
        ? std::vector<std::size_t>{kResidentSlots + 1, 4 * (kResidentSlots + 1)}
        : std::vector<std::size_t>{17, 68, kResidentSlots + 1};
    std::vector<std::size_t> residents;
    for (const auto population : populations) {
        for (const int bars : {8, 32}) {
            attempt("Q=" + std::to_string(population) + " bars=" + std::to_string(bars), [&] {
                BookHost host(row, population, bars);
                const auto input = tape(bars);
                host.run(input.data(), bars, "1", "1");
                FIXTURE(host.last_error().empty() && host.callbacks == static_cast<std::size_t>(bars));
                FIXTURE(host.saw_held && host.saw_empty);
                if (row == NativeRow::Book) residents.push_back(host.resident_held);
            });
        }
    }
    if (row == NativeRow::Book)
        ROW_CHECK("delegated_native_request_allocation", residents.size() == 4
                    && std::all_of(residents.begin(), residents.end(), [](std::size_t count) {
                        return count == kResidentSlots;
                    }),
                  "resident count remains 2^12 at Q and 4Q, on N and 4N bars");
}

class RootsHost final : public Host {
public:
    RootsHost(std::size_t population, int horizon, bool explicit_quantity)
        : Host(horizon), population_(population), horizon_(horizon), explicit_(explicit_quantity) {}
    bool quiet_reset = false;
    bool saw_large = false, saw_smaller = false, saw_fresh_wave = false;
    std::size_t held_count = 0, smaller_count = 0;
    void on_source_bar(const Bar&) override {
        ++callbacks;
        const int bar = pine_bar_index();
        if (quiet_reset) {
            const auto* summary = tail();
            ROW_CHECK("adapterpositiontailsummary_roster_roots", summary == nullptr || summary->roster_roots.empty(),
                      "reset_for_run releases the prior run's roster-root workset");
            return;
        }
        if (bar == 0) {
            place(0);
            full_pass();
            compare(expected_roots(), "two native rosters per origin deduplicate to N roots");
            held_count = root_count();
            saw_large = true;
        }
        if (bar == horizon_ / 2) {
            const auto old = expected_roots();
            const std::size_t remove = population_ / 2;
            for (std::size_t i = 0; i < remove; ++i) {
                const auto id = "pending-roots-0-" + std::to_string(i);
                strategy_cancel(id);
                expected_.erase(id);
            }
            // The first pass collects the old rosters, then removes the now
            // absent origins. The next full pass must hold only survivors.
            full_pass();
            compare(old, "first retirement pass records the roster it visited");
            full_pass();
            compare(expected_roots(), "second pass replaces old roots with ceil(N/2) live roots");
            smaller_count = root_count();
            saw_smaller = true;
        }
        if (bar == horizon_ - 6) {
            const auto old = expected_roots();
            strategy_cancel_all();
            expected_.clear();
            full_pass();
            compare(old, "the last nonempty roster pass records its input before removal");
            full_pass();
            // release_closed_cohort_origins returns before rewriting scratch
            // when members==0.
            compare(old, "a memberless pass retains the last workset, within its prior high water");
        }
        if (bar == horizon_ - 5) {
            place(1);
            full_pass();
            compare(expected_roots(), "a fresh N-root wave replaces every old incarnation");
            saw_fresh_wave = true;
        }
        FIXTURE(live_position_size() == 0.0);
        FIXTURE(core().live_size() == expected_.size());
        if (bar > 0 && bar < horizon_ / 2)
            compare(expected_roots(), "quiet bars do not add roster roots");
    }
private:
    std::size_t population_;
    int horizon_;
    bool explicit_;
    std::map<std::string, no::RequestHandle> expected_;

    const source::detail::AdapterPositionTailSummary* tail() const {
        const auto& owner = adapter().*access(Tail{});
        return owner ? static_cast<const source::detail::AdapterPositionTailSummary*>(owner.get()) : nullptr;
    }
    std::size_t root_count() const { return tail() ? tail()->roster_roots.size() : 0; }
    std::vector<std::uint64_t> expected_roots() const {
        std::vector<std::uint64_t> ids;
        for (const auto& item : expected_) ids.push_back(item.second.incarnation);
        return sorted(std::move(ids));
    }
    void compare(const std::vector<std::uint64_t>& expected, const char* reason) const {
        const auto* summary = tail();
        ROW_POP("adapterpositiontailsummary_roster_roots", summary && summary->roster_roots == expected,
                reason, root_count());
    }
    void full_pass() {
        // Only schedule the existing private pass. No container is seeded,
        // edited or modelled in product storage; it reads real accepted native
        // cohorts, just as at bar open. N>=17 leaves >64 member*working pairs.
        (adapter_.*access(Retirement{})).roster_pass_due = true;
        (adapter_.*access(ReleaseOrigins{}))();
    }
    void place(int wave) {
        std::set<std::string> new_ids;
        for (std::size_t i = 0; i < population_; ++i) {
            const auto id = "pending-roots-" + std::to_string(wave) + "-" + std::to_string(i);
            new_ids.insert(id);
            expected_.emplace(id, no::RequestHandle{});
            strategy_entry(id, true, 50.0, kNa, explicit_ ? 2.0 : kNa);
        }
        // Script membership and cardinality are independent of this query.
        // Capture only each accepted handle's opaque identity for the model.
        for (const auto& live : native_working_requests()) {
            const auto found = expected_.find(live.definition->request.label);
            if (found != expected_.end()) found->second = live.definition->handle;
        }
        const auto duplicate_roster = cohort_open();
        for (const auto& id : new_ids) {
            const auto handle = expected_.at(id);
            FIXTURE(handle.incarnation != 0);
            if (handle.incarnation != 0) cohort_add(duplicate_roster, handle);
        }
        FIXTURE(core().live_size() == expected_.size());
    }
};

void roots_row() {
    for (const std::size_t population : {17U, 68U}) {
        for (const bool explicit_quantity : {false, true}) {
            std::vector<std::pair<std::size_t, std::size_t>> counts;
            for (const int bars : {32, 128}) {
                attempt("roots Q=" + std::to_string(population) + " bars=" + std::to_string(bars)
                            + (explicit_quantity ? " explicit" : " default"), [&] {
                    RootsHost host(population, bars, explicit_quantity);
                    const auto input = tape(bars);
                    host.run(input.data(), bars, "1", "1");
                    FIXTURE(host.last_error().empty() && host.callbacks == static_cast<std::size_t>(bars));
                    FIXTURE(host.saw_large && host.saw_smaller && host.saw_fresh_wave);
                    ROW_CHECK("adapterpositiontailsummary_roster_roots", host.held_count == population
                                && host.smaller_count == population - population / 2,
                              "N pending roots shrink to the independently retained half");
                    counts.emplace_back(host.held_count, host.smaller_count);
                    host.quiet_reset = true;
                    host.callbacks = 0;
                    const auto quiet = tape(4);
                    host.run(quiet.data(), 4, "1", "1");
                    FIXTURE(host.last_error().empty() && host.callbacks == 4);
                });
            }
            ROW_CHECK("adapterpositiontailsummary_roster_roots", counts.size() == 2 && counts[0] == counts[1],
                      "the same roster-root populations on N bars and 4N bars");
        }
    }
}

struct Row { const char* id; void (*run)(); };
void keys_row() { source_row(SourceRow::Keys); }
void placement_row() { source_row(SourceRow::PlacementDraft); }
void fixture_row() { source_row(SourceRow::FixtureDraft); }
void book_row() { native_row(NativeRow::Book); }
void conversion_row() { native_row(NativeRow::Conversion); }
} // namespace

int main() {
    const Row rows[] = {
        {"pineexecutionadapter_live_by_source_key", keys_row},
        {"placementsnapshot_market_admission", placement_row},
        {"fixtureintentrow_market_admission", fixture_row},
        {"delegated_native_request_allocation", book_row},
        {"workingrequestcore_liveview_conversion_to_std_vector_liverequest_rows", conversion_row},
        {"adapterpositiontailsummary_roster_roots", roots_row},
    };
    for (const auto& row : rows) {
        dispatched_row = row.id;
        attempt(std::string("row dispatch ") + row.id, row.run);
        ROW_CHECK(row.id, positive[row.id] != 0,
                  "a successful positive real-container assertion must have been reached");
    }
    std::printf("memory proof pending_state: %zu checks, %zu failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
