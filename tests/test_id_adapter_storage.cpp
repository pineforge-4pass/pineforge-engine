// CR-150 L19l: the production resident/disk container and real OriginRoster,
// compared with the old unordered-map + insertion-vector logical state.
#define PINEFORGE_TEST_ALLOCATION_BYTES
#include "global_allocation_replacement.hpp"
#include <pineforge/source/pine_strategy_host.hpp>
#include <pineforge/detail/scratch_defaults.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <map>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace {
using pineforge::BrokerStateHashSink;
using pineforge::source::OriginRoster;
namespace no = pineforge::native_order;
using Adapter = pineforge::source::PineExecutionAdapter;

// The same explicit-instantiation access seam as pending_observer_walk:
// no changed class definition or production friend.
template<class Tag, auto Member> struct Access {
    friend auto access(Tag) { return Member; }
};
struct Cohorts { friend auto access(Cohorts); };
struct CohortFor { friend auto access(CohortFor); };
struct CohortSlots { friend auto access(CohortSlots); };
template struct Access<Cohorts, &Adapter::cohorts_by_id_>;
template struct Access<CohortFor, &Adapter::cohort_for>;
template struct Access<CohortSlots, &Adapter::cohort_opened_slot_count_>;

void require(bool value, const char* why) {
    if (!value) throw std::runtime_error(why);
}

// The disk codec has the same five scalar fields as the adapter's codec.
// The reference below retains every origin, rather than using that codec.
struct Facts {
    struct Record {
        std::uint64_t handle;
        std::int64_t cycle;
        std::uint64_t count, digest, sides;
    };
    no::CohortHandle handle{};
    OriginRoster origins;
    std::vector<no::RequestHandle> opened;
    std::unordered_map<std::uint64_t, double> units;
    std::int64_t cycle = 0;
    std::uint64_t sides = 0;
    Record retired_record() const noexcept {
        return {handle.value, cycle, origins.size(), origins.digest(), sides};
    }
    static Facts restore_record(const Record& row) {
        Facts result;
        result.handle.value = row.handle;
        result.cycle = row.cycle;
        result.sides = row.sides;
        result.origins = OriginRoster::from_retired(static_cast<std::size_t>(row.count), row.digest);
        return result;
    }
    bool spillable() const noexcept {
        return origins.members().empty() && opened.empty() && units.empty();
    }
    bool held() const noexcept { return !opened.empty() || !units.empty(); }
};
using Table = pineforge::detail::DiskResidentIdTable<Facts>;

// This is the previous in-memory representation, with no retirement or
// production restore/digest helper in its reference fold.
struct ReferenceFacts {
    std::uint64_t handle = 0;
    std::int64_t cycle = 0;
    std::vector<no::RequestHandle> origins;
    std::vector<no::RequestHandle> opened;
    std::map<std::uint64_t, double> units;
    std::uint64_t sides = 0;
};
using Reference = std::unordered_map<std::string, ReferenceFacts>;

void hash_handles(BrokerStateHashSink& sink, const std::vector<no::RequestHandle>& handles) {
    sink.u(handles.size());
    for (const auto& handle : handles) {
        sink.s(handle.run.session_key);
        sink.u(0); // The source fold canonicalizes native run generations.
        sink.u(handle.incarnation);
    }
}
void hash_reference(BrokerStateHashSink& sink, const std::string& id, const ReferenceFacts& facts) {
    sink.s(id); sink.u(facts.handle); sink.i(facts.cycle);
    std::uint64_t digest = 1469598103934665603ULL;
    for (const auto& origin : facts.origins)
        digest = (digest ^ origin.incarnation) * 1099511628211ULL;
    sink.u(facts.origins.size()); sink.u(digest);
    hash_handles(sink, facts.opened); sink.u(facts.sides);
    sink.u(facts.units.size());
    for (const auto& unit : facts.units) { sink.u(unit.first); sink.d(unit.second); }
}
void hash_actual(BrokerStateHashSink& sink, const std::string& id, const Facts& facts) {
    sink.s(id); sink.u(facts.handle.value); sink.i(facts.cycle);
    sink.u(facts.origins.size()); sink.u(facts.origins.digest());
    hash_handles(sink, facts.opened); sink.u(facts.sides);
    std::map<std::uint64_t, double> sorted(facts.units.begin(), facts.units.end());
    sink.u(sorted.size());
    for (const auto& unit : sorted) { sink.u(unit.first); sink.d(unit.second); }
}

void same_fold(const Table& table, const Reference& reference, const std::vector<std::string>& order) {
    std::vector<std::string> sorted;
    for (const auto& row : reference) sorted.push_back(row.first);
    std::sort(sorted.begin(), sorted.end());
    BrokerStateHashSink expected, actual;
    expected.u(reference.size());
    for (const auto& id : sorted) hash_reference(expected, id, reference.at(id));
    expected.u(order.size());
    for (const auto& id : order) expected.s(id);
    actual.u(table.size());
    for (const auto& row : table.all_sorted()) hash_actual(actual, row.first, row.second);
    actual.u(table.size());
    table.for_each_id([&](const auto& id) { actual.s(id); });
    require(actual.h == expected.h, "cohort hash/order/count differs from in-memory fold");
}

std::string id_for(std::size_t n) {
    return "census-entry-" + std::to_string(n) + std::string("\0tail", 5);
}
no::RequestHandle origin_for(std::uint64_t n) { return {{"disk-cohort-test", 1}, n}; }

void check_held_rows(const Table& table) {
    std::vector<std::string> expected;
    for (const auto& row : table.resident_rows())
        if (!row.second.opened.empty() || !row.second.units.empty())
            expected.push_back(row.first);
    const auto before = table.work_counters();
    const auto allocations = global_allocation::allocations;
    global_allocation::refusing = true;
    std::size_t visited = 0;
    for (const auto& row : table.held_rows()) {
        require(visited < expected.size() && row.first == expected[visited],
                "held offsets equal the independent resident predicate in visiting order");
        ++visited;
    }
    global_allocation::refusing = false;
    const auto after = table.work_counters();
    require(visited == expected.size() && table.held_rows_exact()
            && table.held_size() == expected.size(), "held set has neither omissions nor extras");
    require(after.held_rows_visited - before.held_rows_visited == expected.size()
            && after.resident_rows_visited == before.resident_rows_visited
            && after.iterated_rows == before.iterated_rows,
            "held walk visits H rows without resident lookup or generic iteration");
    require(global_allocation::allocations == allocations, "held walk allocates nothing");
}

