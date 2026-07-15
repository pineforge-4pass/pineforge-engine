/*
 * A global strategy.exit (omitted from_entry) armed after a same-direction
 * high-level MARKET strategy.entry on a POOC bar must cover the position that
 * exists after that queued entry fills. Freezing the exit reservation at the
 * pre-add live quantity strands the new pyramid slice when the bracket fires.
 */

#include <pineforge/engine.hpp>
#include <pineforge/na.hpp>

#include <cmath>
#include <cstdio>
#include <limits>
#include <string>
#include <vector>

using namespace pineforge;

namespace {

int failures = 0;

#define CHECK(cond, tag) do { \
    if (!(cond)) { \
        std::printf("FAIL: %s (line %d)\n", (tag), __LINE__); \
        ++failures; \
    } \
} while (0)

constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();

bool near(double lhs, double rhs, double tolerance = 1e-9) {
    return std::fabs(lhs - rhs) <= tolerance;
}

enum class QueuedEntryShape {
    SameDirectionMarket,
    OppositeMarket,
    RawMarket,
    PricedEntry,
    CoofRecalcMarket,
};

struct CaseConfig {
    bool pooc = true;
    std::vector<QueuedEntryShape> entry_shapes = {
        QueuedEntryShape::SameDirectionMarket,
    };
    std::vector<QueuedEntryShape> entry_shapes_after_exit;
    std::vector<QueuedEntryShape> carried_entry_shapes;
    std::string from_entry;
    double qty_percent = 100.0;
    double explicit_exit_qty = kNaN;
    int pyramiding = 3;
    bool second_global_exit = false;
    bool prior_partial_global_exit = false;
    bool replace_first_add_after_exit = false;
    bool later_bar_same_direction_entry = false;
    bool later_bar_sibling_exit = false;
    bool defer_exit_until_bar4 = false;
};

class ReservationProbe final : public BacktestEngine {
public:
    explicit ReservationProbe(CaseConfig config) : config_(std::move(config)) {
        initial_capital_ = 1'000'000.0;
        default_qty_type_ = QtyType::FIXED;
        default_qty_value_ = 1.0;
        commission_value_ = 0.0;
        slippage_ = 0;
        pyramiding_ = config_.pyramiding;
        process_orders_on_close_ = config_.pooc;
    }

