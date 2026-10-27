#define PINEFORGE_TEST_ALLOCATION_BYTES
#include "global_allocation_replacement.hpp"
#include "a5u_test_store.hpp"
#include "a5u_graph_views.hpp"
#include <pineforge/source/pine_strategy_host.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <vector>

namespace {
using pineforge::detail::o1::Id;
using pineforge::detail::o1::UdtGraph;
using a5u_test::require;

std::size_t allocated_bytes = 0;
void allocated(std::size_t n) { allocated_bytes += n; }
struct Count {
    Count() {
        allocated_bytes = 0;
        global_allocation::recording = true;
        global_allocation::on_recorded = &allocated;
    }
    ~Count() {
        global_allocation::on_recorded = nullptr;
        global_allocation::recording = false;
    }
};

std::array<double, 4> history(double value) { return {{value, -0.0, -value, value * 0.25}}; }
std::array<unsigned char, 17> label(std::uint64_t seed) {
    std::array<unsigned char, 17> result{};
    for (std::size_t i = 0; i < result.size(); ++i)
        result[i] = static_cast<unsigned char>((seed * 131 + i * 37) % 256);
    return result;
}

class Host final : public pineforge::source::PineStrategyHost {
    UdtGraph& graph_;
    a5u_test::IdMap map_;
    Id holder_, root_;
    std::unique_ptr<pineforge::detail::o1::Tx> transaction_;
    struct Expected {
        Id child = 0;
        Id vector = 0;
        Id text = 0;
        double value = 0;
        std::uint64_t seed = 0;
        bool extra_key = false;
    } current_, checkpoint_;
    std::uint64_t checkpoint_hash_ = 0;

