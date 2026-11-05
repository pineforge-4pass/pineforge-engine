// Expected red on de9f11ce: the first native refusal below reports bar 0,
// not bar 1. With an outer TA context it reports that unrelated bar instead.
// The nested rows also require Pine context restoration after a child run.
// Fail-before is inferred from script_store.cpp; execution belongs to WIN-CX.
#include <pineforge/native_host.hpp>
#include "cr559_map_capacity_fixture.hpp"

#include <pineforge/ta.hpp>
#include "../src/native_execution_consumer.hpp"

#include <array>
#include <memory>
#include <string>
#include <utility>

namespace {
using namespace pineforge;

const std::array<Bar, 3> bars{{
    {100.0, 102.0, 99.0, 101.0, 10.0, 1704067200000LL},
    {101.0, 103.0, 100.0, 102.0, 11.0, 1704067500000LL},
    {102.0, 104.0, 101.0, 103.0, 12.0, 1704067800000LL},
}};

NativeRunSpec spec_for(const char* session) {
    NativeRunSpec spec;
    spec.identity.session_key = session;
    spec.identity.run_number = 1;
    spec.input_tf = spec.script_tf = "5";
    spec.ticker = "MOCK";
    spec.tickerid = "TEST:MOCK";
    spec.type = "crypto";
    spec.currency = "USDT";
    spec.basecurrency = "ETH";
    spec.timezone = "UTC";
    spec.session = "24x7";
    spec.initial_capital = 10000.0;
    spec.point_value = spec.account_fx = 1.0;
    spec.price_tick = 0.01;
    return spec;
}

void require_failure(BacktestEngine& host, cr559::Scalar expected_bar) {
    auto* handle = &host;
    cr559::require(strategy_last_run_status(handle) == 1, "map refusal did not fail the run");
    const char* code = strategy_get_last_error_code(handle);
    const char* args = strategy_get_last_error_args(handle);
    const char* text = strategy_get_last_error(handle);
    cr559::require(code && std::string(code) == "pine_map_capacity_exceeded",
                   "map context row lost its typed failure");
    if (!args || std::string(args) != cr559::capacity_args(3, 4, expected_bar))
        throw std::runtime_error("wrong map refusal bar context: " +
                                 std::string(args ? args : "<null>"));
    cr559::require(text && std::string(text) == cr559::capacity_message(3, 4, expected_bar),
                   "map context row changed exact TV English");
}

class LateNativeHost final : public NativeStrategyHost {
public:
    void on_native_run_begin() override { map_ = cr559::Map::new_(); }
    void on_native_bar(const Bar&, const NativeDecisionContext&) override {
        ++callbacks;
        if (callbacks == 1) {
            for (cr559::Scalar index = 0; index < 3; ++index)
                (void)map_.put(cr559::key_for(index), cr559::value_for(index));
            cr559::require(map_.put(cr559::key_for(0), -19) == cr559::value_for(0),
                           "replacement was refused at the configured limit");
        } else {
            (void)map_.put(cr559::key_for(3), 31);
            passed_refusal = true;
        }
    }
    void verify() const {
        cr559::require(callbacks == 2 && !passed_refusal, "native refusal did not stop bar one");
        cr559::require(map_.size() == 3 && map_.get(cr559::key_for(0)) == -19 &&
                           !map_.contains(cr559::key_for(3)),
                       "native refusal changed the map");
    }
    void hash_host_extension(BrokerStateHashSink& sink) const override {
        NativeStrategyHost::hash_host_extension(sink);
        sink.s("cr559/late-native");
        sink.i(callbacks);
        sink.b(passed_refusal);
        sink.b(map_.is_na());
        if (!map_.is_na())
            map_.hash_into(sink, [](auto& out, cr559::Scalar key) { out.i(key); },
                           [](auto& out, cr559::Scalar value) { out.i(value); });
    }