    void on_bar(const Bar&) override {
        if (bar_index_ == 0) {
            strategy_entry("BASE", /*is_long=*/true,
                           kNaN, kNaN, /*qty=*/1.0);
            for (std::size_t i = 0;
                 i < config_.carried_entry_shapes.size(); ++i) {
                queue_carried_entry(config_.carried_entry_shapes[i], i);
            }
        }
        if (bar_index_ == 2
            && (config_.later_bar_same_direction_entry
                || config_.later_bar_sibling_exit
                || config_.defer_exit_until_bar4)
            && position_side_ == PositionSide::LONG) {
            for (const auto& order : pending_orders_) {
                if (order.type == OrderType::EXIT && order.id == "EXIT") {
                    post_fill_exit_qty_ = order.qty;
                }
            }
            if (config_.later_bar_same_direction_entry) {
                strategy_entry("LATER_BAR_ADD", /*is_long=*/true,
                               kNaN, kNaN, /*qty=*/1.0);
            }
            if (config_.later_bar_sibling_exit) {
                strategy_exit("LATER_SIBLING", "",
                              /*limit=*/120.0, /*stop=*/kNaN,
                              /*trail_points=*/kNaN,
                              /*trail_offset=*/kNaN,
                              /*trail_price=*/kNaN,
                              /*qty_percent=*/100.0,
                              "later sibling");
            }
            for (const auto& order : pending_orders_) {
                if (order.type == OrderType::EXIT && order.id == "EXIT") {
                    later_bar_exit_dynamic_qty_ =
                        order.pooc_global_full_exit_dynamic_qty;
                    post_fill_exit_qty_ = order.qty;
                }
                if (order.type == OrderType::EXIT
                    && order.id == "LATER_SIBLING") {
                    later_bar_sibling_captured_ = true;
                }
            }
        }
        if (bar_index_ != 1 || position_side_ != PositionSide::LONG) {
            return;
        }

        for (std::size_t i = 0; i < config_.entry_shapes.size(); ++i) {
            queue_entry(config_.entry_shapes[i], i);
        }

        if (config_.prior_partial_global_exit) {
            strategy_exit("PARTIAL", "",
                          /*limit=*/104.0, /*stop=*/kNaN,
                          /*trail_points=*/kNaN, /*trail_offset=*/kNaN,
                          /*trail_price=*/kNaN,
                          /*qty_percent=*/50.0, "partial sibling");
        }

        const bool defer_exit = config_.later_bar_same_direction_entry
            || config_.later_bar_sibling_exit
            || config_.defer_exit_until_bar4;
        strategy_exit("EXIT", config_.from_entry,
                      /*limit=*/defer_exit ? 110.0 : 105.0,
                      /*stop=*/kNaN,
                      /*trail_points=*/kNaN, /*trail_offset=*/kNaN,
                      /*trail_price=*/kNaN,
                      config_.qty_percent, "exit",
                      config_.explicit_exit_qty);

        if (config_.replace_first_add_after_exit) {
            strategy_entry("ADD_0", /*is_long=*/true,
                           kNaN, kNaN, /*qty=*/1.0);
        }

        if (config_.second_global_exit) {
            strategy_exit("EXIT2", "",
                          /*limit=*/106.0, /*stop=*/kNaN,
                          /*trail_points=*/kNaN, /*trail_offset=*/kNaN,
                          /*trail_price=*/kNaN,
                          /*qty_percent=*/100.0, "second exit");
        }

        for (std::size_t i = 0;
             i < config_.entry_shapes_after_exit.size(); ++i) {
            queue_entry(config_.entry_shapes_after_exit[i],
                        config_.entry_shapes.size() + i);
        }

        for (const auto& order : pending_orders_) {
            if (order.type != OrderType::EXIT) continue;
            ++exit_order_count_;
            if (order.id == "EXIT2") {
                second_exit_captured_ = true;
            }
            if (order.id == "EXIT") {
                captured_ = true;
                exit_qty_is_nan_ = std::isnan(order.qty);
                exit_qty_ = order.qty;
                exit_qty_percent_ = order.qty_percent;
                exit_dynamic_qty_ =
                    order.pooc_global_full_exit_dynamic_qty;
            }
        }
    }

    bool captured() const { return captured_; }
    bool exit_qty_is_nan() const { return exit_qty_is_nan_; }
    double exit_qty() const { return exit_qty_; }
    double exit_qty_percent() const { return exit_qty_percent_; }
    double position_size() const { return signed_position_size(); }
    int exit_order_count() const { return exit_order_count_; }
    bool second_exit_captured() const { return second_exit_captured_; }
    bool exit_dynamic_qty() const { return exit_dynamic_qty_; }
    bool later_bar_exit_dynamic_qty() const {
        return later_bar_exit_dynamic_qty_;
    }
    double post_fill_exit_qty() const { return post_fill_exit_qty_; }
    bool later_bar_sibling_captured() const {
        return later_bar_sibling_captured_;
    }

private:
    void queue_carried_entry(QueuedEntryShape shape, std::size_t ordinal) {
        const std::string suffix = "_" + std::to_string(ordinal);
        switch (shape) {
            case QueuedEntryShape::PricedEntry:
                strategy_entry("CARRIED_ENTRY" + suffix,
                               /*is_long=*/true,
                               /*limit=*/50.0, kNaN, /*qty=*/1.0);
                break;
            case QueuedEntryShape::RawMarket:
                strategy_order("CARRIED_RAW" + suffix,
                               /*is_long=*/true, /*qty=*/1.0,
                               /*limit=*/50.0, /*stop=*/kNaN);
                break;
            default:
                CHECK(false, "unsupported carried entry shape");
                break;
        }
    }

