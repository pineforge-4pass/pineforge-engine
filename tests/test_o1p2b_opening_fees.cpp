// P22: run this unit against 6cfcff0696 before applying the production hunk.
// The selected-exposure allocation assertion must be RED there. The oracle
// below owns a vector deliberately; it is never part of a production path.
#include <pineforge/source/pine_adapter.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <memory>
#include <new>
#include <string>
#include <vector>

namespace allocations {
bool enabled = false;
std::size_t calls = 0;
void* get(std::size_t bytes) {
    if (enabled) ++calls;
    if (void* result = std::malloc(bytes ? bytes : 1)) return result;
    throw std::bad_alloc();
}
}

void* operator new(std::size_t bytes) { return allocations::get(bytes); }
void* operator new[](std::size_t bytes) { return allocations::get(bytes); }
void operator delete(void* value) noexcept { std::free(value); }
void operator delete[](void* value) noexcept { std::free(value); }
void operator delete(void* value, std::size_t) noexcept { std::free(value); }
void operator delete[](void* value, std::size_t) noexcept { std::free(value); }

namespace {
using namespace pineforge;
using Adapter = source::PineExecutionAdapter;
using Fee = source::OpenEntryFeeFact;
namespace no = pineforge::native_order;

int checks = 0, failures = 0;
#define CHECK(value) do { ++checks; if (!(value)) { ++failures; \
    std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #value); } } while (false)

template<class Tag, auto Member> struct Access {
    friend auto member(Tag) { return Member; }
};
struct Fees { friend auto member(Fees); };
template struct Access<Fees, &Adapter::open_entry_fees_>;
struct Record { friend auto member(Record); };
template struct Access<Record, &Adapter::record_opening_fee>;
struct Consume { friend auto member(Consume); };
template struct Access<Consume, &Adapter::consume_opening_fees>;

bool bits(double left, double right) {
    return std::memcmp(&left, &right, sizeof(left)) == 0;
}

// Independent accounting model: select lots into a test-only eligibility
// mask, calculate cumulative FIFO draws, then filter exhausted lots in a
// separate pass. It does not invoke production consumption or hash code.
void reference_consume(std::vector<Fee>& rows, const no::ExecutionAppliedEvent& event,
                       const source::SourceId* id) {
    if (!(event.closed_units > 0.0) || !std::isfinite(event.closed_units)) return;
    std::vector<bool> eligible(rows.size(), true);
    for (std::size_t i = 0; i < rows.size(); ++i) {
        const auto origin = rows[i].opening.incarnation;
        if (const auto* single = std::get_if<execution::OpeningExposure>(&event.scope)) {
            eligible[i] = origin == single->incarnation;
        } else if (const auto* many = std::get_if<no::SelectedExposure>(&event.scope)) {
            if (!many->incarnations.empty())
                eligible[i] = std::count(many->incarnations.begin(), many->incarnations.end(), origin) != 0;
        }
        eligible[i] = eligible[i] && (!id || rows[i].source_id == *id)
            && rows[i].units > 0.0 && std::isfinite(rows[i].units);
    }
    double available = event.closed_units;
    std::vector<bool> exhausted(rows.size(), false);
    for (std::size_t i = 0; i < rows.size() && available > 0.0; ++i) {
        if (!eligible[i]) continue;
        const double draw = std::min(available, rows[i].units);
        const double share = draw / rows[i].units;
        const double paid = rows[i].nonpercent_fee * share;
        rows[i].nonpercent_fee -= paid;
        rows[i].units -= draw;
        available -= draw;
        exhausted[i] = !(rows[i].units > 0.0);
    }
    std::vector<Fee> survivors;
    for (std::size_t i = 0; i < rows.size(); ++i)
        if (!exhausted[i]) survivors.push_back(rows[i]);
    rows = std::move(survivors);
}

std::uint64_t fee_leaf(const std::vector<Fee>& rows) {
    BrokerStateHashSink sink;
    sink.u(rows.size());
    for (const auto& row : rows) {
        sink.s(row.opening.run.session_key);
        sink.u(0); // Existing source hash normalises the native run number.
        sink.u(row.opening.incarnation);
        sink.s(row.source_id);
        sink.d(row.units);
        sink.d(row.nonpercent_fee);
    }
    return sink.h;
}

void same_rows(const Adapter& adapter, const std::vector<Fee>& expected) {
    const auto& actual = adapter.*member(Fees{});
    CHECK(actual.size() == expected.size());
    if (actual.size() != expected.size()) return;
    for (std::size_t i = 0; i < expected.size(); ++i) {
        CHECK(actual[i].opening == expected[i].opening);
        CHECK(actual[i].source_id == expected[i].source_id);
        CHECK(bits(actual[i].units, expected[i].units));
        CHECK(bits(actual[i].nonpercent_fee, expected[i].nonpercent_fee));
    }
    CHECK(fee_leaf(actual) == fee_leaf(expected));
}

void consume(Adapter& adapter, std::vector<Fee>& expected,
             const no::ExecutionAppliedEvent& event, const source::SourceId* id = nullptr) {
    reference_consume(expected, event, id);
    allocations::calls = 0;
    allocations::enabled = true;
    (adapter.*member(Consume{}))(event, id);
    allocations::enabled = false;
    CHECK(allocations::calls == 0);
    same_rows(adapter, expected);
}

void selection_and_paid_cost(std::size_t population) {
    Adapter adapter;
    source::PineStrategyConfig config;
    config.commission_type = static_cast<int>(CommissionType::CASH_PER_CONTRACT);
    config.commission_value = 0.37;
    adapter.set_configuration(config);
    std::vector<Fee> expected;
    for (std::size_t i = 0; i < population; ++i) {
        source::PlacementSnapshot placement;
        placement.source_id = i % 3 == 0 ? "A" : "B";
        no::ExecutionAppliedEvent event;
        auto definition = std::make_shared<no::RequestDefinition>();
        definition->handle = {{"o1p2b-fees", 7}, i + 1};
        event.definition = definition;
        event.opened_units = 1.0 + static_cast<double>(i % 7) * 0.25;
        (adapter.*member(Record{}))(placement, event);
        expected.push_back({event.handle(), placement.source_id, event.opened_units,
                            config.commission_value * event.opened_units});
    }
    CHECK((adapter.*member(Fees{})).size() == population);
    same_rows(adapter, expected);

    // Selection order is deliberately the reverse of fee FIFO order; repeated
    // and nonexistent origins must not reorder or duplicate a paid-cost draw.
    no::SelectedExposure selection;
    for (std::size_t i = population; i > 0; --i) {
        if (i % 2) selection.incarnations.push_back(i);
    }
    selection.incarnations.push_back(1);
    selection.incarnations.push_back(population + 100);
    no::ExecutionAppliedEvent close;
    close.scope = std::move(selection);
    close.closed_units = static_cast<double>(population) / 3.0 + 0.125;
    const source::SourceId a = "A";
    consume(adapter, expected, close, &a);

    close.scope = execution::OpeningExposure{population / 2};
    close.closed_units = 0.125;
    consume(adapter, expected, close);
    close.scope = no::SelectedExposure{}; // Empty selection means whole book.
    close.closed_units = 0.375;
    consume(adapter, expected, close);

    close.scope = execution::Book{};
    for (const double invalid : {0.0, -1.0, std::numeric_limits<double>::infinity(),
                                 std::numeric_limits<double>::quiet_NaN()}) {
        close.closed_units = invalid;
        consume(adapter, expected, close);
    }
    close.closed_units = static_cast<double>(population) * 10.0;
    consume(adapter, expected, close);
    CHECK(expected.empty());
}

void reversal_order_fee() {
    Adapter adapter;
    source::PineStrategyConfig config;
    config.commission_type = static_cast<int>(CommissionType::CASH_PER_ORDER);
    config.commission_value = 7.25;
    adapter.set_configuration(config);
    auto definition = std::make_shared<no::RequestDefinition>();
    definition->handle = {{"o1p2b-reversal", 11}, 17};
    no::ExecutionAppliedEvent event;
    event.definition = definition;
    event.opened_units = -3.0;
    event.closed_units = 2.0;
    source::PlacementSnapshot placement;
    placement.source_id = std::string("long\0id", 7);
    (adapter.*member(Record{}))(placement, event);
    std::vector<Fee> expected{{event.handle(), placement.source_id, 3.0, 7.25 * 3.0 / 5.0}};
    same_rows(adapter, expected);
    event.scope = execution::OpeningExposure{17};
    event.closed_units = 0.75;
    consume(adapter, expected, event);
    event.closed_units = 2.25;
    consume(adapter, expected, event);
    CHECK(expected.empty());

    config.commission_type = static_cast<int>(CommissionType::PERCENT);
    adapter.set_configuration(config);
    (adapter.*member(Record{}))(placement, event);
    CHECK((adapter.*member(Fees{})).empty());
}
}

int main() {
    for (const std::size_t count : {std::size_t{16}, std::size_t{257}, std::size_t{4097}})
        selection_and_paid_cost(count);
    reversal_order_fee();
    std::printf("o1p2b opening fees: %d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
