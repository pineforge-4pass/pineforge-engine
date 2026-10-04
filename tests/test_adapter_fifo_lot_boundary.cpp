#include <pineforge/bar.hpp>
#include <pineforge/engine.hpp>
#include <pineforge/pending_order_mirror.hpp>
#include <pineforge/source/pine_strategy_host.hpp>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <string>
#include <vector>

namespace {

using namespace pineforge;
int failures = 0;
int checks = 0;
constexpr double missing = std::numeric_limits<double>::quiet_NaN();

#define CHECK(expression) do { \
    ++checks; \
    if (!(expression)) { \
        ++failures; \
        std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #expression); \
    } \
} while (false)

enum class Mode { SeparateExits, GapExits, FifoBoundary, ShortFifoBoundary,
                  AnyBoundary, ChainedBoundary, AddonEntries, SmallClose,
                  SmallNamedClose, PercentFloor, CashFloor, FractionalChain, LongChain,
                  UnrelatedSuffix, CancelledPredecessor, ReplacedPredecessor,
                  PartialPredecessor, NearAbove, NearBelow, OutsideTolerance,
                  PoocNearAbove, PoocNearBelow };

class LotStrategy final : public source::PineStrategyHost {
public:
    explicit LotStrategy(Mode mode) : mode_(mode) {
        source::PineStrategyConfig config;
        config.initial_capital = 1000000.0;
        config.default_qty_type = static_cast<int>(QtyType::FIXED);
        config.default_qty_value = 1.0;
        config.pyramiding = 10;
        config.margin_long = mode == Mode::GapExits ? 100.0 : 0.0;
        config.margin_short = config.margin_long;
        config.close_entries_rule_any = mode == Mode::AnyBoundary;
        config.process_orders_on_close = mode == Mode::PoocNearAbove || mode == Mode::PoocNearBelow;
        if (mode == Mode::GapExits) {
            config.initial_capital = 500000.0;
            config.commission_type = static_cast<int>(CommissionType::CASH_PER_CONTRACT);
            config.commission_value = 20.0;
        }
        if (mode == Mode::PercentFloor || mode == Mode::CashFloor) {
            config.initial_capital = 8362.172842;
            config.default_qty_type = static_cast<int>(mode == Mode::PercentFloor
                ? QtyType::PERCENT_OF_EQUITY : QtyType::CASH);
            config.default_qty_value = mode == Mode::PercentFloor ? 100.0 : 8362.172842;
        }
        if (mode == Mode::FractionalChain || mode == Mode::LongChain) {
            config.default_qty_type = static_cast<int>(QtyType::CASH);
            config.commission_type = static_cast<int>(CommissionType::PERCENT);
            config.commission_value = 0.1;
            config.slippage = 2;
            if (mode == Mode::LongChain) config.pyramiding = 64;
        }
        configure_pine_strategy(config);
        set_syminfo_mintick(mode == Mode::GapExits ? 0.001 : 0.01);
        if (mode == Mode::FifoBoundary || mode == Mode::ShortFifoBoundary
            || mode == Mode::AnyBoundary || mode == Mode::ChainedBoundary
            || mode == Mode::UnrelatedSuffix || mode == Mode::CancelledPredecessor
            || mode == Mode::ReplacedPredecessor || mode == Mode::PartialPredecessor)
            qty_step_ = 0.0001;
        if (mode == Mode::GapExits) qty_step_ = 0.01;
        if (mode == Mode::PercentFloor || mode == Mode::CashFloor) qty_step_ = 0.0001;
        if (mode == Mode::FractionalChain || mode == Mode::LongChain) qty_step_ = 0.0001;
    }

