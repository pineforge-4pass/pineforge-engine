#include "a5m_store_fixture.hpp"
#include <pineforge/detail/matrix_store.hpp>
#include <pineforge/generic_matrix.hpp>
#include <pineforge/matrix.hpp>
#include <pineforge/native_host.hpp>
#include <array>
#include <cstdio>
#include <cstring>
#include <vector>

namespace {

class FillHost final : public pineforge::NativeStrategyHost {
public:
    pineforge::detail::o1::Store& store;
    pineforge::NumericMatrix numeric = pineforge::NumericMatrix::new_(0, 3);
    pineforge::GenericMatrix<bool> mask = pineforge::GenericMatrix<bool>::new_(0, 0);
    std::vector<double> committed_closes;
    int fills = 0;

    explicit FillHost(pineforge::detail::o1::Store& owner) : store(owner) {
        pineforge::NativeRunSpec spec;
        spec.identity = {"a5m-fill-rollback", 1};
        spec.input_tf = spec.script_tf = "1";
        spec.ticker = "MATRIX";
        spec.tickerid = "TEST:MATRIX";
        spec.type = "crypto";
        spec.currency = "USDT";
        spec.basecurrency = "ETH";
        spec.timezone = "UTC";
        spec.session = "24x7";
        spec.slot_label_policy = pineforge::NativeSlotLabelPolicy::FeedTolerant;
        spec.initial_capital = 1000000.0;
        spec.point_value = spec.account_fx = 1.0;
        spec.price_tick = 0.125;
        spec.calculation = pineforge::NativeCalculationTrigger::BarCloseAndFills;
        spec.max_recalculations_per_point = 4;
        a5m_test::require(configure_native(spec).status == pineforge::NativeSetupStatus::Applied,
                          "configure matrix fill host");
    }

    void verify() const {
        const int count = static_cast<int>(committed_closes.size());
        a5m_test::require(numeric.rows() == count && numeric.columns() == 3 &&
                          mask.rows() == count && mask.columns() == 3, "fill rollback shape changed");
        for (int r = 0; r < count; ++r)
            for (int c = 0; c < 3; ++c) {
                const double expected = committed_closes[static_cast<std::size_t>(r)] + c * 0.25;
                const double actual = numeric.get(r, c);
                a5m_test::require(std::memcmp(&actual, &expected, sizeof actual) == 0,
                                  "fill rollback changed a numeric cell byte");
                a5m_test::require(mask.get(r, c) == ((r + c) % 2 == 0),
                                  "fill rollback changed a boolean cell");
            }
    }

    void on_native_bar(const pineforge::Bar& bar,
                       const pineforge::NativeDecisionContext&) override {
        numeric.add_row(numeric.rows(), {bar.close, bar.close + 0.25, bar.close + 0.5});
        const int r = static_cast<int>(committed_closes.size());
        mask.add_row(mask.rows(), {r % 2 == 0, r % 2 != 0, r % 2 == 0});
        committed_closes.push_back(bar.close);
        if (committed_closes.size() == 1) {
            pineforge::native_order::Request entry{
                pineforge::native_order::Transact{1.0}, "matrix-fill", "a5m"};
            entry.trigger = pineforge::native_order::Limit{bar.close - 1.0};
            (void)submit(entry);
        }
    }

    void on_native_recalculate(const pineforge::Bar& bar,
                               const pineforge::NativeDecisionContext& context,
                               pineforge::NativeCalculationReason reason,
                               const pineforge::native_order::ExecutionAppliedEvent* cause) override {
        if (reason != pineforge::NativeCalculationReason::OrderFill) {
            pineforge::NativeStrategyHost::on_native_recalculate(bar, context, reason, cause);
            return;
        }
        a5m_test::require(cause != nullptr, "fill callback lacks its execution");
        ++fills;
        verify();
        auto numeric_alias = numeric;
        auto mask_alias = mask;
        const auto numeric_id = numeric.external_identity(), mask_id = mask.external_identity();
        const auto numeric_hash = numeric.external_hash(), mask_hash = mask.external_hash();
        const auto live = store.stats().bytes_live;
        {
            auto transaction = store.begin();
            numeric_alias.fill(-bar.high);
            numeric_alias.add_row(0, {bar.low, bar.high, bar.open});
            numeric_alias.add_col(1, std::vector<double>(numeric_alias.rows(), bar.close));
            numeric_alias.sort(0, true);
            numeric_alias.remove_row(numeric_alias.rows() - 1);
            mask_alias.reshape(mask_alias.rows() + 4, 5);
            mask_alias.fill(true);
            transaction->rollback();
        }
        verify();
        a5m_test::require(numeric_alias.external_identity() == numeric_id &&
                          mask_alias.external_identity() == mask_id &&
                          numeric_alias.external_hash() == numeric_hash &&
                          mask_alias.external_hash() == mask_hash &&
                          store.stats().bytes_live == live,
                          "fill rollback changed identity, durable bytes or logical ownership");
    }
};

} // namespace

int main() {
    try {
        a5m_test::StoreFixture fixture;
        pineforge::detail::matrix_store::Runtime runtime(*fixture.store);
        pineforge::detail::matrix_store::ScopedRuntime scope(&runtime);
        FillHost host(*fixture.store);
        const std::array<pineforge::Bar, 3> bars{{
            {100.0, 101.0, 99.0, 100.5, 1000.0, INT64_C(1736121600000)},
            {100.5, 102.0, 98.0, 101.25, 1000.0, INT64_C(1736121660000)},
            {101.25, 103.0, 100.0, 102.5, 1000.0, INT64_C(1736121720000)}}};
        host.run(bars.data(), static_cast<int>(bars.size()), "1", "1");
        a5m_test::require(host.last_error().empty(), host.last_error().c_str());
        a5m_test::require(host.fills > 0 && host.committed_closes.size() == bars.size(),
                          "rollback fixture never reached a real fill recalculation");
        host.verify();
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "A5M fill rollback: %s\n", error.what());
        return 1;
    }
}
