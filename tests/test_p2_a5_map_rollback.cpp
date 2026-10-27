#include "p2_a5strmap_fixture.hpp"
#include <pineforge/map.hpp>
#include <optional>
#include <utility>

#if __has_include(<pineforge/script_string.hpp>)
#include "p2_a5strmap_store_fixture.hpp"
#define PF_A5_EXTERNAL_MAP 1
#endif

namespace {
using namespace pineforge;
using Map = PineMap<int, std::string>;
using Oracle = std::vector<std::pair<int, std::string>>;
constexpr int entry_count = 128;

class RollbackHost final : public source::PineStrategyHost {
public:
    RollbackHost() {
        // Carry the even-bar market entry to the next open. Terminal
        // process_orders_on_close fills do not run fill recalculations.
        a5_fixture::configure(*this, true, /*on_close=*/false);
#ifdef PF_A5_EXTERNAL_MAP
        session_ = std::make_unique<a5_fixture::Session>();
        auto holder = std::make_shared<detail::MapStoreHolder>(*session_->store);
        detail::MapStoreScope scope(holder);
#endif
        map_ = Map::new_();
        alias_ = map_;
        for (int i = 0; i < entry_count; ++i) {
            std::string text(4096, static_cast<char>('a' + i % 26));
            text[11] = '\0';
            (void)map_.put(i, text);
            oracle_.emplace_back(i, std::move(text));
        }
    }
    void snapshot_script_state() override {
        checkpoint_oracle_ = oracle_; // Independent oracle, outside the census.
        checkpoint_sequence_ = sequence_;
        const auto before = a5_heap::live.load();
        a5_heap::reset_peak();
        {
            a5_heap::Window count;
            checkpoint_.emplace(map_.snapshot());
        }
        capture_peak_ = std::max(capture_peak_,
            a5_heap::growth(a5_heap::peak.load(), before));
        ++captures_;
    }
    void restore_script_state() override {
        a5_fixture::require(checkpoint_.has_value(), "host restore before checkpoint");
        {
            a5_heap::Window count;
            map_.restore(*checkpoint_);
        }
        oracle_ = checkpoint_oracle_;
        sequence_ = checkpoint_sequence_;
        exact(map_);
        exact(alias_); // Restore must update the original ID in place.
        ++restores_;
    }
    void commit_script_state() override {
        checkpoint_oracle_ = oracle_;
        checkpoint_sequence_ = sequence_;
        a5_heap::Window count;
        checkpoint_.emplace(map_.snapshot());
        ++commits_;
    }
    void on_source_bar(const Bar& bar) override {
        ++callbacks_;
        // Every calculation changes an owning payload and insertion order.
        const auto removed = oracle_.front();
        oracle_.erase(oracle_.begin());
        std::string changed = removed.second;
        changed[19] = static_cast<char>('A' + sequence_++ % 26);
        oracle_.emplace_back(removed.first, changed);
        {
            a5_heap::Window count;
            (void)map_.remove(removed.first);
            (void)map_.put(removed.first, std::move(changed));
        }
        if (pine_bar_index() % 2 == 0 && live_position_size() == 0)
            strategy_entry("rollback", true, na<double>(), na<double>(), 1.0);
        if (pine_bar_index() % 2 != 0 && live_position_size() != 0)
            strategy_close("rollback");
        (void)bar;
    }
    void verify() const {
        std::printf("A5ROLLBACK callbacks=%d trades=%d captures=%d restores=%d commits=%d capture_peak=%zu\n",
                    callbacks_, trade_count(), captures_, restores_, commits_, capture_peak_);
        a5_fixture::require(captures_ > 0 && restores_ > 0 && commits_ > 0,
                            "real host did not execute rollback hooks");
        a5_fixture::require(callbacks_ > 12 && trade_count() > 0,
                            "fixture did not execute fill recalculations");
        exact(map_); exact(alias_);
        a5_fixture::require(capture_peak_ <= a5_fixture::operation_ceiling,
                            "calc-on-fill checkpoint copies payload-sized resident state");
    }
    void hash_host_extension(BrokerStateHashSink& sink) const override {
        source::PineStrategyHost::hash_host_extension(sink);
        sink.s("p2-a5strmap/rollback"); sink.i(sequence_);
        for (const auto& entry : oracle_) { sink.i(entry.first); sink.s(map_.get(entry.first)); }
    }
private:
    void exact(const Map& map) const {
        const auto keys = map.keys();
        const auto values = map.values();
        a5_fixture::require(keys.size() == oracle_.size(), "rollback changed key count");
        for (std::size_t i = 0; i < oracle_.size(); ++i) {
            a5_fixture::require(keys[i] == oracle_[i].first, "rollback changed insertion order");
            a5_fixture::require(values[i] == oracle_[i].second &&
                                    map.get(oracle_[i].first) == oracle_[i].second,
                                "rollback changed exact payload bytes");
        }
    }
#ifdef PF_A5_EXTERNAL_MAP
    std::unique_ptr<a5_fixture::Session> session_;
#endif
    Map map_, alias_;
    std::optional<Map::Snapshot> checkpoint_;
    Oracle oracle_, checkpoint_oracle_;
    int sequence_ = 0, checkpoint_sequence_ = 0;
    int callbacks_ = 0, captures_ = 0, restores_ = 0, commits_ = 0;
    std::size_t capture_peak_ = 0;
};
} // namespace

int main() {
    try {
        const auto input = a5_fixture::bars(12);
        RollbackHost host;
        host.run(input.data(), static_cast<int>(input.size()));
        a5_fixture::require(host.last_error().empty(), host.last_error().c_str());
        host.verify();
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "A5ROLLBACK FAIL: %s\n", error.what());
        return 1;
    }
}