void held_membership_on_every_fuzz_mutation() {
    Table table;
    constexpr std::size_t population = 48;
    for (std::size_t i = 0; i < population; ++i) {
        Facts facts;
        facts.handle.value = i + 1;
        // These placed-never-filled origins keep every row non-spillable.
        facts.origins.push_back(origin_for(i + 1));
        table.emplace(id_for(i), std::move(facts));
        check_held_rows(table);
    }
    require(table.resident_size() == population && table.held_size() == 0,
            "pending cohorts remain resident but do not belong to the held set");
    {
        const auto row = table.find(id_for(0));
        row->second.units[901] = 1.5; // Explicit units-only gain, independent of fuzz coverage.
        table.refresh_held(row);
        check_held_rows(table);
        require(row->second.opened.empty() && table.held_size() == 1,
                "live units alone hold a cohort");
        row->second.units.erase(901);
        table.refresh_held(row);
        check_held_rows(table);
        row->second.opened.push_back(origin_for(902)); // Explicit openings-only gain.
        table.refresh_held(row);
        check_held_rows(table);
        require(row->second.units.empty() && table.held_size() == 1,
                "an opening alone holds a cohort");
        row->second.opened.clear();
        table.refresh_held(row);
        check_held_rows(table);
    }
    std::uint64_t random = 0xb7c3a94dULL;
    for (std::size_t step = 0; step < 768; ++step) {
        random = random * 6364136223846793005ULL + 1442695040888963407ULL;
        const auto slot = static_cast<std::size_t>((random >> 32) % population);
        auto row = table.find(id_for(slot));
        auto& facts = row->second;
        const auto origin = origin_for(1000 + slot);
        switch ((random >> 24) % 7) {
        case 0: facts.opened.push_back(origin); break;
        case 1: facts.units[origin.incarnation] += 0.25; break; // units-only gain
        case 2: if (!facts.opened.empty()) facts.opened.erase(facts.opened.begin()); break;
        case 3: facts.opened.clear(); break;
        case 4: facts.units.erase(origin.incarnation); break;
        case 5: facts.units.clear(); break;
        case 6: // Partial consumption also preserves membership.
            if (!facts.units.empty()) facts.units.begin()->second *= 0.5;
            break;
        }
        table.refresh_held(row);
        check_held_rows(table); // require(), including in Release, after EVERY mutation
        if (step % 11 == 0) {
            table.spill();
            check_held_rows(table);
        }
    }
    {
        const auto row = table.find(id_for(0));
        row->second.units[903] = 2.0;
        table.refresh_held(row);
        check_held_rows(table);
    }
    Table moved(std::move(table));
    check_held_rows(moved);
    check_held_rows(table);
    table = std::move(moved);
    check_held_rows(table);
    check_held_rows(moved);
    // Current-member erasure during a held walk must preserve the next row.
    const auto held = table.held_rows();
    for (auto at = held.begin(); at != held.end();) {
        const auto current = at++;
        current->second.opened.clear();
        table.refresh_held(current);
        check_held_rows(table);
        current->second.units.clear();
        table.refresh_held(current);
        check_held_rows(table);
    }
    require(table.held_size() == 0 && table.resident_size() == population,
            "held set drains even while pending origins pin every resident");
    table.clear();
    check_held_rows(table);
}

// The production retired codec cannot contain units. Exercise the generic
// load path's obligation with an actual disk record whose codec CAN restore
// them, without first calling refresh_held() on the restored row.
struct RestoredUnits {
    struct Record { std::uint64_t origin; double units; };
    std::unordered_map<std::uint64_t, double> live_units_by_origin;
    Record retired_record() const noexcept {
        return live_units_by_origin.empty() ? Record{} : Record{
            live_units_by_origin.begin()->first, live_units_by_origin.begin()->second};
    }
    static RestoredUnits restore_record(const Record& record) {
        RestoredUnits result;
        if (record.units > 0.0) result.live_units_by_origin[record.origin] = record.units;
        return result;
    }
    bool held() const noexcept { return !live_units_by_origin.empty(); }
    bool spillable() const noexcept { return !held(); }
};
using RestoredTable = pineforge::detail::DiskResidentIdTable<RestoredUnits>;
struct RestoredIndex { friend auto access(RestoredIndex); };
template struct Access<RestoredIndex, &RestoredTable::index_>;

void restored_units_join_the_held_set() {
    RestoredTable table;
    auto& index = table.*access(RestoredIndex{});
    const std::string id = id_for(19);
    const std::string later_id = id_for(3);
    index.insert(id, RestoredUnits::Record{713, 2.5});
    index.insert(later_id, RestoredUnits::Record{714, 1.25});
    require(table.resident_size() == 0 && table.held_size() == 0,
            "restoration witness starts with only disk bytes");
    const auto later = table.find(later_id); // Restore in the opposite order.
    const auto row = table.find(id);
    require(later != table.end() && later->second.live_units_by_origin.at(714) == 1.25
            && row != table.end() && row->second.live_units_by_origin.at(713) == 2.5,
            "the live-unit record is decoded from disk");
    std::size_t visited = 0;
    for (const auto& held : table.held_rows()) {
        require(visited < 2
                && held.first == (visited == 0 ? id : later_id)
                && &held.second == (visited == 0 ? &row->second : &later->second),
                "load registers exact restored rows in original offset order");
        ++visited;
    }
    require(visited == 2 && table.held_rows_exact(), "restored live units are held without a refresh");
}

