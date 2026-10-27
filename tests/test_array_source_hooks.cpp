// Hand-lowered generated state, driven by the real source host's historical
// calc-on-fill hooks and request.security_lower_tf dispatch. Compiler emission
// is outside this test's scope.
#include <pineforge/array.hpp>
#include <pineforge/source/pine_strategy_host.hpp>
#include "array_store_fixture.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <optional>
#include <stdexcept>
#include <vector>

namespace {
using namespace pineforge;
using array_test::require;

class CoofArrayHost final : public source::PineStrategyHost {
    detail::o1::Store& store_;
    ScriptArray<double> values_;
    ScriptArray<double> alias_;
    ScriptArray<ScriptArray<double>> nested_;
    // Last nontrivial member: rollback precedes every owning array destructor.
    std::optional<ScriptArrayTransaction> transaction_;
    std::uint64_t checkpoint_hash_ = 0;
    std::uint64_t checkpoint_size_ = 0;
    double checkpoint_first_ = 0.0;
    std::uint64_t bodies_ = 0;
    std::uint64_t restores_ = 0;

    std::uint64_t array_hash() const {
        BrokerStateHashSink sink;
        values_.hash_into(sink);
        nested_.hash_into(sink);
        return sink.h;
    }
public:
    explicit CoofArrayHost(detail::o1::Store& store)
        : store_(store), values_(ScriptArray<double>::from(store, {-0.0, 2.0})),
          alias_(values_.alias()), nested_(store) {
        nested_.push(ScriptArray<double>::from(store, {3.0, 4.0}));
        source::PineStrategyConfig config;
        config.calc_on_order_fills = true;
        config.initial_capital = 1000000.0;
        config.default_qty_value = 1.0;
        configure_pine_strategy(config);
        set_syminfo_timezone("UTC");
        set_syminfo_session("24x7");
    }

    void snapshot_script_state() override {
        if (transaction_) { transaction_->commit(); transaction_.reset(); }
        checkpoint_hash_ = array_hash();
        checkpoint_size_ = values_.size();
        checkpoint_first_ = values_.first();
        transaction_.emplace(store_);
    }
    void restore_script_state() override {
        require(transaction_.has_value(), "source restore arrived without a snapshot");
        transaction_->rollback();
        transaction_.reset();
        const auto first = alias_.first();
        require(std::memcmp(&first, &checkpoint_first_, sizeof first) == 0,
                "calc-on-fill rollback changed the pre-body value");
        require(alias_.size() == checkpoint_size_ && array_hash() == checkpoint_hash_,
                "calc-on-fill rollback changed size or nested state");
        ++restores_;
        transaction_.emplace(store_);
    }
    void commit_script_state() override { snapshot_script_state(); }

    void on_source_bar(const Bar&) override {
        ++bodies_; // Fixture observation; intentionally not rolled back.
        alias_.set_i(0, -static_cast<double>(bodies_) * 0.25);
        values_.push(static_cast<double>(bodies_));
        nested_.mutate(0, [this](ScriptArray<double>& child) {
            child.set_i(0, static_cast<double>(bodies_));
            child.push(-0.0);
        });
        require(alias_.first() == -static_cast<double>(bodies_) * 0.25,
                "calc-on-fill body did not mutate through its alias");
        if (pine_bar_index() % 2 == 0 && signed_position_size() == 0.0)
            strategy_entry("L", true, na<double>(), na<double>(), 1.0);
        if (pine_bar_index() % 2 == 1 && signed_position_size() > 0.0)
            strategy_close("L");
    }

    void hash_host_extension(BrokerStateHashSink& sink) const override {
        source::PineStrategyHost::hash_host_extension(sink);
        sink.u(bodies_); sink.u(restores_);
        sink.u(checkpoint_hash_); sink.u(checkpoint_size_); sink.d(checkpoint_first_);
        values_.hash_into(sink); nested_.hash_into(sink);
    }

