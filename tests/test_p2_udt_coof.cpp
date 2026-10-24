#define PINEFORGE_TEST_ALLOCATION_BYTES
#include "global_allocation_replacement.hpp"
#include "p2_a5matudt_store.hpp"
#include "p2_a5udt_views.hpp"
#include <pineforge/source/pine_strategy_host.hpp>
#include <algorithm>
#include <array>
#include <cinttypes>
#include <cstdio>
#include <memory>
#include <vector>

namespace {
using pineforge::detail::o1::Id;
using p2_a5::require;
std::size_t requested = 0;
void recorded(std::size_t n) { requested += n; }
struct Count {
    Count() {
        requested = 0;
        global_allocation::on_recorded = &recorded;
        global_allocation::recording = true;
    }
    ~Count() {
        global_allocation::recording = false;
        global_allocation::on_recorded = nullptr;
    }
};

class Host final : public pineforge::source::PineStrategyHost {
    pineforge::detail::o1::UdtGraph& graph_;
    p2_a5::IntMap map_;
    Id holder_, root_;
    std::unique_ptr<pineforge::detail::o1::Tx> transaction_;
    std::uint64_t checkpoint_hash_ = 0;
    double checkpoint_value_ = 0;

    Id child(double value) {
        const Id vector = graph_.make(4 * sizeof(double));
        const std::array<double, 4> values{{value, -0.0, 0.125, -17.25}};
        graph_.write(vector, 0, values.data(), sizeof(values));
        const std::array<char, 5> bytes{{'a', '\0', 'b', 'c', 'd'}};
        const Id text = graph_.make(bytes.size());
        graph_.write(text, 0, bytes.data(), bytes.size());
        const Id result = graph_.make(sizeof(double), 2);
        graph_.set_scalar(result, value);
        graph_.set_edge(result, 0, vector); graph_.set_edge(result, 1, text);
        return result;
    }
    void checkpoint() {
        if (transaction_) { transaction_->commit(); transaction_.reset(); }
        checkpoint_hash_ = graph_.hash_state();
        checkpoint_value_ = graph_.scalar<double>(graph_.edge(holder_, 0));
        {
            Count window;
            transaction_ = graph_.begin();
        }
        largest_checkpoint = std::max(largest_checkpoint, requested);
    }
public:
    int snapshots = 0, restores = 0, executions = 0, deletions = 0;
    std::size_t largest_checkpoint = 0, largest_restore = 0;
    explicit Host(pineforge::detail::o1::UdtGraph& graph)
        : graph_(graph), map_(graph), holder_(graph.make(0, 2)), root_(graph.root(map_.id())) {
        const Id shared = child(17.25);
        graph_.set_edge(holder_, 0, shared); graph_.set_edge(holder_, 1, shared);
        map_.put(11, holder_);
        pineforge::source::PineStrategyConfig config{};
        config.calc_on_order_fills = true;
        configure_pine_strategy(config);
    }
    ~Host() override { if (transaction_) transaction_->rollback(); }
    void snapshot_script_state() override { ++snapshots; checkpoint(); }
    void commit_script_state() override { checkpoint(); }
    void restore_script_state() override {
        if (!transaction_) return;
        ++restores;
        {
            Count window;
            transaction_->rollback();
            transaction_.reset();
        }
        largest_restore = std::max(largest_restore, requested);
        require(graph_.hash_state() == checkpoint_hash_, "calc-on-fill did not restore complete graph");
        require(map_.get(11) == holder_, "map value root rollback");
        const Id a = graph_.edge(holder_, 0), b = graph_.edge(holder_, 1);
        require(a == b && graph_.scalar<double>(b) == checkpoint_value_, "aliased child pre-rollback value");
        {
            Count window;
            transaction_ = graph_.begin();
        }
        largest_checkpoint = std::max(largest_checkpoint, requested);
    }
    void on_source_bar(const pineforge::Bar&) override {
        ++executions;
        const Id old = graph_.edge(holder_, 0);
        require(old == graph_.edge(holder_, 1), "input alias identity changed");
        graph_.set_scalar(old, 99.0 + pine_bar_index());
        const Id vector = graph_.edge(old, 0), text = graph_.edge(old, 1);
        graph_.set_scalar(vector, -99.0);
        const char changed = 'z'; graph_.write(text, 0, &changed, 1);
        graph_.set_edge(holder_, 0, 0); graph_.set_edge(holder_, 1, 0);
        require(graph_.collect() == 3, "speculation must delete the child, vector and string");
        require(!graph_.alive(old) && !graph_.alive(vector) && !graph_.alive(text), "speculative deletion never happened");
        ++deletions;
        const Id replacement = child(pine_bar_index() + 0.25);
        require(replacement != old, "deleted id reused in a fill calculation");
        graph_.set_edge(holder_, 0, replacement); graph_.set_edge(holder_, 1, replacement);
        map_.put(22, holder_);
        if (pine_bar_index() % 2) require(map_.erase(22), "map erase did not run");
        if (pine_bar_index() % 4 == 0 && live_position_size() == 0) strategy_entry("L", true);
        if (pine_bar_index() % 4 == 2 && live_position_size() > 0) strategy_close("L");
    }
};
}

int main() {
    try {
        std::vector<pineforge::Bar> bars;
        for (int i = 0; i < 12; ++i)
            bars.push_back({100.0, 102.0, 99.0, 101.0, 1000.0,
                            INT64_C(1700000100000) + i * INT64_C(60000)});
        p2_a5::StoreFixture fixture;
        Host host(fixture.runtime.graph());
        host.run(bars.data(), static_cast<int>(bars.size()), "1", "1", false);
        require(host.last_error().empty(), host.last_error().c_str());
        require(host.restores > 0 && host.executions > static_cast<int>(bars.size()), "real fill recalculation did not run");
        require(host.deletions == host.executions && host.trade_count() > 0, "graph deletion/trade population not reached");
        // One fixed transaction handle (store pointer, flags) plus allocator
        // control/rounding. No term depends on vertices, edges or payload bytes.
        constexpr std::size_t checkpoint_bound = 256;
        require(host.largest_checkpoint <= checkpoint_bound && host.largest_restore <= checkpoint_bound,
                "graph checkpoint/restore allocates a native graph");
        std::printf("P2-A5 graph coof snapshots=%d restores=%d executions=%d deletions=%d"
                    " checkpoint_bytes=%zu restore_bytes=%zu bound=%zu\n",
                    host.snapshots, host.restores, host.executions, host.deletions,
                    host.largest_checkpoint, host.largest_restore, checkpoint_bound);
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "P2-A5 graph coof: %s\n", error.what());
        return 1;
    }
}