    void queue_entry(QueuedEntryShape shape, std::size_t ordinal) {
        const std::string suffix = "_" + std::to_string(ordinal);
        switch (shape) {
            case QueuedEntryShape::SameDirectionMarket:
                strategy_entry("ADD" + suffix, /*is_long=*/true,
                               kNaN, kNaN, /*qty=*/1.0);
                break;
            case QueuedEntryShape::OppositeMarket:
                strategy_entry("REVERSE" + suffix, /*is_long=*/false,
                               kNaN, kNaN, /*qty=*/1.0);
                break;
            case QueuedEntryShape::RawMarket:
                strategy_order("RAW_ADD" + suffix,
                               /*is_long=*/true, /*qty=*/1.0);
                break;
            case QueuedEntryShape::PricedEntry:
                strategy_entry("PRICED_ADD" + suffix, /*is_long=*/true,
                               /*limit=*/99.0, kNaN, /*qty=*/1.0);
                break;
            case QueuedEntryShape::CoofRecalcMarket:
                strategy_entry("COOF_ADD" + suffix, /*is_long=*/true,
                               kNaN, kNaN, /*qty=*/1.0);
                // Pin the provenance guard in isolation. Scheduler behavior is
                // covered by the dedicated COOF suites; this fixture only
                // needs a same-bar MARKET carrying recalc provenance when the
                // reservation decision runs.
                for (auto& order : pending_orders_) {
                    if (order.id == "COOF_ADD" + suffix) {
                        order.created_during_coof_recalc = true;
                    }
                }
                break;
        }
    }
    CaseConfig config_;
    bool captured_ = false;
    bool exit_qty_is_nan_ = false;
    double exit_qty_ = kNaN;
    double exit_qty_percent_ = kNaN;
    int exit_order_count_ = 0;
    bool second_exit_captured_ = false;
    bool exit_dynamic_qty_ = false;
    bool later_bar_exit_dynamic_qty_ = false;
    double post_fill_exit_qty_ = kNaN;
    bool later_bar_sibling_captured_ = false;
};

Bar make_bar(double open, double high, double low, double close,
             int64_t timestamp) {
    return Bar{open, high, low, close, 1000.0, timestamp};
}