    void verify(std::size_t chart_bars) {
        require(bodies_ > chart_bars && restores_ > chart_bars && trade_count() > 0,
                "fixture never reached a real fill recalculation");
        // The run's final committed snapshot must restore through the alias
        // even when explicitly re-read after all broker callbacks finish.
        restore_script_state();
    }
};

class LowerArrayHost final : public source::PineStrategyHost {
    ScriptArray<double> close_;
    ScriptArray<bool> bull_;
    std::array<double, 15> expected_close_{};
    std::array<bool, 15> expected_bull_{};
    std::uint64_t chart_bars_ = 0;
    std::uint64_t evaluations_ = 0;
public:
    explicit LowerArrayHost(detail::o1::Store& store)
        : close_(ScriptArray<double>::preallocated(store, 15)),
          bull_(ScriptArray<bool>::preallocated(store, 15)) {
        register_security_lower_tf_eval(0, "1", "");
    }

    void evaluate_security(int sec_id, const Bar& bar, bool complete) override {
        if (sec_id != 0 || !complete) return;
        const auto index = security_lower_tf_sub_bar_index(0);
        require(index == static_cast<int>(evaluations_ % 15), "lower-TF dispatch order changed");
        if (index == 0) {
            close_.clear();
            bull_.clear();
        }
        // Independent, fixed-size reference containers observe the same
        // public dispatch as the hand-lowered array accumulators.
        expected_close_[static_cast<std::size_t>(index)] = bar.close;
        expected_bull_[static_cast<std::size_t>(index)] = bar.close > bar.open;
        close_.push(bar.close);
        bull_.push(bar.close > bar.open);
        ++evaluations_;
    }
    void on_source_bar(const Bar& bar) override {
        require(close_.size() == 15 && bull_.size() == 15, "lower-TF array did not reset/append");
        for (std::int64_t i = 0; i < 15; ++i) {
            const auto at = static_cast<std::size_t>(i);
            require(close_.get_i(i) == expected_close_[at],
                    "lower-TF numeric results changed order/value");
            require(bull_.get_i(i) == expected_bull_[at], "lower-TF bool cell changed");
        }
        require(close_.last() == bar.close, "last lower-TF element is not the parent close");
        ++chart_bars_;
    }
    void hash_host_extension(BrokerStateHashSink& sink) const override {
        source::PineStrategyHost::hash_host_extension(sink);
        sink.u(chart_bars_); sink.u(evaluations_);
        for (std::size_t i = 0; i < expected_close_.size(); ++i) {
            sink.d(expected_close_[i]); sink.b(expected_bull_[i]);
        }
        close_.hash_into(sink); bull_.hash_into(sink);
    }
    void verify(std::size_t count) const {
        require(chart_bars_ == count && evaluations_ == count * 15,
                "lower-TF fixture did not reach its complete population");
    }
};
} // namespace

int main() {
    try {
        array_test::StoreFixture fixture;
        {
            CoofArrayHost host(*fixture.store);
            std::vector<Bar> bars(16);
            for (std::size_t i = 0; i < bars.size(); ++i)
                bars[i] = {100.0, 102.0, 98.0, 101.0, 1000.0,
                    INT64_C(1736121600000) + static_cast<std::int64_t>(i) * 60000};
            host.run(bars.data(), static_cast<int>(bars.size()), "1", "1");
            require(host.last_error().empty(), host.last_error().c_str());
            host.verify(bars.size());
        }
        {
            constexpr std::size_t charts = 16;
            LowerArrayHost host(*fixture.store);
            // Same parent-bar emulation path as the existing request tests:
            // 15-minute input/chart, fifteen synthesized 1-minute results.
            std::vector<Bar> bars(charts);
            for (std::size_t i = 0; i < bars.size(); ++i) {
                const double open = 100.0 + static_cast<double>(i) * 4.0;
                const double close = open + (i % 2 ? 2.0 : -2.0);
                bars[i] = {open, std::max(open, close) + 4, std::min(open, close) - 4,
                    close, 1000.0, INT64_C(1736121600000) + static_cast<std::int64_t>(i) * 900000};
            }
            host.run(bars.data(), static_cast<int>(bars.size()), "15", "15");
            require(host.last_error().empty(), host.last_error().c_str());
            host.verify(charts);
        }
        std::puts("array source hooks: real fill rollback, lower-TF numeric and bool results");
        return 0;
    } catch (const std::exception& failure) {
        std::fprintf(stderr, "array source hooks: %s\n", failure.what());
        return 1;
    }
}