    void on_source_bar(const Bar&) override {
        const int index = pine_bar_index();
        if (mode_ == Mode::PercentFloor || mode_ == Mode::CashFloor) {
            if (index == 0) entry("E", missing);
            if (index == 1) strategy_close_all();
        } else if (mode_ == Mode::GapExits) {
            if (index == 0) {
                entry("E", 2.0);
                exit("T1", "E", 3315.15, 3300.545, 1.0);
                exit("T2", "E", 3325.15, 3300.545, 1.0);
            }
        } else if (mode_ == Mode::SeparateExits) {
            if (index == 0) {
                entry("E", 2.0);
                exit("T1", "E", 105.0, 95.0, 1.0);
                exit("T2", "E", 105.0, 95.0, 1.0);
            }
        } else if (mode_ == Mode::LongChain) {
            if (index == 0) {
                entry("Seed", 0.0345);
                for (int ordinal = 0; ordinal < 50; ++ordinal)
                    entry("E" + std::to_string(ordinal), 0.0138);
            }
            if (index == 1) strategy_close("Seed", {}, 0.0276);
            if (index == 2) {
                for (int ordinal = 0; ordinal < 50; ++ordinal)
                    strategy_close("E" + std::to_string(ordinal));
            }
            if (index == 3) strategy_close_all();
        } else if (mode_ == Mode::FractionalChain) {
            if (index == 0) {
                entry("A", 0.0414);
                entry("B", 0.0138);
                entry("B", 0.0138);
                entry("C", 0.0138);
                entry("C", 0.0138);
                entry("D", 0.0138);
            }
            if (index == 1) strategy_close("A", {}, 0.0345);
            if (index == 2) {
                strategy_close("B");
                strategy_close("C");
            }
            if (index == 3) strategy_close_all();
        } else if (mode_ == Mode::ChainedBoundary || mode_ == Mode::UnrelatedSuffix
                   || mode_ == Mode::CancelledPredecessor || mode_ == Mode::ReplacedPredecessor
                   || mode_ == Mode::PartialPredecessor) {
            if (index == 0) {
                entry("First", mode_ == Mode::PartialPredecessor ? 0.00147 : 0.00127);
                entry("A", 0.00047);
                entry("B", 0.00057);
                entry("C", mode_ == Mode::PartialPredecessor ? 0.00117 : 0.00097);
                entry("D", 0.00117);
            }
            if (index == 1) {
                if (mode_ == Mode::ChainedBoundary) strategy_close("First");
                if (mode_ == Mode::CancelledPredecessor) {
                    strategy_close("First");
                    strategy_cancel("First");
                }
                if (mode_ == Mode::PartialPredecessor) strategy_close("First", {}, 0.0012);
                if (mode_ == Mode::ReplacedPredecessor) {
                    strategy_close("First", {}, missing, missing, false, 123);
                    strategy_close("C", {}, missing, missing, false, 123);
                } else strategy_close("C");
                if (mode_ == Mode::UnrelatedSuffix || mode_ == Mode::CancelledPredecessor
                    || mode_ == Mode::ReplacedPredecessor) inspect_literal_close();
            }
            if (index == 3) strategy_close_all();
        } else if (mode_ == Mode::NearAbove || mode_ == Mode::NearBelow
                   || mode_ == Mode::OutsideTolerance || mode_ == Mode::PoocNearAbove
                   || mode_ == Mode::PoocNearBelow) {
            if (index == 0) {
                entry("A", 0.4);
                entry("B", 0.5);
                entry("C", 0.9 + (mode_ == Mode::NearBelow || mode_ == Mode::PoocNearBelow
                                ? -5e-11 : mode_ == Mode::OutsideTolerance ? 5e-9 : 5e-11));
                entry("D", 1.1);
            }
            if (index == 1) strategy_close("C");
            if (index == 3) strategy_close_all();
        } else if (mode_ == Mode::FifoBoundary || mode_ == Mode::ShortFifoBoundary
                   || mode_ == Mode::AnyBoundary) {
            if (index == 0) {
                entry("A", 0.00047);
                entry("B", 0.00057);
                entry("C", 0.00097);
                entry("D", 0.00117);
            }
            if (index == 1) strategy_close("C");
            if (index == 3) strategy_close_all();
        } else if (mode_ == Mode::AddonEntries) {
            if (index == 0 || index == 1) entry("E", 1.0);
            if (index == 2) strategy_close_all();
        } else {
            if (index == 0) entry("E", 1.0);
            if (index == 1) {
                if (mode_ == Mode::SmallNamedClose) strategy_close("E", {}, 1e-8);
                else exit("Tiny", "E", 99.0, missing, 1e-8);
            }
            if (index == 3) strategy_close_all();
        }
    }

private:
    void inspect_literal_close() {
        const auto& view = pending_intent_view();
        bool found = false;
        for (int index = 0; index < view.size(); ++index) {
            pf_pending_order_v1_t row{};
            CHECK(view.copy_v1(index, &row) == 0);
            if (std::strcmp(row.id, "__close__C") != 0) continue;
            found = true;
            CHECK(row.quantity_intent_kind == 2U);
            CHECK(row.quantity_intent_units == 9.0 * 0.0001);
            CHECK(row.quantity_reservation_present == 0U);
        }
        CHECK(found);
        // Pinned against the unchanged adapter request shape and hash at 227c2236.
        // expectation corrected (global exit children): UnrelatedSuffix 4142570753076958590ULL -> 10477892404542602439ULL, CancelledPredecessor 3470493253387916331ULL -> 11961130350211294910ULL, the third mode 10254752463860708201ULL -> 3195506365498973368ULL,
        // because the source adapter's state now folds TradingView's entry-id
        // table once it holds two entry ids (pineforge-source-adapter/v4); every
        // printed row is main 700c5d24's.
        const std::uint64_t hash = broker_state_hash_projection();
        const std::uint64_t expected = mode_ == Mode::UnrelatedSuffix
            ? 10477892404542602439ULL : mode_ == Mode::CancelledPredecessor
            ? 11961130350211294910ULL : 3195506365498973368ULL;
        CHECK(hash == expected);
        std::printf("literal-close mode=%d hash=%llu\n", static_cast<int>(mode_),
                    static_cast<unsigned long long>(hash));
    }