Bar bars[] = {
    make_bar(100.0, 101.0, 99.0, 100.0,  900'000),
    make_bar(100.0, 102.0, 98.0, 100.0, 1'800'000),
    make_bar(100.0, 106.0, 98.0, 100.0, 2'700'000),
    make_bar(100.0, 101.0, 99.0, 100.0, 3'600'000),
    make_bar(100.0, 111.0, 99.0, 100.0, 4'500'000),
};

ReservationProbe run_case(CaseConfig config) {
    ReservationProbe probe(std::move(config));
    probe.run(bars, static_cast<int>(sizeof(bars) / sizeof(bars[0])));
    CHECK(probe.last_error().empty(), "case run succeeds");
    CHECK(probe.captured(), "exit reservation captured");
    return probe;
}

void test_positive_global_full_exit_defers_and_flattens_add() {
    ReservationProbe probe = run_case(CaseConfig{});
    CHECK(!probe.exit_qty_is_nan(),
          "eligible global full exit keeps finite sibling reservation");
    CHECK(near(probe.exit_qty(), 1.0),
          "eligible exit retains the pre-add finite fallback");
    CHECK(probe.exit_dynamic_qty(),
          "eligible exit is marked for full-live fill sizing");
    CHECK(near(probe.exit_qty_percent(), 100.0),
          "eligible exit remains a full-percent request");
    CHECK(probe.trade_count() == 2,
          "global bracket closes base and same-bar add slices");
    CHECK(near(probe.position_size(), 0.0),
          "global bracket leaves no stranded pyramid slice");
}

void test_explicit_exit_qty_keeps_literal_reservation() {
    CaseConfig config;
    config.explicit_exit_qty = 1.0;
    ReservationProbe probe = run_case(config);
    CHECK(!probe.exit_qty_is_nan(), "explicit exit qty is never deferred");
    CHECK(near(probe.exit_qty(), 1.0), "explicit exit qty remains literal");
    CHECK(near(probe.position_size(), 1.0),
          "explicit one-lot exit leaves the add slice open");
}

void test_partial_percent_keeps_frozen_reservation() {
    CaseConfig config;
    config.qty_percent = 50.0;
    ReservationProbe probe = run_case(config);
    CHECK(!probe.exit_qty_is_nan(), "partial percent is never deferred");
    CHECK(near(probe.exit_qty(), 0.5), "partial percent reserves live fraction");
    CHECK(near(probe.exit_qty_percent(), 50.0),
          "partial percent remains unchanged");
}

void test_from_entry_bound_exit_keeps_frozen_reservation() {
    CaseConfig config;
    config.from_entry = "BASE";
    ReservationProbe probe = run_case(config);
    CHECK(!probe.exit_qty_is_nan(), "from_entry-bound exit is never deferred");
    CHECK(near(probe.exit_qty(), 1.0),
          "from_entry-bound exit reserves the live base lot");
}

void test_non_pooc_keeps_frozen_reservation() {
    CaseConfig config;
    config.pooc = false;
    ReservationProbe probe = run_case(config);
    CHECK(!probe.exit_qty_is_nan(), "non-POOC exit is never deferred");
    CHECK(near(probe.exit_qty(), 1.0),
          "non-POOC exit reserves the live position");
}

void test_overcap_market_entry_keeps_frozen_reservation() {
    CaseConfig config;
    config.pyramiding = 1;
    ReservationProbe probe = run_case(config);
    CHECK(!probe.exit_qty_is_nan(),
          "over-cap market entry does not defer reservation");
    CHECK(near(probe.exit_qty(), 1.0),
          "over-cap market entry preserves live reservation");
}

void test_only_high_level_same_direction_market_qualifies() {
    for (QueuedEntryShape shape : {
            QueuedEntryShape::OppositeMarket,
            QueuedEntryShape::RawMarket,
            QueuedEntryShape::PricedEntry,
            QueuedEntryShape::CoofRecalcMarket}) {
        CaseConfig config;
        config.entry_shapes = {shape};
        ReservationProbe probe = run_case(config);
        CHECK(!probe.exit_qty_is_nan(),
              "non-qualifying queued entry does not defer reservation");
        CHECK(near(probe.exit_qty(), 1.0),
              "non-qualifying queued entry preserves live reservation");
    }
}

void test_opposite_before_qualifying_add_vetoes_deferred_reservation() {
    CaseConfig config;
    config.entry_shapes = {
        QueuedEntryShape::OppositeMarket,
        QueuedEntryShape::SameDirectionMarket,
    };
    ReservationProbe probe = run_case(config);
    CHECK(!probe.exit_qty_is_nan(),
          "opposite market plus qualifying add keeps frozen reservation");
    CHECK(near(probe.exit_qty(), 1.0),
          "mixed-direction queue reserves only the live position");
}

void test_priced_or_raw_coexistence_vetoes_deferred_reservation() {
    for (QueuedEntryShape extra : {
            QueuedEntryShape::PricedEntry,
            QueuedEntryShape::RawMarket}) {
        CaseConfig config;
        config.entry_shapes = {
            extra,
            QueuedEntryShape::SameDirectionMarket,
        };
        ReservationProbe probe = run_case(config);
        CHECK(!probe.exit_qty_is_nan(),
              "priced/RAW coexistence keeps frozen reservation");
        CHECK(near(probe.exit_qty(), 1.0),
              "mixed entry-kind queue reserves only the live position");
    }
}

void test_nonqualifying_order_after_exit_vetoes_deferred_reservation() {
    for (QueuedEntryShape later : {
            QueuedEntryShape::SameDirectionMarket,
            QueuedEntryShape::OppositeMarket,
            QueuedEntryShape::PricedEntry,
            QueuedEntryShape::RawMarket}) {
        CaseConfig config;
        config.entry_shapes_after_exit = {later};
        ReservationProbe probe = run_case(config);
        CHECK(!probe.exit_qty_is_nan(),
              "later nonqualifying order restores frozen reservation");
        CHECK(near(probe.exit_qty(), 1.0),
              "later mixed queue reserves only the pre-add live position");
        CHECK(!probe.exit_dynamic_qty(),
              "any later admitted entry-like order clears dynamic sizing");
    }
}

void test_prior_bar_carried_entry_vetoes_deferred_reservation() {
    for (QueuedEntryShape carried : {
            QueuedEntryShape::PricedEntry,
            QueuedEntryShape::RawMarket}) {
        CaseConfig config;
        config.carried_entry_shapes = {carried};
        ReservationProbe probe = run_case(config);
        CHECK(!probe.exit_qty_is_nan(),
              "carried priced/RAW entry keeps frozen reservation");
        CHECK(near(probe.exit_qty(), 1.0),
              "carried entry coexistence reserves the live position");
        CHECK(!probe.exit_dynamic_qty(),
              "prior-bar carried entry never enables dynamic sizing");
    }
}

void test_sibling_global_exit_preserves_first_reservation() {
    CaseConfig config;
    config.second_global_exit = true;
    ReservationProbe probe = run_case(config);
    CHECK(probe.exit_order_count() == 1,
          "full first exit consumes sibling reservation capacity");
    CHECK(!probe.second_exit_captured(),
          "second global exit is not admitted without capacity");
    CHECK(probe.exit_dynamic_qty(),
          "first fully reserved exit keeps the bounded dynamic marker");
}

void test_prior_partial_sibling_blocks_dynamic_full_reservation() {
    CaseConfig config;
    config.prior_partial_global_exit = true;
    ReservationProbe probe = run_case(config);
    CHECK(probe.exit_order_count() == 2,
          "partial sibling and remaining-capacity exit are both admitted");
    CHECK(!probe.exit_qty_is_nan(),
          "remaining-capacity global exit keeps finite reservation");
    CHECK(near(probe.exit_qty(), 0.5),
          "global exit reserves only capacity left by partial sibling");
    CHECK(!probe.exit_dynamic_qty(),
          "partial sibling prevents full-live dynamic sizing");
}

void test_same_id_add_replacement_after_exit_clears_dynamic_sizing() {
    CaseConfig config;
    config.replace_first_add_after_exit = true;
    ReservationProbe probe = run_case(config);
    CHECK(!probe.exit_dynamic_qty(),
          "post-exit same-id replacement clears dynamic sizing");
    CHECK(!probe.exit_qty_is_nan() && near(probe.exit_qty(), 1.0),
          "same-id replacement retains the finite pre-add fallback");
    CHECK(probe.trade_count() == 1,
          "replacement add is not counted as a bound pre-exit fill");
    CHECK(near(probe.position_size(), 1.0),
          "finite fallback leaves the unbound replacement add open");
}

void test_later_bar_entry_clears_resting_dynamic_sizing() {
    CaseConfig config;
    config.later_bar_same_direction_entry = true;
    ReservationProbe probe = run_case(config);
    CHECK(probe.exit_dynamic_qty(),
          "pre-exit add initially enables dynamic sizing");
    CHECK(!probe.later_bar_exit_dynamic_qty(),
          "later-bar admitted entry clears resting dynamic sizing");
    CHECK(near(probe.post_fill_exit_qty(), 2.0),
          "filled pre-exit add grows finite reservation before invalidation");
    CHECK(probe.trade_count() == 2,
          "finite exit closes base and the covered pre-exit add");
    CHECK(near(probe.position_size(), 1.0),
          "finite exit leaves the later unbound add open");
}

void test_samebar_later_add_does_not_erase_preexit_add_coverage() {
    CaseConfig config;
    config.entry_shapes_after_exit = {
        QueuedEntryShape::SameDirectionMarket,
    };
    config.defer_exit_until_bar4 = true;
    ReservationProbe probe = run_case(config);
    CHECK(!probe.exit_dynamic_qty(),
          "post-exit same-bar add clears dynamic sizing before fills");
    CHECK(near(probe.post_fill_exit_qty(), 2.0),
          "pre-exit bound add still grows finite reservation at fill");
    CHECK(probe.trade_count() == 2,
          "bounded reservation closes base and pre-exit add only");
    CHECK(near(probe.position_size(), 1.0),
          "same-bar post-exit add remains outside bounded coverage");
}

void test_later_bar_sibling_sees_grown_finite_reservation() {
    CaseConfig config;
    config.later_bar_sibling_exit = true;
    ReservationProbe probe = run_case(config);
    CHECK(near(probe.post_fill_exit_qty(), 2.0),
          "successful covered add grows first exit reservation");
    CHECK(!probe.later_bar_sibling_captured(),
          "later sibling is rejected after bounded reservation growth");
    CHECK(probe.trade_count() == 2,
          "first exit closes both bounded lots");
    CHECK(near(probe.position_size(), 0.0),
          "later sibling scenario finishes flat");
}

void test_rejected_bound_add_does_not_inflate_finite_reservation() {
    CaseConfig config;
    config.entry_shapes = {
        QueuedEntryShape::SameDirectionMarket,
        QueuedEntryShape::SameDirectionMarket,
    };
    config.pyramiding = 2;
    config.defer_exit_until_bar4 = true;
    ReservationProbe probe = run_case(config);
    CHECK(near(probe.post_fill_exit_qty(), 2.0),
          "only the one admitted bound add grows finite reservation");
    CHECK(probe.trade_count() == 2,
          "rejected second add creates no extra covered trade");
    CHECK(near(probe.position_size(), 0.0),
          "admitted base and add are fully covered");
}

void test_multiple_qualifying_adds_remain_covered() {
    CaseConfig config;
    config.entry_shapes = {
        QueuedEntryShape::SameDirectionMarket,
        QueuedEntryShape::SameDirectionMarket,
    };
    config.defer_exit_until_bar4 = true;
    ReservationProbe probe = run_case(config);
    CHECK(!probe.exit_qty_is_nan(),
          "multiple qualifying adds keep finite sibling reservation");
    CHECK(near(probe.exit_qty(), 1.0),
          "multiple-add exit retains the one-lot fallback");
    CHECK(probe.exit_dynamic_qty(),
          "multiple pre-exit qualifying adds enable dynamic sizing");
    CHECK(near(probe.post_fill_exit_qty(), 3.0),
          "each successful pre-exit add grows finite reservation exactly");
    CHECK(probe.trade_count() == 3,
          "global bracket closes base and both qualifying adds");
    CHECK(near(probe.position_size(), 0.0),
          "multiple qualifying adds leave no stranded slice");
}

}  // namespace