    Expected child(double value, std::uint64_t seed) {
        Expected result;
        result.value = value;
        result.seed = seed;
        const auto numbers = history(value);
        const auto bytes = label(seed);
        result.vector = graph_.make(sizeof(numbers));
        graph_.write(result.vector, 0, numbers.data(), sizeof(numbers));
        result.text = graph_.make(bytes.size());
        graph_.write(result.text, 0, bytes.data(), bytes.size());
        result.child = graph_.make(sizeof(value), 2);
        graph_.set_scalar(result.child, value);
        graph_.set_edge(result.child, 0, result.vector);
        graph_.set_edge(result.child, 1, result.text);
        return result;
    }
    void verify() const {
        require(map_.get(11) == holder_ && map_.get(33) == holder_, "map aliases lost a root");
        require((map_.get(22) == holder_) == current_.extra_key, "map insertion/erase rollback");
        require(graph_.edge(holder_, 0) == current_.child &&
                    graph_.edge(holder_, 1) == current_.child,
                "aliased UDT child identity");
        const auto value = graph_.scalar<double>(current_.child);
        require(std::memcmp(&value, &current_.value, sizeof(value)) == 0, "pre-rollback scalar bytes");
        require(graph_.edge(current_.child, 0) == current_.vector &&
                    graph_.edge(current_.child, 1) == current_.text, "nested child identity");
        const auto expected_numbers = history(current_.value);
        std::array<double, 4> numbers{};
        graph_.read(current_.vector, 0, numbers.data(), sizeof(numbers));
        require(std::memcmp(numbers.data(), expected_numbers.data(), sizeof(numbers)) == 0,
                "pre-rollback vector bytes");
        std::array<unsigned char, 17> bytes{};
        graph_.read(current_.text, 0, bytes.data(), bytes.size());
        require(bytes == label(current_.seed), "pre-rollback string bytes");
    }
    void checkpoint() {
        if (transaction_) {
            Count window;
            transaction_->commit();
            transaction_.reset();
        }
        checkpoint_ = current_; // Independent input-derived expectation, not graph reads.
        checkpoint_hash_ = graph_.hash_state();
        {
            Count window;
            transaction_ = graph_.begin();
        }
        largest_checkpoint = std::max(largest_checkpoint, allocated_bytes);
    }

public:
    int snapshots = 0, restores = 0, executions = 0, deletions = 0;
    std::size_t largest_checkpoint = 0, largest_restore = 0;
    explicit Host(UdtGraph& graph)
        : graph_(graph), map_(graph), holder_(graph.make(0, 2)), root_(graph.root(map_.id())) {
        current_ = child(17.25, 319);
        graph_.set_edge(holder_, 0, current_.child);
        graph_.set_edge(holder_, 1, current_.child);
        map_.put(11, holder_);
        map_.put(33, holder_);
        pineforge::source::PineStrategyConfig config{};
        config.calc_on_order_fills = true;
        configure_pine_strategy(config);
    }
    ~Host() override {
        if (transaction_) {
            Count window;
            transaction_->rollback();
            transaction_.reset();
        }
    }
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
        largest_restore = std::max(largest_restore, allocated_bytes);
        current_ = checkpoint_;
        require(graph_.hash_state() == checkpoint_hash_, "calc-on-fill graph bytes changed");
        verify();
        {
            Count window;
            transaction_ = graph_.begin();
        }
        largest_checkpoint = std::max(largest_checkpoint, allocated_bytes);
    }
    void on_source_bar(const pineforge::Bar&) override {
        ++executions;
        verify();
        const Expected old = current_;
        graph_.set_scalar(old.child, old.value + executions);
        graph_.set_scalar(old.vector, -old.value);
        const auto changed = label(old.seed + executions);
        graph_.write(old.text, 0, changed.data(), changed.size());
        graph_.set_edge(holder_, 0, 0);
        graph_.set_edge(holder_, 1, 0);
        // Deletion cardinality comes from the three distinct old owning ids.
        const std::array<Id, 3> old_ids{{old.child, old.vector, old.text}};
        require(graph_.collect() == old_ids.size(), "speculative nested graph deletion count");
        for (const Id id : old_ids) require(!graph_.alive(id), "speculative deletion did not run");
        ++deletions;
        current_ = child(pine_bar_index() + 0.25, static_cast<std::uint64_t>(executions));
        require(current_.child > old.child, "fill calculation recycled a dead id");
        graph_.set_edge(holder_, 0, current_.child);
        graph_.set_edge(holder_, 1, current_.child);
        map_.put(22, holder_);
        current_.extra_key = true;
        if (pine_bar_index() % 2) {
            require(map_.erase(22), "map erase was not exercised");
            current_.extra_key = false;
        }
        if (pine_bar_index() % 4 == 0 && live_position_size() == 0) strategy_entry("L", true);
        if (pine_bar_index() % 4 == 2 && live_position_size() > 0) strategy_close("L");
    }
};
} // namespace

int main() {
    try {
        a5u_test::StoreFixture fixture;
        UdtGraph graph(*fixture.store);
        {
            Host host(graph);
            std::vector<pineforge::Bar> bars(12);
            for (std::size_t i = 0; i < bars.size(); ++i)
                bars[i] = {100.0, 102.0, 99.0, 101.0, 1000.0,
                           INT64_C(1700000100000) + static_cast<std::int64_t>(i) * 60000};
            host.run(bars.data(), static_cast<int>(bars.size()), "1", "1", false);
            require(host.last_error().empty(), host.last_error().c_str());
            require(host.restores > 0 && host.executions > static_cast<int>(bars.size()) &&
                        host.trade_count() > 0, "real calc-on-order-fills was not reached");
            require(host.deletions == host.executions, "fill workload omitted graph deletion");
            // StoreTx is one pointer, vptr and flags. One 256-byte transfer
            // buffer is already more space than its checkpoint needs.
            constexpr std::size_t checkpoint_bound = 256;
            require(host.largest_checkpoint <= checkpoint_bound && host.largest_restore <= checkpoint_bound,
                    "checkpoint copied native graph state");
            std::printf("A5U coof snapshots=%d restores=%d executions=%d checkpoint=%zu restore=%zu\n",
                        host.snapshots, host.restores, host.executions,
                        host.largest_checkpoint, host.largest_restore);
        }
        graph.close();
        require(fixture.store->stats().bytes_live == 0, "calc-on-fill graph was not closed");
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "A5U calc-on-fill: %s\n", error.what());
        return 1;
    }
}