    void entry(const std::string& identifier, double quantity) {
        strategy_entry(identifier, mode_ != Mode::ShortFifoBoundary,
                       missing, missing, quantity);
    }

    void exit(const std::string& identifier, const std::string& entry_identifier,
              double limit, double stop, double quantity) {
        strategy_exit(identifier, entry_identifier, limit, stop, missing, missing,
                      missing, 100.0, identifier, quantity);
    }

    Mode mode_;
};

std::vector<Bar> bars(bool crossing) {
    std::vector<Bar> result;
    for (int index = 0; index < 7; ++index) {
        const bool active = crossing && index == 1;
        result.push_back({100.0, active ? 106.0 : 101.0,
                          active ? 94.0 : 99.0, 100.0, 1.0,
                          1746057600000LL + index * 900000LL});
    }
    return result;
}

void run_case(Mode mode, int expected_rows, const std::vector<double>& quantities) {
    LotStrategy strategy(mode);
    auto feed = mode == Mode::GapExits
        ? std::vector<Bar>{{3304.455, 3309.68, 3301.335, 3305.15, 1.0, 1747834200000LL},
                           {3305.135, 3306.51, 3295.44, 3303.7, 1.0, 1747835100000LL},
                           {3303.695, 3304.09, 3295.355, 3298.865, 1.0, 1747836000000LL}}
        : bars(mode == Mode::SeparateExits);
    if (mode == Mode::PercentFloor || mode == Mode::CashFloor) {
        for (auto& bar : feed) {
            bar.open = bar.high = bar.low = bar.close = 1772.21;
        }
    }
    strategy.run(feed.data(), static_cast<int>(feed.size()), "15", "15");
    CHECK(strategy.last_error().empty());
    CHECK(strategy.trade_count() == expected_rows);
    for (int index = 0; index < strategy.trade_count(); ++index) {
        const auto& trade = strategy.get_trade(index);
        std::printf("row %d %s/%s qty=%.17g pnl=%.17g\n", index,
                    trade.entry_id.c_str(), trade.exit_id.c_str(), trade.qty, trade.pnl);
        CHECK(trade.qty > 0.0);
        if (index < static_cast<int>(quantities.size())) {
            CHECK(std::abs(trade.qty - quantities[static_cast<std::size_t>(index)]) < 1e-14);
        }
    }
    if ((mode == Mode::SeparateExits || mode == Mode::GapExits)
        && strategy.trade_count() == 2) {
        CHECK(strategy.get_trade(0).exit_id == "T1");
        CHECK(strategy.get_trade(1).exit_id == "T2");
    }
    if (mode == Mode::AddonEntries && strategy.trade_count() == 2) {
        CHECK(strategy.get_trade(0).entry_incarnation != strategy.get_trade(1).entry_incarnation);
    }
}

}