void replay_recurring_ids() {
    Table table;
    Reference reference;
    std::vector<std::string> order;
    for (std::size_t i = 0; i < 2048; ++i) {
        const auto id = id_for((i * 137) % 2048);
        Facts facts;
        facts.handle.value = i + 1;
        const auto placed = table.emplace(id, std::move(facts));
        require(placed.second, "new cohort");
        ReferenceFacts old;
        old.handle = i + 1;
        reference.emplace(id, std::move(old));
        order.push_back(id);
        auto& current = placed.first->second;
        const auto origin = origin_for(i + 1);
        current.origins.push_back(origin);
        current.cycle = static_cast<std::int64_t>(i + 1);
        current.sides = (i % 2 == 0) ? 1 : 2;
        current.origins.retire_before(current.origins.size());
        auto& expected = reference.at(id);
        expected.origins.push_back(origin);
        expected.cycle = current.cycle;
        expected.sides = current.sides;
    }
    table.spill();
    require(table.resident_size() == 0, "retired cohorts remain on heap");
    same_fold(table, reference, order);
    table.spill();
    for (std::size_t i = 0; i < order.size(); i += 17) {
        const auto& id = order[i];
        const auto found = table.find(id);
        require(found != table.end(), "old cohort id is lost");
        auto& current = found->second;
        auto& expected = reference.at(id);
        require(current.handle.value == expected.handle, "recurring id got another handle");
        const auto origin = origin_for(10000 + i);
        current.origins.push_back(origin);
        current.opened.push_back(origin);
        table.refresh_held(found);
        current.units[origin.incarnation] = 3.25;
        table.refresh_held(found);
        current.cycle += 5000;
        expected.origins.push_back(origin);
        expected.opened.push_back(origin);
        expected.units[origin.incarnation] = 3.25;
        expected.cycle += 5000;
    }
    same_fold(table, reference, order);
    table.spill();
    std::vector<std::string> expected_live, actual_live;
    for (const auto& id : order)
        if (!reference.at(id).opened.empty()) expected_live.push_back(id);
    for (const auto& id : table.resident_ids()) actual_live.push_back(id);
    require(actual_live == expected_live, "live cohort walk lost first-insertion order");
    for (auto at = table.begin(); at != table.end(); ++at) {
        auto& row = *at;
        row.second.opened.clear();
        table.refresh_held(at);
        row.second.units.clear();
        table.refresh_held(at);
        row.second.origins.retire_before(row.second.origins.size());
        auto& expected = reference.at(row.first);
        expected.opened.clear();
        expected.units.clear();
    }
    table.spill();
    require(table.resident_size() == 0, "empty live tables did not leave RAM");
    same_fold(table, reference, order);
}

void pinned_lookup_and_resident_reads() {
    Table table;
    Facts initial;
    initial.handle.value = 1;
    {
        const auto first = table.emplace("first", std::move(initial)).first;
        first->second.cycle = 7;
        Facts next;
        next.handle.value = 2;
        table.emplace("next", std::move(next));
        const auto again = table.find("first");
        require(&first->second == &again->second, "nested lookup lost pinned identity");
        again->second.cycle = 9;
        table.spill();
        require(first->second.cycle == 9, "nested mutation is stale");
    }
    table.spill();
    require(table.resident_size() == 0, "released pins retain empty facts");
    require(table.find_resident("first") == table.end(), "retired cohort carries live exposure");
    require(table.find("first")->second.cycle == 9, "writeback lost the mutation");
    table.clear();
    require(table.size() == 0 && table.begin() == table.end(), "run reset retains ids");
}

std::size_t retained_bytes(std::size_t count, std::size_t period) {
    global_allocation::recording = true;
    std::size_t bytes = 0;
    {
        const auto before = global_allocation::live_bytes;
        Table table;
        for (std::size_t i = 0; i < count; ++i) {
            const auto id = id_for(period == 0 ? i : i % period);
            auto found = table.find(id);
            if (found == table.end()) {
                Facts facts;
                facts.handle.value = i + 1;
                found = table.emplace(id, std::move(facts)).first;
            }
            found->second.origins.push_back(origin_for(i + 1));
            found->second.origins.retire_before(found->second.origins.size());
        }
        table.spill();
        require(table.resident_size() == 0, "Q9 did not exercise disk eviction");
        bytes = global_allocation::live_bytes - before;
    }
    global_allocation::recording = false;
    return bytes;
}

void q9_difference_of_differences() {
    const auto reused_low = retained_bytes(10, 1);
    const auto unique_low = retained_bytes(10, 0);
    const auto reused_high = retained_bytes(10000, 1);
    const auto unique_high = retained_bytes(10000, 0);
    const auto low_delta = static_cast<std::int64_t>(unique_low) - static_cast<std::int64_t>(reused_low);
    const auto high_delta = static_cast<std::int64_t>(unique_high) - static_cast<std::int64_t>(reused_high);
    require(high_delta <= low_delta + 1024, "Q9 unique-minus-reused retained-byte slope grows");
}

class IdHost final : public pineforge::source::PineStrategyHost {
public:
    explicit IdHost(int period) : period_(period) {
        pineforge::source::PineStrategyConfig config;
        config.initial_capital = 1000000.0;
        config.process_orders_on_close = true;
        config.default_qty_value = 1.0;
        configure_pine_strategy(config);
        set_syminfo_timezone("UTC");
        set_syminfo_session("24x7");
        set_syminfo_mintick(0.25);
    }
    void on_source_bar(const pineforge::Bar&) override {
        const int i = pine_bar_index();
        if (i % 4 == 0) {
            const auto ordinal = static_cast<std::size_t>(i / 4);
            active_ = id_for(period_ == 0 ? ordinal : ordinal % static_cast<std::size_t>(period_));
            const auto na = std::numeric_limits<double>::quiet_NaN();
            strategy_entry(active_, true, na, na, 1.0);
        } else if (i % 4 == 1) {
            strategy_close(active_);
        }
    }
private:
    int period_;
    std::string active_;
};

std::vector<pineforge::Bar> chart() {
    std::vector<pineforge::Bar> bars(256);
    for (std::size_t i = 0; i < bars.size(); ++i)
        bars[i] = {100.0, 101.0, 99.0, 100.0, 1000.0,
                   1736121600000LL + static_cast<std::int64_t>(i) * 60000};
    return bars;
}

int old_slot_count(const Adapter& adapter);

class SlotCountHost final : public pineforge::source::PineStrategyHost {
public:
    static constexpr int kEntries = 256;

    SlotCountHost() {
        pineforge::source::PineStrategyConfig config;
        config.initial_capital = 1e12;
        config.margin_long = config.margin_short = 0.0;
        config.pyramiding = 5000;
        configure_pine_strategy(config);
        set_syminfo_timezone("UTC");
        set_syminfo_session("24x7");
        set_syminfo_mintick(0.25);
        adapter_.fixture_verify_source_entry_slots(true);
    }