int main() {
    test_positive_global_full_exit_defers_and_flattens_add();
    test_explicit_exit_qty_keeps_literal_reservation();
    test_partial_percent_keeps_frozen_reservation();
    test_from_entry_bound_exit_keeps_frozen_reservation();
    test_non_pooc_keeps_frozen_reservation();
    test_overcap_market_entry_keeps_frozen_reservation();
    test_only_high_level_same_direction_market_qualifies();
    test_opposite_before_qualifying_add_vetoes_deferred_reservation();
    test_priced_or_raw_coexistence_vetoes_deferred_reservation();
    test_nonqualifying_order_after_exit_vetoes_deferred_reservation();
    test_prior_bar_carried_entry_vetoes_deferred_reservation();
    test_sibling_global_exit_preserves_first_reservation();
    test_prior_partial_sibling_blocks_dynamic_full_reservation();
    test_same_id_add_replacement_after_exit_clears_dynamic_sizing();
    test_later_bar_entry_clears_resting_dynamic_sizing();
    test_samebar_later_add_does_not_erase_preexit_add_coverage();
    test_later_bar_sibling_sees_grown_finite_reservation();
    test_rejected_bound_add_does_not_inflate_finite_reservation();
    test_multiple_qualifying_adds_remain_covered();
    if (failures != 0) {
        std::printf("%d check(s) FAILED\n", failures);
        return 1;
    }
    std::printf("test_pooc_global_full_exit passed.\n");
    return 0;
}
