// Literal settlement controls for the existing Pine source interpretation.
// These commands intentionally exercise the Pine close-artifact contract;
// they do not claim that a generic native reduction can open exposure.
#include <pineforge/engine.hpp>
#include <pineforge/source/pine_strategy_host.hpp>
#include <cmath>
#include <cstdio>
#include <limits>
#include <string>
#include <vector>

using namespace pineforge;
namespace {
int failures = 0;
#define CHECK(condition) do { if (!(condition)) { \
    std::fprintf(stderr, "FAIL %s:%d %s\n", __FILE__, __LINE__, #condition); \
    ++failures; } } while (0)

class Book : public pineforge::source::PineStrategyHost {
public:
    Book() {
        initial_capital_ = 1000000;
        default_qty_type_ = QtyType::FIXED;
        default_qty_value_ = 1;
        commission_value_ = 0;
        slippage_ = 0;
        margin_long_ = margin_short_ = 0;
        pyramiding_ = 1;
        bar_index_ = 0;
        current_bar_ = {100, 100, 100, 100, 1, 0};
    }
    void on_source_bar(const Bar&) override {}
    void entry(const std::string& id, bool buy, double amount) {
        const double nan = std::numeric_limits<double>::quiet_NaN();
        strategy_entry(id, buy, nan, nan, amount);
    }
    void settle() {
        ++bar_index_;
        current_bar_ = {100, 100, 100, 100, 1, int64_t(bar_index_) * 60000};
        process_pending_orders(current_bar_);
    }
    void close_seed() { strategy_close("seed"); }
    void change_close_reservation() {
        for (auto& order : pending_orders_) {
            if (order.id == "__close__seed") {
                order.qty = 1;
                order.quantity_request.reserve(1, 3);
                return;
            }
        }
        CHECK(false);
    }
    double signed_exposure() const {
        return position_side_ == PositionSide::FLAT ? 0
            : position_side_ == PositionSide::LONG ? position_qty_ : -position_qty_;
    }
    uint64_t fills() const { return broker_fill_event_seq_; }
    const std::vector<Trade>& rows() const { return trades_; }
    const std::vector<PyramidEntry>& lots() const { return pyramid_entries_; }
};

void check_rows(const Book& book, const std::vector<std::string>& ids,
                const std::vector<double>& amounts, bool seed_buy) {
    CHECK(book.rows().size() == ids.size());
    if (book.rows().size() != ids.size()) return;
    for (size_t i = 0; i < ids.size(); ++i) {
        const auto& row = book.rows()[i];
        CHECK(row.entry_id == ids[i]);
        CHECK(row.qty == amounts[i]);
        CHECK(row.is_long == (i == 0 ? seed_buy : true));
        CHECK(row.entry_price == 100 && row.exit_price == 100);
        CHECK(row.pnl == 0 && row.commission == 0);
        CHECK(row.exit_bar_index == 2);
        CHECK(row.entry_bar_index == (i == 0 ? 1 : 2));
        CHECK(row.entry_incarnation != 0);
        if (i > 0) CHECK(row.entry_incarnation != book.rows()[0].entry_incarnation);
        if (i > 1) CHECK((row.entry_incarnation == book.rows()[1].entry_incarnation) == seed_buy);
    }
}

void settle_batch(bool seed_buy, double seed, double opposite, double same,
                  bool close, bool alter_reservation = false) {
    Book book;
    book.entry("seed", seed_buy, seed);
    book.settle();
    CHECK(book.signed_exposure() == (seed_buy ? seed : -seed));
    CHECK(book.rows().empty() && book.fills() == 1);
    book.entry("opposite", !seed_buy, opposite);
    book.entry("seed", seed_buy, same);
    if (close) book.close_seed();
    // Explicit native reservation checkpoint: the original close Units stay3.
    // This is not a claim that a Pine source call changes this reservation.
    if (alter_reservation) book.change_close_reservation();
    book.settle();

    // Buy-before-sell, frozen signed transaction quantities, and FIFO give
    // these independent integer conservation results at the same price.
    double expected = seed_buy ? same : -same;
    if (close) expected = seed_buy ? same - seed
                                  : -(same - std::min(seed, opposite));
    CHECK(book.signed_exposure() == expected);
    CHECK(book.fills() == (close ? 4u : 3u));
    if (seed_buy) {
        if (close) check_rows(book, {"seed", "seed", "seed"},
                             {seed, opposite, seed}, true);
        else check_rows(book, {"seed", "seed"}, {seed, opposite}, true);
    } else {
        if (close) check_rows(book, {"seed", "opposite", "__close__seed"},
                             {seed, opposite, std::min(seed, opposite)}, false);
        else check_rows(book, {"seed", "opposite"}, {seed, opposite}, false);
    }
    double lot_total = 0;
    for (const auto& lot : book.lots()) lot_total += lot.qty;
    CHECK(lot_total == std::abs(expected));
    std::printf("settlement side=%s seed=%.0f opposite=%.0f same=%.0f close=%d reserve=%d exposure=%.0f rows=%zu fills=%llu\n",
        seed_buy ? "long" : "short", seed, opposite, same, close,
        alter_reservation, book.signed_exposure(), book.rows().size(),
        static_cast<unsigned long long>(book.fills()));
    const auto row_count = book.rows().size();
    const auto fill_count = book.fills();
    book.settle();
    CHECK(book.signed_exposure() == expected);
    CHECK(book.rows().size() == row_count && book.fills() == fill_count);
}

void vanished_target_without_matching_entry_cannot_open_artifact() {
    Book book;
    book.entry("seed", false, 3);
    book.settle();
    book.entry("opposite", true, 2);
    book.entry("different", false, 4);
    book.close_seed();
    book.settle();
    CHECK(book.signed_exposure() == -4);
    CHECK(book.fills() == 3);
    check_rows(book, {"seed", "opposite"}, {3, 2}, false);
    for (const auto& lot : book.lots()) CHECK(lot.entry_id != "__close__seed");
}
} // namespace

int main() {
    for (bool buy : {false, true}) {
        settle_batch(buy, 1, 1, 1, false);
        settle_batch(buy, 1, 1, 1, true);
        settle_batch(buy, 3, 2, 4, false);
        settle_batch(buy, 3, 2, 4, true);
    }
    settle_batch(false, 3, 2, 4, true, true);
    vanished_target_without_matching_entry_cannot_open_artifact();
    std::printf("Pine transaction settlement: %d failure(s)\n", failures);
    return failures ? 1 : 0;
}