int main() {
    // TV separate-exit-identities: SHA-256 d8702847a1b5e4109b0080c8ff9e45107119a19d762399558113486a2ea28d99.
    run_case(Mode::SeparateExits, 2, {1.0, 1.0});
    // TV gap-sibling-exits: SHA-256 1b1408c6759615be94446621c2ced4f439037bccf01332f307a6fc3657f7997f.
    run_case(Mode::GapExits, 2, {1.0, 1.0});
    // TV fifo-named-boundary: SHA-256 d2c641650269f5dca1455473ca41e523811d94dd7c343bbf529bb56f0087760c.
    run_case(Mode::FifoBoundary, 4, {0.0004, 0.0005, 0.0009, 0.0011});
    // TV short-fifo-boundary: SHA-256 a739fd1b85798dfab19d8323656021f25f69acca2f045460ae29bc2bbbdc519a.
    run_case(Mode::ShortFifoBoundary, 4, {0.0004, 0.0005, 0.0009, 0.0011});
    // TV any-named-boundary: SHA-256 4ed52e8fd4f08c8128dc198cff436e1a7e70fa564462c203decf16bcb435c87f.
    run_case(Mode::AnyBoundary, 4, {0.0009, 0.0004, 0.0005, 0.0011});
    // TV chained-fifo-boundary: SHA-256 b56e0675d078046ed195ed7482d1125648363de31f21f08abb8ccf28359f1c89.
    run_case(Mode::ChainedBoundary, 5, {0.0012, 0.0004, 0.0005, 0.0009, 0.0011});
    // TV addon-lot-identities: SHA-256 ca04884f0c50cfd786287e576e7bbb3dcf2ece1ec90c1d4d610b66b663e8399c.
    run_case(Mode::AddonEntries, 2, {1.0, 1.0});
    // Adapter-only tolerance/request-shape control; no separate TV tape claim.
    run_case(Mode::SmallClose, 2, {1e-8, 1.0 - 1e-8});
    // Adapter-only tolerance/request-shape control; no separate TV tape claim.
    run_case(Mode::SmallNamedClose, 2, {1e-8, 1.0 - 1e-8});
    // TV percent-sizing-floor: SHA-256 24561b06b0a55f15eece1b43449dab96149f80d136959b477c7f08ec0f2b0011.
    run_case(Mode::PercentFloor, 1, {4.7184});
    // TV cash-sizing-floor: SHA-256 4f46ff7c192d191d38c871a009fb7d7625bdafe393d826eef305456e5dd7e3dd.
    run_case(Mode::CashFloor, 1, {4.7184});
    // TV fractional-cost-chain: SHA-256 e7c2bfb094916e9b2002795370bbd1a4765b2d24130ee5e6e9f0dd259fecd639.
    run_case(Mode::FractionalChain, 7, {0.0345, 0.0069, 0.0069, 0.0069,
                                     0.0069, 0.0069, 0.0138});
    std::vector<double> long_chain_quantities(102, 0.0069);
    long_chain_quantities.front() = 0.0276;
    // TV fifty-static-closes: SHA-256 21fe943546b5f8e9df10b621da16b3b95c9eff305f4343b2dd220864d8c11e37.
    run_case(Mode::LongChain, 102, long_chain_quantities);
    // TV unrelated-suffix: SHA-256 c002e29b6254fd2a8b7655f5d41ef7e6bf0493037e1b5896e029dd5b656de719.
    run_case(Mode::UnrelatedSuffix, 6, {0.0009, 0.0003, 0.0004, 0.0005, 0.0009, 0.0011});
    // Adapter-only tolerance/request-shape control; no separate TV tape claim.
    run_case(Mode::CancelledPredecessor, 6, {0.0009, 0.0003, 0.0004, 0.0005, 0.0009, 0.0011});
    // Adapter-only tolerance/request-shape control; no separate TV tape claim.
    run_case(Mode::ReplacedPredecessor, 6, {0.0009, 0.0003, 0.0004, 0.0005, 0.0009, 0.0011});
    // Adapter-only tolerance/request-shape control; no separate TV tape claim.
    run_case(Mode::PartialPredecessor, 6, {0.0012, 0.0002, 0.0004, 0.0005, 0.0011, 0.0011});
    // Adapter-only tolerance/request-shape control; no separate TV tape claim.
    run_case(Mode::NearAbove, 4, {0.4, 0.5, 0.90000000005, 1.1});
    // Adapter-only tolerance/request-shape control; no separate TV tape claim.
    run_case(Mode::NearBelow, 4, {0.4, 0.5, 0.89999999995, 1.1});
    // Adapter-only tolerance/request-shape control; no separate TV tape claim.
    run_case(Mode::OutsideTolerance, 5, {0.4, 0.5, 5e-9, 0.9, 1.1});
    // Adapter-only tolerance control: the existing POOC boundary has the same bound.
    run_case(Mode::PoocNearAbove, 4, {0.4, 0.5, 0.90000000005, 1.1});
    // Adapter-only tolerance control: the existing POOC boundary has the same bound.
    run_case(Mode::PoocNearBelow, 4, {0.4, 0.5, 0.89999999995, 1.1});
    std::printf("test_adapter_fifo_lot_boundary: %d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