    void on_source_bar(const pineforge::Bar&) override {
        const auto i = pine_bar_index();
        if (i < kEntries) {
            const auto na = std::numeric_limits<double>::quiet_NaN();
            strategy_entry("slot-" + std::to_string(i), true, na, na, 1.0);
        }
        auto& table = adapter_.*access(Cohorts{});
        const auto before = adapter_.cohort_table_work();
        const auto allocations = global_allocation::allocations;
        global_allocation::refusing = true;
        last_slots = adapter_.source_entry_slot_count();
        global_allocation::refusing = false;
        const auto after = adapter_.cohort_table_work();
        require(after.resident_rows_visited - before.resident_rows_visited
                    == table.resident_size(),
                "slot oracle visits each resident row exactly once");
        require(after.find_resident_probes == before.find_resident_probes,
                "slot oracle does no per-id lookups");
        require(global_allocation::allocations == allocations,
                "slot oracle allocates nothing");
        // Exercise the production decision path too, with the fixture oracle
        // disabled. Debug's independent assertion uses uninstrumented rows.
        adapter_.fixture_verify_source_entry_slots(false);
        const auto production_before = adapter_.cohort_table_work();
        const auto production_allocations = global_allocation::allocations;
        global_allocation::refusing = true;
        const auto production_slots = adapter_.source_entry_slot_count();
        global_allocation::refusing = false;
        const auto production_after = adapter_.cohort_table_work();
        require(production_slots == last_slots
                && production_slots == static_cast<int>(adapter_.*access(CohortSlots{}))
                && production_slots == static_cast<int>(table.resident_opened_slot_count()),
                "production slot result equals the maintained count and independent sum");
        require(production_after.resident_rows_visited == production_before.resident_rows_visited
                && production_after.iterated_rows == production_before.iterated_rows
                && production_after.held_rows_visited == production_before.held_rows_visited,
                "production slot read visits no resident, generic or held rows");
        require(global_allocation::allocations == production_allocations,
                "production slot read allocates nothing");
        adapter_.fixture_verify_source_entry_slots(true);
        if (i == kEntries) checked_slots = true;
    }

    const Adapter& adapter() const { return adapter_; }
    int last_slots = 0;
    bool checked_slots = false;
};

void slot_count_one_pass_and_no_allocation() {
    auto bars = chart();
    bars.resize(static_cast<std::size_t>(SlotCountHost::kEntries + 1), bars.back());
    for (std::size_t i = 0; i < bars.size(); ++i)
        bars[i].timestamp = 1736121600000LL + static_cast<std::int64_t>(i) * 60000;
    SlotCountHost host;
    host.run(bars.data(), static_cast<int>(bars.size()), "1", "1");
    require(host.last_error().empty(), host.last_error().c_str());
    require(host.checked_slots && host.last_slots == SlotCountHost::kEntries,
            "production openings populate the maintained slot counter");
    require(old_slot_count(host.adapter()) == SlotCountHost::kEntries,
            "maintained slot counter equals the independent opening sum");
}

void real_host_recurrence() {
    const auto bars = chart();
    const auto expected_trades = static_cast<int>(bars.size() / 4);
    for (const int period : {0, 1, 31}) {
        IdHost host(period);
        host.run(bars.data(), static_cast<int>(bars.size()), "1", "1", false, 4,
                 pineforge::MagnifierDistribution::ENDPOINTS);
        require(host.last_error().empty(), host.last_error().c_str());
        require(host.trade_count() == expected_trades, "public host did not complete its trade population");
        for (int i = 0; i < host.trade_count(); ++i) {
            const auto& trade = host.get_trade(i);
            const auto id = id_for(static_cast<std::size_t>(period == 0 ? i : i % period));
            require(trade.entry_id == id && trade.qty == 1.0 && !trade.open_at_end,
                    "public host changed recurring id or lot identity");
        }
        // Reset the same adapter and its files, rather than only constructing
        // fresh hosts; the second run must not see old handles or rank keys.
        host.run(bars.data(), static_cast<int>(bars.size()), "1", "1", false, 4,
                 pineforge::MagnifierDistribution::ENDPOINTS);
        require(host.last_error().empty() && host.trade_count() == expected_trades, "id-table run reset");
    }
}

struct TemporaryDirectorySetting {
    bool was_set = std::getenv("TMPDIR") != nullptr;
    std::string previous = was_set ? std::getenv("TMPDIR") : "";
    ~TemporaryDirectorySetting() {
        if (was_set) ::setenv("TMPDIR", previous.c_str(), 1);
        else ::unsetenv("TMPDIR");
    }
};

void real_host_io_failure() {
    const auto bars = chart();
    IdHost host(0);
    TemporaryDirectorySetting saved;
    // /dev/null is a file: a scratch child of it cannot be created on either
    // target platform, independent of the run uid's directory permissions.
    require(::setenv("TMPDIR", "/dev/null/pineforge-id-test", 1) == 0, "set bad scratch directory");
    host.run(bars.data(), static_cast<int>(bars.size()), "1", "1", false, 4,
             pineforge::MagnifierDistribution::ENDPOINTS);
    require(!host.last_error().empty(), "disk failure did not fail the run");
    require(std::string(pineforge::run_failure_code_of(host)) == "scratch_io_failed",
            "scratch failure lost its typed code at the run catcher");
}

int old_slot_count(const Adapter& adapter) {
    const auto& table = adapter.*access(Cohorts{});
    std::size_t sum = 0;
    for (const auto& id : table.resident_ids()) {
        const auto found = table.find_resident(id);
        if (found != table.end()) sum += found->second.opened.size();
    }
    return sum > static_cast<std::size_t>(std::numeric_limits<int>::max())
        ? std::numeric_limits<int>::max() : static_cast<int>(sum);
}

