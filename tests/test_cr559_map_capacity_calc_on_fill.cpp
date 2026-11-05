// Expected red on a3e7822e: on_source_bar reaches the fixed 50,000 refusal
// before twelve bars complete, so last_error() is not empty.
// This drives real fill recalculations, in addition to the Store rollback TU.
#include "p2_a5strmap_fixture.hpp"
#include "cr559_map_capacity_fixture.hpp"

#include <pineforge/source/pine_strategy_host.hpp>

#include <optional>

namespace {
class GrowthFillHost final : public pineforge::source::PineStrategyHost {
public:
    GrowthFillHost() {
        a5_fixture::configure(*this, true, false);
        map_ = cr559::Map::new_();
        alias_ = map_;
        cr559::append(map_, model_, 0, 4096);
        next_ = 4096;
    }
    void snapshot_script_state() override {
        checkpoint_.emplace(map_.snapshot());
        saved_model_ = model_;
        saved_next_ = next_;
        ++captures;
    }
    void restore_script_state() override {
        cr559::require(checkpoint_.has_value(), "calc-on-fill restored before checkpoint");
        map_.restore(*checkpoint_);
        model_ = saved_model_;
        next_ = saved_next_;
        cr559::exact(map_, model_);
        cr559::exact(alias_, model_);
        ++restores;
    }
    void commit_script_state() override {
        checkpoint_.emplace(map_.snapshot());
        saved_model_ = model_;
        saved_next_ = next_;
        ++commits;
    }
    void on_source_bar(const pineforge::Bar&) override {
        ++callbacks;
        const auto end = next_ + 4097;
        cr559::append(map_, model_, next_, end);
        next_ = end;
        const auto first = model_.front();
        cr559::require(map_.remove(first.first) == first.second,
                       "fill calculation remove changed the previous value");
        model_.erase(model_.begin());
        (void)map_.put(first.first, -next_);
        model_.emplace_back(first.first, -next_);
        if (pine_bar_index() % 2 == 0 && live_position_size() == 0)
            strategy_entry("cr559-growth-fill", true, pineforge::na<double>(),
                           pineforge::na<double>(), 1.0);
        if (pine_bar_index() % 2 != 0 && live_position_size() != 0)
            strategy_close("cr559-growth-fill");
    }
    void verify() const {
        cr559::require(captures > 0 && restores > 0 && commits > 0 &&
                           callbacks > 12 && trade_count() > 0,
                       "growth fixture did not execute actual calc-on-fill hooks");
        cr559::exact(map_, model_);
        cr559::exact(alias_, model_);
    }
    void hash_host_extension(pineforge::BrokerStateHashSink& sink) const override {
        PineStrategyHost::hash_host_extension(sink);
        sink.s("cr559/growing-map");
        sink.i(next_);
        map_.hash_into(sink, [](auto& out, cr559::Scalar key) { out.i(key); },
                       [](auto& out, cr559::Scalar value) { out.i(value); });
    }

    int callbacks = 0;
    int captures = 0;
    int restores = 0;
    int commits = 0;

private:
    cr559::Map map_, alias_;
    cr559::Model model_, saved_model_;
    std::optional<cr559::Map::Snapshot> checkpoint_;
    cr559::Scalar next_ = 0;
    cr559::Scalar saved_next_ = 0;
};
} // namespace

int main() {
    try {
        cr559::Session session;
        GrowthFillHost host;
        const auto bars = a5_fixture::bars(12);
        host.run(bars.data(), static_cast<int>(bars.size()));
        cr559::require(host.last_error().empty(), host.last_error().c_str());
        cr559::require(host.last_run_status() == 0, "growth calc-on-fill run did not complete");
        host.verify();
        cr559::require(session.store.arena_growths > 1 && session.store.index_growths > 1,
                       "calc-on-fill row never grew and rehashed actual map roots");
        std::printf("CR559 fill callbacks=%d captures=%d restores=%d commits=%d\n",
                    host.callbacks, host.captures, host.restores, host.commits);
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "CR559 fill FAIL: %s\n", error.what());
        return 1;
    }
}