    int callbacks = 0;
    bool passed_refusal = false;

private:
    cr559::Map map_;
};

template <class Host, class... Args>
std::unique_ptr<Host> make_limited_host(Args&&... args) {
    detail::MapPairLimitScope creation_policy(3);
    return std::make_unique<Host>(std::forward<Args>(args)...);
}

void run_late_native() {
    auto host = make_limited_host<LateNativeHost>();
    cr559::require(host->configure_native(spec_for("cr559-late-native")).status ==
                       NativeSetupStatus::Applied,
                   "native context fixture setup was refused");
    host->run(bars.data(), static_cast<int>(bars.size()));
    host->verify();
    require_failure(*host, 1);
}

class PineContextParent final : public NativeStrategyHost {
public:
    explicit PineContextParent(bool use_block) : use_block_(use_block) {}
    void on_native_bar(const Bar&, const NativeDecisionContext&) override {
        ++callbacks;
        auto* ambient = use_block_ ? NativeExecutionConsumer::bound(*this).pump_ambient() : nullptr;
        cr559::require(!use_block_ || ambient != nullptr, "parent runtime block was not installed");
        // This is the same scope the source host installs around on_source_bar.
        internal::AmbientBarContextScope pine_bar(ambient, 37, 0);
        run_late_native(); // Child bar one must not inherit Pine bar 37.
        child_checked = true;
        cr559::require(ta::bar_context().installed && ta::bar_context().bar_index == 37,
                       "child run changed its parent's TA context");
        map_ = cr559::Map::new_();
        for (cr559::Scalar index = 0; index < 3; ++index)
            (void)map_.put(cr559::key_for(index), cr559::value_for(index));
        (void)map_.put(cr559::key_for(3), 41); // Parent must still report Pine bar 37.
        passed_refusal = true;
    }
    void hash_host_extension(BrokerStateHashSink& sink) const override {
        NativeStrategyHost::hash_host_extension(sink);
        sink.s("cr559/pine-context-parent");
        sink.b(use_block_);
        sink.i(callbacks);
        sink.b(child_checked);
        sink.b(passed_refusal);
        sink.b(map_.is_na());
        if (!map_.is_na())
            map_.hash_into(sink, [](auto& out, cr559::Scalar key) { out.i(key); },
                           [](auto& out, cr559::Scalar value) { out.i(value); });
    }

    int callbacks = 0;
    bool child_checked = false;
    bool passed_refusal = false;

private:
    bool use_block_;
    cr559::Map map_;
};

void standalone_context_is_restored() {
    ta::BarContextScope own_bar(73, 0);
    auto map = cr559::Map::new_(3);
    for (cr559::Scalar index = 0; index < 3; ++index)
        (void)map.put(cr559::key_for(index), cr559::value_for(index));
    cr559::expect_capacity_error([&] { (void)map.put(cr559::key_for(3), 43); }, 3, 4, 73);
}
} // namespace

int main() {
    try {
        cr559::Session session;
        run_late_native();
        {
            ta::BarContextScope unrelated(91, 0);
            run_late_native();
            cr559::require(ta::bar_context().bar_index == 91,
                           "native callback changed the unrelated TA context");
        }
        for (const bool use_block : {false, true}) {
            auto parent = make_limited_host<PineContextParent>(use_block);
            cr559::require(parent->configure_native(spec_for("cr559-pine-parent")).status ==
                               NativeSetupStatus::Applied,
                           "parent context fixture setup was refused");
            parent->run(bars.data(), static_cast<int>(bars.size()));
            cr559::require(parent->callbacks == 1 && parent->child_checked &&
                               !parent->passed_refusal,
                           "nested context row did not reach the parent's actual refusal");
            require_failure(*parent, 37);
            standalone_context_is_restored();
        }
        std::puts("CR559 context: late native, unrelated TA, nested Pine and both ambient paths checked");
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "CR559 context FAIL: %s\n", error.what());
        return 1;
    }
}