// Field serialization excludes addresses and C++ padding. Every published
// scalar/string is compared bit-for-bit, including NaNs and signed zeroes.
template<class T> void append_bits(std::string& out, const T& value) {
    out.append(reinterpret_cast<const char*>(&value), sizeof value);
}
void append_text(std::string& out, const std::string& text) {
    append_bits(out, text.size());
    out += text;
}
std::string report_image(const pineforge::BacktestEngine& host) {
    pineforge::ReportC report{};
    host.fill_report(&report);
    struct Release {
        pineforge::ReportC& report;
        ~Release() { pineforge::BacktestEngine::free_report(&report); }
    } release{report};
    std::string out;
    append_bits(out, report.total_trades); append_bits(out, report.trades_len);
    append_bits(out, report.net_profit);
    append_bits(out, report.input_bars_processed); append_bits(out, report.script_bars_processed);
    append_bits(out, report.security_feeds_total);
    append_bits(out, report.security_eval_complete_total);
    append_bits(out, report.security_eval_partial_total);
    append_bits(out, report.magnifier_sub_bars_total); append_bits(out, report.magnifier_sample_ticks_total);
    append_bits(out, report.input_tf_seconds); append_bits(out, report.script_tf_seconds);
    append_bits(out, report.script_tf_ratio); append_bits(out, report.needs_aggregation);
    append_bits(out, report.bar_magnifier_enabled);
    append_bits(out, report.metrics);
    for (int i = 0; i < report.trades_len; ++i) {
        const auto& t = report.trades[i];
        append_bits(out, t.entry_time); append_bits(out, t.exit_time);
        append_bits(out, t.entry_price); append_bits(out, t.exit_price);
        append_bits(out, t.pnl); append_bits(out, t.pnl_pct); append_bits(out, t.is_long);
        append_bits(out, t.max_runup); append_bits(out, t.max_drawdown);
        append_bits(out, t.qty); append_bits(out, t.commission);
        append_bits(out, t.entry_bar_index); append_bits(out, t.exit_bar_index);
        append_bits(out, t.open_at_end);
    }
    append_bits(out, report.security_diag_len);
    for (int i = 0; i < report.security_diag_len; ++i) {
        const auto& d = report.security_diag[i];
        append_bits(out, d.sec_id); append_bits(out, d.feed_count);
        append_bits(out, d.eval_complete_count); append_bits(out, d.eval_partial_count);
    }
    append_bits(out, report.trace_names_len);
    for (int i = 0; i < report.trace_names_len; ++i) append_text(out, report.trace_names[i]);
    append_bits(out, report.trace_len);
    for (int i = 0; i < report.trace_len; ++i) {
        append_bits(out, report.trace[i].timestamp);
        append_bits(out, report.trace[i].bar_index);
        append_bits(out, report.trace[i].name_id);
        append_bits(out, report.trace[i].value);
    }
    append_bits(out, report.equity_curve_len);
    for (std::int64_t i = 0; i < report.equity_curve_len; ++i)
        append_bits(out, report.equity_curve[i]);
    append_bits(out, report.broker_state_hash_len);
    for (std::int64_t i = 0; i < report.broker_state_hash_len; ++i)
        append_bits(out, report.broker_state_hash[i]);
    append_bits(out, host.report_trade_count());
    for (int i = 0; i < host.report_trade_count(); ++i) {
        const auto t = host.get_report_trade(i);
        append_text(out, t.entry_id); append_text(out, t.entry_comment);
        append_text(out, t.exit_comment); append_text(out, t.exit_id);
        append_bits(out, t.entry_incarnation); append_bits(out, t.exit_from_bracket);
        append_bits(out, t.close_cause);
    }
    return out;
}

class SpillHost final : public pineforge::source::PineStrategyHost {
public:
    SpillHost(std::size_t interval, bool on_close, bool coof) : interval_(interval) {
        pineforge::source::PineStrategyConfig config;
        config.process_orders_on_close = on_close;
        config.calc_on_order_fills = coof;
        config.margin_long = config.margin_short = 0.0;
        config.pyramiding = 4;
        configure_pine_strategy(config);
        set_broker_state_hash_recording(true);
        set_syminfo_timezone("UTC");
        set_syminfo_session("24x7");
        set_syminfo_mintick(0.25);
        adapter_.fixture_verify_source_entry_slots(true);
    }
    void on_source_bar(const pineforge::Bar&) override {
        auto& table = adapter_.*access(Cohorts{});
        table.fixture_implicit_spill_interval(interval_);
        std::vector<std::string> ids, rows;
        for (const auto& id : table.resident_ids()) ids.push_back(id);
        for (const auto& row : table.resident_rows()) rows.push_back(row.first);
        require(ids == rows, "real-order walks retain the old visiting order on every callback");
        require(table.held_rows_exact(), "real orders maintain the exact held set in Release too");
        require(adapter_.source_entry_slot_count() == old_slot_count(adapter_),
                "real orders preserve the per-bar slot oracle");
        const auto i = pine_bar_index();
        if (i == previous_) return;
        previous_ = i;
        const auto na = std::numeric_limits<double>::quiet_NaN();
        switch (i % 12) {
        case 0: strategy_entry("A", true, na, na, 2.0); break;
        case 1: strategy_exit("partial", "A", 100.0, na, na, na, na, 50.0); break;
        case 3: strategy_entry("B", false, na, na, 2.0); break;
        case 5: strategy_close_all(); break;
        case 7: strategy_entry("C", true, na, na, 2.0); break;
        case 8: strategy_exit("stop", "C", na, 99.0); break;
        case 10: strategy_close_all(); break;
        default: break;
        }
    }
    const auto& work() const { return adapter_.cohort_table_work(); }
private:
    std::size_t interval_;
    int previous_ = -1;
};

void real_orders_spill_differential() {
    const auto bars = chart();
    for (const bool on_close : {false, true}) {
        for (const bool coof : {false, true}) {
            SpillHost eager(1, on_close, coof);
            eager.run(bars.data(), static_cast<int>(bars.size()), "1", "1");
            require(eager.last_error().empty(), eager.last_error().c_str());
            bool partial = false, long_side = false, short_side = false;
            for (int i = 0; i < eager.trade_count(); ++i) {
                const auto t = eager.get_trade(i);
                partial = partial || (t.entry_id == "A" && t.qty == 1.0);
                long_side = long_side || t.is_long;
                short_side = short_side || !t.is_long;
            }
            require(partial && long_side && short_side,
                    "differential script really fills, partially exits and reverses");
            require(eager.work().rows_spilled > 0 && eager.work().rows_restored > 0,
                    "differential script really spills and restores retired cohorts");
            const auto reference = report_image(eager);
            for (const std::size_t interval : {0U, 1U, 7U, 1000U}) {
                SpillHost candidate(interval, on_close, coof);
                candidate.run(bars.data(), static_cast<int>(bars.size()), "1", "1");
                require(candidate.last_error().empty(), candidate.last_error().c_str());
                require(report_image(candidate) == reference,
                        "implicit spill schedule changes a trade or final report byte");
            }
        }
    }
}

