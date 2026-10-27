// U07 rollback inside a real calc_on_order_fills run.
//
// The host maps the engine's generated-state hooks (engine.hpp
// snapshot/restore/commit_script_state) onto ONE Store transaction: snapshot
// and commit commit the open scope and begin the next; restore rolls it back
// and begins the next. Every script execution mutates an ALIASED child (two
// holder edges reach it), its nested vector and nested string, unlinks it,
// collects (three releases), creates a replacement and inserts/erases a map
// key whose value is a UDT. A restore must bring back the exact checkpoint
// graph: bytes, aliases, deleted nodes and map keys. Expectations come from
// input-derived values tracked beside the graph, never from graph reads.
//
// FAIL-BEFORE. Base f66318a3 has no pineforge/detail/udt_graph.hpp (missing
// header diagnostic). Behavioural RED: mutations rollback-commits and
// checkpoint-copies.
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
#include <limits>
#include <memory>
#include <vector>

namespace {
using pineforge::detail::o1::Id;
using pineforge::detail::o1::UdtGraph;
using pineforge::detail::o1::UdtType;
using a5u_test::require;

// One hook window: heap allocation calls and requested bytes.
std::size_t window_bytes = 0;
void allocated(std::size_t n) { window_bytes += n; }
struct Window {
    std::size_t start = global_allocation::allocations;
    std::size_t& calls;
    std::size_t& bytes;
    Window(std::size_t& calls_out, std::size_t& bytes_out) : calls(calls_out), bytes(bytes_out) {
        window_bytes = 0;
        global_allocation::recording = true;
        global_allocation::on_recorded = &allocated;
    }
    ~Window() {
        global_allocation::on_recorded = nullptr;
        global_allocation::recording = false;
        calls = global_allocation::allocations - start;
        bytes = window_bytes;
    }
};

std::array<double, 4> history(double value) { return {{value, -0.0, -value, value * 0.25}}; }
std::array<unsigned char, 17> label(std::uint64_t seed) {
    std::array<unsigned char, 17> result{};
    for (std::size_t i = 0; i < result.size(); ++i)
        result[i] = static_cast<unsigned char>((seed * 131 + i * 37) % 256);
    return result;
}

constexpr UdtType kVector{1, sizeof(std::array<double, 4>), 0};
constexpr UdtType kText{2, sizeof(std::array<unsigned char, 17>), 0};
constexpr UdtType kChild{3, sizeof(double), 2};

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
        result.vector = graph_.make(kVector, numbers.data(), nullptr);
        result.text = graph_.make(kText, bytes.data(), nullptr);
        const std::array<Id, 2> children{{result.vector, result.text}};
        result.child = graph_.make(kChild, &value, children.data());
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
                    graph_.edge(current_.child, 1) == current_.text,
                "nested child identity");
        const auto expected_numbers = history(current_.value);
        std::array<double, 4> numbers{};
        graph_.read(current_.vector, 0, numbers.data(), sizeof(numbers));
        require(std::memcmp(numbers.data(), expected_numbers.data(), sizeof(numbers)) == 0,
                "pre-rollback vector bytes");
        std::array<unsigned char, 17> bytes{};
        graph_.read(current_.text, 0, bytes.data(), bytes.size());
        require(bytes == label(current_.seed), "pre-rollback string bytes");
    }
    void begin_scope() {
        std::size_t calls = 0, bytes = 0;
        {
            Window window(calls, bytes);
            transaction_ = graph_.begin();
        }
        begin_calls_min = std::min(begin_calls_min, calls);
        begin_calls_max = std::max(begin_calls_max, calls);
        begin_bytes_max = std::max(begin_bytes_max, bytes);
    }
    void finish_scope(bool commit) {
        std::size_t calls = 0, bytes = 0;
        {
            Window window(calls, bytes);
            if (commit) transaction_->commit();
            else transaction_->rollback();
            transaction_.reset();
        }
        finish_calls_max = std::max(finish_calls_max, calls);
    }
    void checkpoint() {
        if (transaction_) finish_scope(true);
        checkpoint_ = current_; // input-derived expectation, not graph reads
        checkpoint_hash_ = graph_.hash_state();
        begin_scope();
    }

public:
    int snapshots = 0, restores = 0, executions = 0, deletions = 0;
    std::size_t begin_calls_min = std::numeric_limits<std::size_t>::max();
    std::size_t begin_calls_max = 0, begin_bytes_max = 0, finish_calls_max = 0;

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
            transaction_->rollback();
            transaction_.reset();
        }
    }
    Id root() const { return root_; }
    void snapshot_script_state() override {
        ++snapshots;
        checkpoint();
    }
    void commit_script_state() override { checkpoint(); }
    void restore_script_state() override {
        if (!transaction_) return;
        ++restores;
        finish_scope(false);
        current_ = checkpoint_;
        require(graph_.hash_state() == checkpoint_hash_, "calc-on-fill graph bytes changed");
        verify();
        begin_scope();
    }
    void on_source_bar(const pineforge::Bar&) override {
        ++executions;
        verify();
        const Expected old = current_;
        graph_.set_scalar(old.child, old.value + executions);
        graph_.set_scalar(old.vector, -old.value);
        const auto changed = label(old.seed + static_cast<std::uint64_t>(executions));
        graph_.write(old.text, 0, changed.data(), changed.size());
        graph_.set_edge(holder_, 0, 0);
        graph_.set_edge(holder_, 1, 0);
        // Deletion cardinality: the three distinct old owning ids; the child
        // reached through two holder edges is released once.
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
        const auto empty_live = fixture.store->stats().bytes_live;
        Id root = 0;
        {
            Host host(graph);
            std::vector<pineforge::Bar> bars(12);
            for (std::size_t i = 0; i < bars.size(); ++i)
                bars[i] = {100.0, 102.0, 99.0, 101.0, 1000.0,
                           INT64_C(1700000100000) + static_cast<std::int64_t>(i) * 60000};
            host.run(bars.data(), static_cast<int>(bars.size()), "1", "1", false);
            require(host.last_error().empty(), host.last_error().c_str());
            require(host.restores > 0 && host.executions > static_cast<int>(bars.size()) &&
                        host.trade_count() > 0,
                    "real calc-on-order-fills was not reached");
            require(host.deletions == host.executions, "fill workload omitted graph deletion");
            // o1_store.hpp: begin() makes exactly one heap allocation (its Tx
            // handle); commit/rollback make none. A handle is a vtable pointer,
            // a store pointer and two flags, far below 256 bytes, while one
            // graph node record alone is 72 bytes and this graph holds a dozen:
            // a checkpoint that copied graph state natively breaks either line.
            require(host.begin_calls_min == 1 && host.begin_calls_max == 1 &&
                        host.finish_calls_max == 0,
                    "checkpoint/restore allocated beyond one Tx handle");
            require(host.begin_bytes_max <= 256, "checkpoint copied native graph state");
            std::printf("A5U coof snapshots=%d restores=%d executions=%d handle_bytes=%zu\n",
                        host.snapshots, host.restores, host.executions, host.begin_bytes_max);
            root = host.root();
        } // ~Host rolls back the scope its last checkpoint opened
        graph.drop_root(root);
        require(graph.collect() > 0 && fixture.store->stats().bytes_live == empty_live,
                "dropping the map root did not reclaim the whole graph");
        graph.close();
        require(fixture.store->stats().bytes_live == 0, "calc-on-fill graph was not closed");
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "A5U calc-on-fill: %s\n", error.what());
        return 1;
    }
}
