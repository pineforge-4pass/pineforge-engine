// CR-150 L19l: the production resident/disk container and real OriginRoster,
// compared with the old unordered-map + insertion-vector logical state.
#define PINEFORGE_TEST_ALLOCATION_BYTES
#include "global_allocation_replacement.hpp"
#include <pineforge/source/pine_strategy_host.hpp>

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
// Binary keys remain a storage test; successful Pine commands use
// ordinary IDs. The command-door regression separately pins NUL refusal.
std::string host_id_for(std::size_t n) {
    return "census-entry-" + std::to_string(n) + "/tail";
}
no::RequestHandle origin_for(std::uint64_t n) { return {{"disk-cohort-test", 1}, n}; }

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
        current.units[origin.incarnation] = 3.25;
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
    for (auto& row : table) {
        row.second.opened.clear();
        row.second.units.clear();
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
            active_ = host_id_for(period_ == 0 ? ordinal : ordinal % static_cast<std::size_t>(period_));
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
            const auto id = host_id_for(static_cast<std::size_t>(period == 0 ? i : i % period));
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
} // namespace

int main() {
    try {
        replay_recurring_ids();
        pinned_lookup_and_resident_reads();
        q9_difference_of_differences();
        real_host_recurrence();
        real_host_io_failure();
        std::puts("adapter id storage: OK");
    } catch (const std::exception& error) {
        global_allocation::recording = false;
        std::fprintf(stderr, "FAIL: %s\n", error.what());
        return 1;
    }
}