class CloseSelectionHost final : public pineforge::source::PineStrategyHost {
public:
    CloseSelectionHost(int population, bool precheck) : population_(population) {
        pineforge::source::PineStrategyConfig config;
        config.initial_capital = 1e12;
        config.margin_long = config.margin_short = 0.0;
        config.pyramiding = 4096;
        config.process_orders_on_close = true;
        configure_pine_strategy(config);
        set_broker_state_hash_recording(true);
        set_syminfo_timezone("UTC");
        set_syminfo_session("24x7");
        set_syminfo_mintick(0.25);
        adapter_.fixture_bracket_remove_precheck(precheck);
    }
    void on_source_bar(const pineforge::Bar&) override {
        const auto i = pine_bar_index();
        const auto na = std::numeric_limits<double>::quiet_NaN();
        if (i < population_) {
            const auto id = "close-owner-" + std::to_string(i);
            strategy_entry(id, true, na, na, 1.0);
            strategy_exit("close-exit-" + std::to_string(i), id, na, 50.0);
        } else if (i == population_) {
            require(trade_count() == 0 && physical_position().signed_units == population_,
                    "one opening per bar holds N units and none of the N exits filled");
            require(native_working_requests().size() == static_cast<std::size_t>(population_),
                    "the close starts with N real working exit legs");
            // Arm before requesting the close and stop at its post-Applied
            // observer, covering this close's admission and settlement.
            auto& scratch = pineforge::source::detail::exit_scratch(adapter_);
            scratch.observation = this;
            scratch.observe_lookup = [](void* opaque, std::size_t) noexcept {
                ++static_cast<CloseSelectionHost*>(opaque)->close_lookups;
            };
            armed_ = true;
            strategy_close_all();
        }
    }
    std::uint64_t close_lookups = 0;
    int close_events = 0;
protected:
    void on_native_applied_event(const no::ExecutionAppliedEvent& event,
                                const pineforge::NativeDecisionContext&) override {
        if (!armed_) return;
        require(event.closed_units == population_ && event.opened_units == 0.0,
                "the measured event is the full close of the populated book");
        ++close_events;
        armed_ = false;
        auto& scratch = pineforge::source::detail::exit_scratch(adapter_);
        scratch.observe_lookup = nullptr;
        scratch.observation = nullptr;
        require(native_working_requests().empty(), "the close cancels every pending exit");
    }
private:
    int population_;
    bool armed_ = false;
};

void close_selection_lookup_growth_and_equivalence() {
    std::uint64_t checked[2]{}, unchecked[2]{};
    for (int sample = 0; sample < 2; ++sample) {
        const int n = sample == 0 ? 128 : 256;
        auto bars = chart();
        bars.resize(static_cast<std::size_t>(n + 2), bars.back());
        for (std::size_t i = 0; i < bars.size(); ++i)
            bars[i].timestamp = 1736121600000LL + static_cast<std::int64_t>(i) * 60000;
        CloseSelectionHost candidate(n, true), reference(n, false);
        for (auto* host : {&candidate, &reference}) {
            host->run(bars.data(), static_cast<int>(bars.size()), "1", "1");
            require(host->last_error().empty(), host->last_error().c_str());
            require(host->close_events == 1 && host->trade_count() == n,
                    "close-selection witness executes one close and closes every opening");
            for (int i = 0; i < host->trade_count(); ++i) {
                const auto& trade = host->get_trade(i);
                require(trade.qty == 1.0 && !trade.open_at_end && !trade.exit_from_bracket,
                        "all exits stay pending until the script close");
            }
        }
        require(report_image(candidate) == report_image(reference)
                && candidate.broker_state_hash_projection() == reference.broker_state_hash_projection(),
                "precheck preserves every trade/report byte, recorded hash and final source hash");
        checked[sample] = candidate.close_lookups;
        unchecked[sample] = reference.close_lookups;
        require(checked[sample] > 0 && unchecked[sample] >=
                    static_cast<std::uint64_t>(n) * static_cast<std::uint64_t>(n + 1) / 2,
                "the existing find_key hook observes the old triangular family scans");
        std::printf("CR347 close N=%d find_key=%llu precheck_disabled_find_key=%llu\n", n,
            static_cast<unsigned long long>(checked[sample]),
            static_cast<unsigned long long>(unchecked[sample]));
    }
    // Red on the old remove_selected: each owner seeks every remaining
    // family's hot member, N(N+1)/2 seeks. This hook is called by find_key;
    // all legs are already gone before the later ended_sources.contains pass.
    require(checked[1] < 3 * checked[0], "doubling pending exits must take less than 3x close key seeks");
    require(unchecked[0] > checked[0] && unchecked[1] > checked[1],
            "fixture switch actually disables the precheck");
    require(unchecked[1] > 3 * unchecked[0], "the disabled fixture retains the quadratic witness");
}

class DenseHost final : public pineforge::source::PineStrategyHost {
public:
    explicit DenseHost(bool jailed = false, bool cap_probe = false, int period = 0)
        : cap_probe_(cap_probe), period_(period) {
        pineforge::source::PineStrategyConfig config;
        config.initial_capital = 1e12;
        config.margin_long = config.margin_short = 0.0;
        config.pyramiding = 5000;
        configure_pine_strategy(config);
        set_syminfo_mintick(0.25);
        set_syminfo_timezone("UTC");
        set_syminfo_session("24x7");
        if (jailed) {
            pf_scratch_tables_v1 descriptor{};
            descriptor.struct_size = static_cast<std::uint32_t>(sizeof descriptor);
            descriptor.version = 1;
            descriptor.backend = PF_SCRATCH_TABLE_BOUNDED_MEMORY;
            require(set_scratch_tables_v1(&descriptor) == 0, "select jailed cohort table");
        }
    }
    void on_source_bar(const pineforge::Bar&) override {
        const auto i = pine_bar_index();
        const auto na = std::numeric_limits<double>::quiet_NaN();
        auto& table = adapter_.*access(Cohorts{});
        if (cap_probe_ && i == 4096) {
            const auto logical = table.size();
            const auto resident = table.resident_size();
            const auto slab = scratch_tables_for_run();
            const auto& region = slab->region(pineforge::detail::ScratchResource::cohort);
            const auto extent = region.logical_extent;
            std::vector<unsigned char> before(static_cast<std::size_t>(extent));
            slab->read_at(region, 0, before.data(), before.size());
            const auto last_handle = table.find_resident("dense-4095")->second.handle;
            const auto before_hash = broker_state_hash_projection();
            try { (adapter_.*access(CohortFor{}))("refused-before-kernel"); }
            catch (const pineforge::detail::ScratchCapacityError& error) {
                refused = std::string(error.what()).find("4,096") != std::string::npos;
                const auto failure = pineforge::classify_run_failure(error);
                require(failure.code == pineforge::RunFailureCode::scratch_capacity_exceeded
                        && failure.args && *failure.args == "{\"resource\":\"cohort\"}",
                        "adapter cap refusal retains typed resource");
            }
            require(refused && broker_state_hash_projection() == before_hash
                    && table.size() == logical && table.resident_size() == resident,
                    "cap ticket refuses before source/kernel state mutation");
            std::vector<unsigned char> after(before.size());
            slab->read_at(region, 0, after.data(), after.size());
            require(before == after && region.logical_extent == extent, "cap preserves index bytes");
            require(cohort_open().value == last_handle.value + 1,
                    "refused cohort did not consume a kernel cohort handle");
            return;
        }
        const auto slot = period_ == 0 ? i : i % period_;
        const auto id = "dense-" + std::to_string(slot);
        const auto exit_id = "exit-" + std::to_string(slot);
        if (period_ != 0 && i >= period_) {
            strategy_cancel(exit_id);
            strategy_cancel(id);
        }
        strategy_entry(id, true, 1.0, na, 1.0); // Never fills on this chart.
        // The cap witness needs 4,096 real pending origins, not exit legs.
        if (!cap_probe_) strategy_exit(exit_id, id, na, 50.0);
        if (i == 127) at_128 = adapter_.pending_scan_work();
        if (i == 255) at_256 = adapter_.pending_scan_work();
    }
    std::uint64_t cohort_operations() const {
        const auto& w = adapter_.cohort_table_work();
        return w.rows_visited_by_spill + w.find_resident_probes + w.resident_rows_visited
            + w.held_rows_visited + w.iterated_rows + w.rows_visited_by_capacity
            + w.rows_spilled + w.rows_restored;
    }
    std::uint64_t cohort_walk_operations() const {
        const auto& w = adapter_.cohort_table_work();
        return w.rows_visited_by_spill + w.find_resident_probes + w.resident_rows_visited
            + w.held_rows_visited + w.rows_visited_by_capacity + w.rows_spilled + w.rows_restored;
    }
    Adapter::PendingScanWork at_128{}, at_256{};
    const Adapter& adapter() const { return adapter_; }
    bool refused = false;
private:
    bool cap_probe_;
    int period_;
};

