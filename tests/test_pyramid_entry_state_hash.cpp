#include <pineforge/engine.hpp>
#include <pineforge/source/pine_strategy_host.hpp>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <string>
#include <vector>

// Literal state construction only. No on_bar, run, stream input, strategy
// command, corpus, Pine or grading execution is used by this fixture.
using namespace pineforge;

namespace {
class LiteralBook final : public pineforge::source::PineStrategyHost {
public:
    LiteralBook() {
        position_side_ = PositionSide::LONG;
        position_entry_price_ = 100;
        position_entry_time_ = 1000;
        position_qty_ = 2;
        position_entry_count_ = 1;
        position_open_bar_ = 3;
        position_cycle_seq_ = 1;
        next_position_cycle_seq_ = 2;
        PyramidEntry entry{};
        entry.price = 100;
        entry.time = 1000;
        entry.qty = 2;
        entry.entry_id = "entry";
        entry.entry_bar_index = 3;
        entry.entry_comment = "literal";
        entry.max_runup = 4;
        entry.max_drawdown = 2;
        entry.entry_path_position = 0.5;
        entry.entry_commission_account = 0.25;
        entry.entry_incarnation = 7;
        pyramid_entries_.push_back(entry);
    }

    void on_source_bar(const Bar&) override {
        // This hook must never be reached by a hash-only fixture.
        std::abort();
    }
    PyramidEntry& lot() { return pyramid_entries_.front(); }
    const PyramidEntry& lot() const { return pyramid_entries_.front(); }
    double paid_entry_fee() const { return open_entry_commission(lot()); }
    std::string pine_comment() const { return open_trade_entry_comment(0); }
    double pine_runup() const { return open_trade_max_runup(0); }
    double pine_drawdown() const { return open_trade_max_drawdown(0); }
};

struct Mutation {
    const char* name;
    void (*apply)(PyramidEntry&);
};
const Mutation kMutations[] = {
    {"price", [](PyramidEntry& e) { e.price = 101; }},
    {"time", [](PyramidEntry& e) { ++e.time; }},
    {"qty", [](PyramidEntry& e) { e.qty = 3; }},
    {"entry_id", [](PyramidEntry& e) { e.entry_id = "other"; }},
    {"entry_bar_index", [](PyramidEntry& e) { ++e.entry_bar_index; }},
    {"entry_comment", [](PyramidEntry& e) { e.entry_comment = "different"; }},
    {"max_runup", [](PyramidEntry& e) { e.max_runup = 5; }},
    {"max_drawdown", [](PyramidEntry& e) { e.max_drawdown = 3; }},
    {"skip_entry_bar_high", [](PyramidEntry& e) { e.skip_entry_bar_high = true; }},
    {"skip_entry_bar_low", [](PyramidEntry& e) { e.skip_entry_bar_low = true; }},
    {"market_pyramid_add", [](PyramidEntry& e) { e.market_pyramid_add = true; }},
    {"entry_path_position", [](PyramidEntry& e) { e.entry_path_position = 1.5; }},
    {"entry_commission_account", [](PyramidEntry& e) { e.entry_commission_account = 0.5; }},
    {"entry_incarnation", [](PyramidEntry& e) { ++e.entry_incarnation; }},
    {"bracket_slot_shadowed", [](PyramidEntry& e) { e.bracket_slot_shadowed = true; }},
    {"ordinary_market_open", [](PyramidEntry& e) { e.ordinary_market_open = true; }},
    {"pooc_terminal_market_entry", [](PyramidEntry& e) { e.pooc_terminal_market_entry = true; }},
    {"ordinary_stop_open", [](PyramidEntry& e) { e.ordinary_stop_open = true; }},
};

bool same_hashes(const LiteralBook& a, const LiteralBook& b) {
    // stream_state_hash also reads initialized stream aggregators. This
    // fixture has never begun a stream: compare the broker surface only.
    return a.broker_state_hash() == b.broker_state_hash();
}

double from_bits(uint64_t bits) {
    double value;
    static_assert(sizeof(value) == sizeof(bits), "binary64 required");
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}
}  // namespace

int main() {
    int failures = 0;
    const LiteralBook base;
    for (const auto& mutation : kMutations) {
        LiteralBook changed;
        if (!same_hashes(base, changed)) {
            std::printf("FAIL literal identical books do not hash equally\n");
            ++failures;
        }
        mutation.apply(changed.lot());
        const bool broker_changed = base.broker_state_hash() != changed.broker_state_hash();
        if (!broker_changed) {
            std::printf("FAIL PyramidEntry.%s: broker hash unchanged\n", mutation.name);
            ++failures;
        }
    }

    // These omitted fields are directly observable without running a bar.
    LiteralBook visible;
    visible.lot().entry_commission_account = 0.5;
    visible.lot().entry_comment = "different";
    visible.lot().max_runup = 8;
    visible.lot().max_drawdown = 7;
    if (base.paid_entry_fee() == visible.paid_entry_fee()
        || base.pine_comment() == visible.pine_comment()
        || base.pine_runup() == visible.pine_runup()
        || base.pine_drawdown() == visible.pine_drawdown()) {
        std::printf("FAIL fixture did not change exposed physical-lot facts\n");
        ++failures;
    }

    // Normalization remains the established FNV scalar contract, not raw
    // object bytes. NaN payload/sign and negative zero are canonicalized.
    const std::vector<double PyramidEntry::*> doubles = {
        &PyramidEntry::price, &PyramidEntry::qty, &PyramidEntry::max_runup,
        &PyramidEntry::max_drawdown, &PyramidEntry::entry_path_position,
        &PyramidEntry::entry_commission_account,
    };
    for (auto field : doubles) {
        LiteralBook a, b;
        a.lot().*field = 0.0;
        b.lot().*field = -0.0;
        if (!same_hashes(a, b)) { ++failures; std::printf("FAIL signed-zero normalization\n"); }
        a.lot().*field = from_bits(0x7ff8000000000001ULL);
        b.lot().*field = from_bits(0xfff8000000000011ULL);
        if (!same_hashes(a, b)) { ++failures; std::printf("FAIL NaN normalization\n"); }
    }
    LiteralBook a, b;
    a.lot().entry_comment = std::string(128, 'x') + "a";
    b.lot().entry_comment = std::string(128, 'x') + "b";
    if (same_hashes(a, b)) { ++failures; std::printf("FAIL full string suffix not hashed\n"); }
    std::printf("physical-lot hash: %zu fields, %d failed; no bars executed\n",
                sizeof(kMutations) / sizeof(kMutations[0]), failures);
    return failures ? 1 : 0;
}