void print_slice(const char* slice, const Adapter::PendingScanWork& work) {
    const auto print = [&](const char* name, const Adapter::PendingScanCounter& c) {
        std::printf("CR347 %s %s calls=%llu live_iterations=%llu placement_finds=%llu "
                    "lot_iterations=%llu max_live=%llu max_placement=%llu max_brackets=%llu "
                    "max_resident=%llu max_logical=%llu\n", slice, name,
            static_cast<unsigned long long>(c.calls),
            static_cast<unsigned long long>(c.live_handle_iterations),
            static_cast<unsigned long long>(c.placement_find_calls),
            static_cast<unsigned long long>(c.open_lot_iterations),
            static_cast<unsigned long long>(c.max_live_handles),
            static_cast<unsigned long long>(c.max_placement),
            static_cast<unsigned long long>(c.max_pending_bracket_legs),
            static_cast<unsigned long long>(c.max_resident_cohorts),
            static_cast<unsigned long long>(c.max_logical_cohorts));
    };
    print("priced_entry_order_working", work.priced_entry);
    print("entry_order_working", work.entry);
    print("standing_exit", work.standing_exit);
    print("open_lot_of", work.open_lot);
}

void rolling_modulo_pending_population() {
    constexpr int period = 64;
    auto bars = chart();
    bars.resize(512, bars.back()); // Eight complete wraps of each modulo slot.
    for (std::size_t i = 0; i < bars.size(); ++i)
        bars[i].timestamp = 1736121600000LL + static_cast<std::int64_t>(i) * 60000;
    for (const bool jailed : {false, true}) {
        DenseHost rolling(jailed, false, period);
        rolling.run(bars.data(), static_cast<int>(bars.size()), "1", "1");
        require(rolling.last_error().empty(), rolling.last_error().c_str());
        const auto& work = rolling.adapter().pending_scan_work();
        require(rolling.trade_count() == 0 && work.entry.max_logical_cohorts == period
                && work.entry.max_resident_cohorts <= period,
                "rolling modulo script reuses a fixed cohort population");
        // Each modulo slot contributes one entry id and one exit id, and this
        // script can keep at most one deferred bracket leg for that slot.
        // The four-row slack covers startup and the terminal cancellation
        // boundary. Live handles are the entry/exit pair plus that same small
        // boundary slack.
        constexpr std::uint64_t small = 4;
        const auto bracket_bound = static_cast<std::uint64_t>(period) + small;
        const auto placement_bound = static_cast<std::uint64_t>(period)
            + static_cast<std::uint64_t>(period) + bracket_bound;
        const auto maximum = [](auto selector, const Adapter::PendingScanWork& value) {
            return std::max({selector(value.priced_entry), selector(value.entry),
                             selector(value.standing_exit), selector(value.open_lot)});
        };
        const auto max_live_handles = maximum(
            [](const auto& counter) { return counter.max_live_handles; }, work);
        const auto max_placement = maximum(
            [](const auto& counter) { return counter.max_placement; }, work);
        const auto max_brackets = maximum(
            [](const auto& counter) { return counter.max_pending_bracket_legs; }, work);
        require(max_live_handles <= static_cast<std::uint64_t>(2 * period) + small
                && max_brackets <= bracket_bound && max_placement <= placement_bound,
                "rolling modulo bounds live handles, placement rows and deferred brackets");
        print_slice(jailed ? "rolling-64-jailed" : "rolling-64-unjailed", work);
        // Pending-order maxima are deliberately logged too: the cohort cap
        // alone is not a proof that placement/order memory is bounded.
    }
}

void dense_operation_growth_and_instrumentation(bool jailed) {
    std::uint64_t operations[2]{}, walk_operations[2]{};
    for (int sample = 0; sample < 2; ++sample) {
        const auto n = sample == 0 ? 256 : 512;
        auto bars = chart();
        bars.resize(static_cast<std::size_t>(n), bars.back());
        for (int i = 0; i < n; ++i) bars[i].timestamp = 1736121600000LL + i * 60000LL;
        DenseHost host(jailed);
        host.run(bars.data(), n, "1", "1");
        require(host.last_error().empty(), host.last_error().c_str());
        require(host.trade_count() == 0, "dense entries really remain pending");
        const auto& table = host.adapter().*access(Cohorts{});
        require(table.resident_size() == static_cast<std::size_t>(n)
                && table.held_size() == 0 && table.held_rows_exact(),
                "dense placed-never-filled cohorts pin residents without holding openings or units");
        require(host.adapter().cohort_table_work().resident_rows_visited == 0
                && host.adapter().cohort_table_work().held_rows_visited == 0,
                "production decisions visit no rows when H is zero despite growing pending R");
        operations[sample] = host.cohort_operations();
        walk_operations[sample] = host.cohort_walk_operations();
        require(operations[sample] > 0, "cohort work instrumentation is active");
        print_slice(jailed ? "N=128-jailed" : "N=128-unjailed", host.at_128);
        print_slice(jailed ? "N=256-jailed" : "N=256-unjailed", host.at_256);
        auto second_slice = host.at_256;
        const auto subtract = [](auto& after, const auto& before) {
            after.calls -= before.calls;
            after.live_handle_iterations -= before.live_handle_iterations;
            after.placement_find_calls -= before.placement_find_calls;
            after.open_lot_iterations -= before.open_lot_iterations;
        };
        subtract(second_slice.priced_entry, host.at_128.priced_entry);
        subtract(second_slice.entry, host.at_128.entry);
        subtract(second_slice.standing_exit, host.at_128.standing_exit);
        subtract(second_slice.open_lot, host.at_128.open_lot);
        print_slice(jailed ? "N=129..256-jailed" : "N=129..256-unjailed", second_slice);
        const auto monotone = [](const auto& a, const auto& b) {
            require(b.calls >= a.calls && b.live_handle_iterations >= a.live_handle_iterations
                    && a.placement_find_calls == a.live_handle_iterations
                    && b.placement_find_calls == b.live_handle_iterations
                    && b.max_live_handles >= a.max_live_handles
                    && b.max_placement >= a.max_placement
                    && b.max_pending_bracket_legs >= a.max_pending_bracket_legs
                    && b.max_resident_cohorts >= a.max_resident_cohorts
                    && b.max_logical_cohorts >= a.max_logical_cohorts,
                    "each actual pending-loop counter is monotone, including short-circuited loops");
        };
        monotone(host.at_128.priced_entry, host.at_256.priced_entry);
        monotone(host.at_128.entry, host.at_256.entry);
        monotone(host.at_128.standing_exit, host.at_256.standing_exit);
        monotone(host.at_128.open_lot, host.at_256.open_lot);
        const auto scanned = [](const Adapter::PendingScanWork& work) {
            return work.priced_entry.live_handle_iterations + work.entry.live_handle_iterations
                + work.standing_exit.live_handle_iterations;
        };
        require(scanned(host.at_128) > 0 && scanned(host.at_256) > scanned(host.at_128)
                && host.at_128.entry.calls > 0 && host.at_256.entry.calls > host.at_128.entry.calls,
                "the dense script itself exercises nonzero, growing pending scans");
        require(host.at_256.open_lot.live_handle_iterations == 0
                && host.at_256.open_lot.placement_find_calls == 0,
                "open_lot_of has a native-lot walk, no live-handle or placement loop");
        const auto allocations = global_allocation::allocations;
        global_allocation::refusing = true;
        const auto heap = host.adapter().cohort_heap_estimate();
        global_allocation::refusing = false;
        require(global_allocation::allocations == allocations && heap.resident_rows >= 256
                && heap.total_bytes >= heap.row_bytes
                && heap.total_bytes >= heap.opened_bytes + heap.origin_bytes
                && heap.bytes_per_row == heap.total_bytes / heap.resident_rows,
                "heap estimate is a nonallocating function of the actual resident rows");
        std::printf("CR347 profile=%s N=%d cohort_operations=%llu resident=%zu heap_estimate=%zu "
                    "bytes_per_row=%zu row=%zu key=%zu origins=%zu opened=%zu units=%zu directory=%zu\n",
            jailed ? "jailed" : "unjailed", n,
            static_cast<unsigned long long>(operations[sample]), heap.resident_rows,
            heap.total_bytes, heap.bytes_per_row, heap.row_bytes, heap.key_bytes,
            heap.origin_bytes, heap.opened_bytes, heap.live_unit_bytes, heap.directory_bytes);
    }
    // These are the walks fixed in CR-347 rounds 4/5; they deliberately exclude the
    // retire_origin_prefixes Iterator pass below.
    require(walk_operations[1] * 2 <= walk_operations[0] * 5,
            "doubling dense pending cohorts exceeds 2.5x fixed cohort walks");
    // EXPECTED_RED_UNTIL_RETIRE_DIRTY_SET: iterated_rows measures the known
    // Theta(R) retire pass. Print the full ratio and keep it unasserted until
    // that pass has a dirty-set implementation.
    std::printf("CR347 profile=%s dense_full_ratio=%llu/%llu fixed_walk_ratio=%llu/%llu "
                "iterated_rows_N=%llu iterated_rows_2N=%llu\n",
        jailed ? "jailed" : "unjailed",
        static_cast<unsigned long long>(operations[1]),
        static_cast<unsigned long long>(operations[0]),
        static_cast<unsigned long long>(walk_operations[1]),
        static_cast<unsigned long long>(walk_operations[0]),
        static_cast<unsigned long long>(operations[0] - walk_operations[0]),
        static_cast<unsigned long long>(operations[1] - walk_operations[1]));
}

void adapter_cap_rollback() {
    auto bars = chart();
    bars.resize(4097, bars.back());
    for (std::size_t i = 0; i < bars.size(); ++i)
        bars[i].timestamp = 1736121600000LL + static_cast<std::int64_t>(i) * 60000;
    DenseHost capped(true, true);
    capped.run(bars.data(), static_cast<int>(bars.size()), "1", "1");
    require(capped.last_error().empty() && capped.refused, "adapter cap rollback witness ran");
}
} // namespace

int main(int argc, char** argv) {
    try {
        // Lab-only targeted entry points keep fail-before runs independent
        // of the slower full adapter storage/cap population.
        if (argc == 2 && std::string(argv[1]) == "--close-selection") {
            close_selection_lookup_growth_and_equivalence();
            std::puts("adapter close selection: OK");
            return 0;
        }
        if (argc == 2 && std::string(argv[1]) == "--held-membership") {
            held_membership_on_every_fuzz_mutation();
            restored_units_join_the_held_set();
            std::puts("adapter held membership: OK");
            return 0;
        }
        if (argc != 1) {
            std::fputs("expected --close-selection, --held-membership, or no arguments\n", stderr);
            return 2;
        }
        replay_recurring_ids();
        pinned_lookup_and_resident_reads();
        held_membership_on_every_fuzz_mutation();
        restored_units_join_the_held_set();
        q9_difference_of_differences();
        real_host_recurrence();
        real_host_io_failure();
        slot_count_one_pass_and_no_allocation();
        real_orders_spill_differential();
        close_selection_lookup_growth_and_equivalence();
        dense_operation_growth_and_instrumentation(false);
        dense_operation_growth_and_instrumentation(true);
        adapter_cap_rollback();
        rolling_modulo_pending_population();
        std::puts("adapter id storage: OK");
    } catch (const std::exception& error) {
        global_allocation::recording = false;
        global_allocation::refusing = false;
        std::fprintf(stderr, "FAIL: %s\n", error.what());
        return 1;
    }
}
