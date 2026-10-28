// UDT graph growth rows (100,000 and 1,000,000 bars) through a real native host
// over synthetic deterministic bars.
//
// Script shape, one execution per bar:
//   type Pivot
//       int   bar
//       float price
//       Pivot prev            // edge 0: the previous bar's object, na on bar 0
//       array<float> history  // edge 1: nested numeric vector, 4 values
//       string label          // edge 2: nested string, 32 bytes
//       array<float> same     // edge 3: the SAME vector again (an alias)
//   var Pivot last = na
//   last := Pivot.new(bar_index, close, last, history, label, history)
// Every object stays reachable from `last`, so the live population grows with
// bars and nothing is released before the run ends.
// This is a resident-window claim. In a jailed run scratch backing is RAM;
// retaining this entire graph grows the backing's memory charge with bars.
//
// LIVE-OBJECT LIMIT. Every node is one Store object and a Store admits at most
// Shape::max_live_objects live objects (default 1,048,576, the cap). The graph
// holds its control record, its root and three nodes per bar, so the 100,000-
// bar row stays below the limit. The 1,000,000-bar row reaches it: the bar
// that would create the (limit + 1)-th object stops the run with the typed
// cohort refusal, its caller scope rolls back, and every earlier bar's
// objects stay exact. The admitted bar count is derived from the limit below.
//
// FAIL-BEFORE (behavioural). Without udt_graph.hpp (base f66318a3) this file
// compiles in its generated-container mode only: a C++ struct per object that
// owns a std::vector<double> and a std::string, the objects retained in a
// std::vector (the shape of tests/test_generic_matrix_udt.cpp's Pivot). It
// reaches every bar and every late read, then fails the resident bound.
// `--native` runs the same control on the new tree. (That base compile was
// recorded before the pooled-handle Store; the Store test double included
// here now overrides that Store's hooks, so `--native` is the RED control.)
//
// GREEN. The same host callback builds the graph through UdtGraph over a Store
// of fixed Shape. Residency = the Store's fixed workspace + the heap an
// operation holds, both bounded from the Shape and the o1 contract alone
// (a5u_test_store.hpp), never from a measurement or from N.
#pragma once

#define PINEFORGE_TEST_ALLOCATION_BYTES
#include "global_allocation_replacement.hpp"
#include "a5u_test_store.hpp"
#include <pineforge/native_host.hpp>

#if __has_include(<pineforge/detail/udt_graph.hpp>)
#include <pineforge/detail/udt_graph.hpp>
#define PF_UDT_HAS_GRAPH 1
#else
#define PF_UDT_HAS_GRAPH 0
#endif

#include <pineforge/run_failure.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace a5u_growth {
using a5u_test::require;

// 256 resident pages hold the Store's hot index paths for this workload; the
// bound below is derived from that Shape and does not move with N.
inline constexpr std::uint32_t kResidentPages = 256;
inline constexpr std::uint64_t kStoreBound = a5u_test::store_bound(kResidentPages);
inline constexpr std::uint64_t kResidentBound = kStoreBound + a5u_test::kOperationHeapBound;
// Logical field bytes one bar adds: vector 4 doubles + label 32 bytes +
// object fields (int64, double) + the object's four edge ids.
inline constexpr std::uint64_t kLogicalBytesPerBar = 32 + 32 + 16 + 4 * 8;

inline double price(std::size_t bar) {
    // Unique exact binary fractions: a store that repeated a window of old
    // objects could not pass the late reads.
    return 100.0 + static_cast<double>(bar) * 2.0 +
           static_cast<double>((bar * 37) % 8) * 0.125;
}
inline std::array<double, 4> history(std::size_t bar) {
    return {{price(bar), -0.0, -price(bar), static_cast<double>(bar) * 0.25}};
}
inline std::array<unsigned char, 32> label(std::size_t bar) {
    std::array<unsigned char, 32> result{};
    for (std::size_t i = 0; i < result.size(); ++i) {
        const std::uint64_t at = static_cast<std::uint64_t>(bar) * result.size() + i;
        result[i] = static_cast<unsigned char>((at * 131 + at / 251) % 256);
    }
    return result;
}

// Retained heap of the family's own operations. Only family calls record:
// caller-owned bars and the host's own allocations are outside the window.
inline std::size_t heap_base = 0;
inline std::size_t heap_peak = 0;
inline std::size_t window_base = 0;
inline std::size_t operation_peak = 0;
inline void allocated(std::size_t) {
    const std::size_t live = global_allocation::live_bytes;
    if (live > heap_base && live - heap_base > heap_peak) heap_peak = live - heap_base;
    if (live > window_base && live - window_base > operation_peak) operation_peak = live - window_base;
}
struct OperationWindow {
    bool previous = global_allocation::recording;
    std::size_t previous_base = window_base;
    OperationWindow() {
        window_base = global_allocation::live_bytes;
        global_allocation::recording = true;
    }
    ~OperationWindow() {
        window_base = previous_base;
        global_allocation::recording = previous;
    }
};

class NativeFamily {
    struct Pivot {
        std::int64_t bar;
        double price;
        std::int64_t previous; // index, so destruction needs no deep stack
        std::vector<double> history;
        std::string label;
    };
    std::vector<Pivot> objects_;

public:
    void append(std::size_t bar, double close) {
        const auto numbers = history(bar);
        const auto bytes = label(bar);
        objects_.push_back({static_cast<std::int64_t>(bar), close,
                            static_cast<std::int64_t>(bar) - 1,
                            std::vector<double>(numbers.begin(), numbers.end()),
                            std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size())});
    }
    void verify(std::size_t count) const {
        require(objects_.size() == count, "native object creation was refused");
        for (std::size_t i = 0; i < count; ++i) {
            const auto& object = objects_[i];
            const auto numbers = history(i);
            const auto bytes = label(i);
            require(object.bar == static_cast<std::int64_t>(i) && object.price == price(i) &&
                        object.previous == static_cast<std::int64_t>(i) - 1,
                    "native old scalar/chain read");
            require(object.history.size() == numbers.size() &&
                        std::memcmp(object.history.data(), numbers.data(), sizeof(numbers)) == 0,
                    "native old nested vector read");
            require(object.label.size() == bytes.size() &&
                        std::memcmp(object.label.data(), bytes.data(), bytes.size()) == 0,
                    "native old nested string read");
        }
    }
    void finish() {
        std::vector<Pivot> empty;
        empty.swap(objects_);
    }
};

#if PF_UDT_HAS_GRAPH
class GraphFamily {
    using Id = pineforge::detail::o1::Id;
    using UdtType = pineforge::detail::o1::UdtType;
    struct Fields {
        std::int64_t bar;
        double price;
    };
    static constexpr UdtType kVector{1, sizeof(std::array<double, 4>), 0};
    static constexpr UdtType kString{2, sizeof(std::array<unsigned char, 32>), 0};
    static constexpr UdtType kPivot{3, sizeof(Fields), 4};
    pineforge::detail::o1::UdtGraph& graph_;
    a5u_test::CountingStore& operations_;
    Id root_;

public:
    GraphFamily(pineforge::detail::o1::UdtGraph& graph, a5u_test::CountingStore& operations)
        : graph_(graph), operations_(operations), root_(graph.root()) {}
    void append(std::size_t bar, double close) {
        const auto calls = operations_.data_calls;
        const auto scopes = operations_.begin_calls;
        const auto joins = operations_.joined_scopes;
        auto transaction = graph_.begin();
        const auto numbers = history(bar);
        const auto bytes = label(bar);
        const Id vector = graph_.make(kVector, numbers.data(), nullptr);
        const Id string = graph_.make(kString, bytes.data(), nullptr);
        const Fields fields{static_cast<std::int64_t>(bar), close};
        const std::array<Id, 4> children{{graph_.edge(root_, 0), vector, string, vector}};
        graph_.set_edge(root_, 0, graph_.make(kPivot, &fields, children.data()));
        transaction->commit();
        // make_node: control read + create + staged record/body write +
        // directory-head write. All three schemas fit its 512-byte staging
        // buffer (72-byte record plus the declared payload/edges above).
        constexpr std::uint64_t makes_per_bar = 3; // vector, string, Pivot
        constexpr std::uint64_t calls_per_make = 1 + 1 + 1 + 1;
        // validate_child: one header read per non-na Pivot child (prev,
        // vector, string, vector). Bar zero omits the prev header read.
        constexpr std::uint64_t child_validations = kPivot.edge_count;
        constexpr std::uint64_t root_read = 2; // edge: root header + edge
        // set_edge: child validation + root header + edge write.
        constexpr std::uint64_t root_rebind = 1 + 1 + 1;
        constexpr std::uint64_t append_calls =
            makes_per_bar * calls_per_make + child_validations + root_read + root_rebind;
        // The caller's one explicit handle per bar; each make() joins it
        // (Store::scope()) rather than opening a handle of its own.
        constexpr std::uint64_t append_begins = 1;
        require(operations_.data_calls - calls <= append_calls &&
                    operations_.begin_calls - scopes == append_begins &&
                    operations_.joined_scopes - joins == makes_per_bar,
                "per-bar graph append exceeded its constant Store operation bound");
    }
    void verify(std::size_t count) const {
        Id current = graph_.edge(root_, 0);
        for (std::size_t remaining = count; remaining > 0; --remaining) {
            const std::size_t bar = remaining - 1;
            require(current != 0, "old UDT object is na");
            const auto calls = operations_.data_calls;
            Fields fields{};
            std::array<Id, 4> children{};
            graph_.read_object(current, kPivot, &fields, children.data());
            require(fields.bar == static_cast<std::int64_t>(bar) && fields.price == price(bar),
                    "old UDT scalar read");
            require(children[1] != 0 && children[1] == children[3],
                    "late read split the aliased vector");
            const auto expected_numbers = history(bar);
            std::array<double, 4> numbers{};
            graph_.read_object(children[1], kVector, numbers.data(), nullptr);
            require(std::memcmp(numbers.data(), expected_numbers.data(), sizeof(numbers)) == 0,
                    "old nested vector bits");
            std::array<unsigned char, 32> bytes{};
            graph_.read_object(children[2], kString, bytes.data(), nullptr);
            require(bytes == label(bar), "old nested string bytes");
            // read_object: one staged Store read each for Pivot, vector,
            // string, with their header included rather than looked up again.
            constexpr std::uint64_t late_read_calls = 1 + 1 + 1;
            require(operations_.data_calls - calls == late_read_calls,
                    "late read repeated a node header lookup");
            current = children[0];
        }
        require(current == 0, "UDT chain lost its na terminator or holds extra objects");
    }
    std::uint64_t finish() {
        graph_.drop_root(root_);
        return graph_.collect();
    }
};
#endif

template<class Family>
class Host final : public pineforge::NativeStrategyHost {
    Family& family_;
    std::size_t callbacks_ = 0;

public:
    explicit Host(Family& family) : family_(family) {
        pineforge::NativeRunSpec spec;
        spec.identity = {"udt-growth", 1};
        spec.input_tf = "1";
        spec.script_tf = "1";
        spec.ticker = "UDT";
        spec.tickerid = "TEST:UDT";
        spec.type = "crypto";
        spec.currency = "USDT";
        spec.basecurrency = "ETH";
        spec.timezone = "UTC";
        spec.session = "24x7";
        spec.slot_label_policy = pineforge::NativeSlotLabelPolicy::FeedTolerant;
        spec.initial_capital = 1000000.0;
        spec.point_value = 1.0;
        spec.account_fx = 1.0;
        spec.price_tick = 0.125;
        spec.event_retention = pineforge::NativeEventRetention::Window;
        require(configure_native(spec).status == pineforge::NativeSetupStatus::Applied,
                "configure UDT growth host");
    }
    void on_native_bar(const pineforge::Bar& bar, const pineforge::NativeDecisionContext&) override {
        OperationWindow window;
        require(bar.close == price(callbacks_), "synthetic UDT bar sequence");
        family_.append(callbacks_, bar.close);
        ++callbacks_;
    }
    std::size_t callbacks() const { return callbacks_; }
};

// `admitted` bars complete. When it is below `count`, the next bar's callback
// throws `refusal` and the host records that exception as the run's failure.
template<class Family>
void drive(Family& family, std::size_t count, std::size_t admitted, const std::string& refusal) {
    std::vector<pineforge::Bar> bars(count);
    for (std::size_t i = 0; i < count; ++i) {
        const double value = price(i);
        bars[i] = {value, value + 1, value - 1, value, 1000,
                   INT64_C(1736121600000) + static_cast<std::int64_t>(i) * 60000};
    }
    Host<Family> host(family);
    host.run(bars.data(), static_cast<int>(count), "1", "1");
    if (admitted == count) {
        require(host.last_error().empty(), host.last_error().c_str());
    } else {
        require(host.last_error() == refusal &&
                    std::strcmp(pineforge::run_failure_code_of(host), "scratch_capacity_exceeded") == 0 &&
                    std::strcmp(pineforge::run_failure_args_of(host), "{\"resource\":\"cohort\"}") == 0,
                "live-object limit did not stop the run with the typed cohort refusal");
    }
    require(host.callbacks() == admitted, "UDT workload did not reach every admitted bar");
    OperationWindow window;
    family.verify(admitted); // every old object and byte, after the last admitted bar
}

#if PF_UDT_HAS_GRAPH
// Store objects the graph holds: its control record and root, then three
// nodes per bar (object, vector, string; the aliased vector is one node).
inline constexpr std::uint64_t kFixedObjects = 2;
inline constexpr std::uint64_t kNodesPerBar = 3;
// Bars a Store with this live-object limit admits. The bar that would create
// the (limit + 1)-th live object is refused and rolled back; earlier bars stay.
inline std::size_t admitted_bars(std::size_t count, std::uint64_t limit) {
    return static_cast<std::size_t>(
        std::min<std::uint64_t>(count, (limit - kFixedObjects) / kNodesPerBar));
}
#endif

inline int run(std::size_t count, int argc, char** argv) {
    try {
        require(argc == 1 || (argc == 2 && std::strcmp(argv[1], "--native") == 0),
                "usage: test_a5u_growth [--native]");
        const bool native = !PF_UDT_HAS_GRAPH || argc == 2;
#if PF_UDT_HAS_GRAPH
        // The test Shape keeps the default limit, which is the cap.
        const std::size_t admitted =
            native ? count : admitted_bars(count, pineforge::detail::o1::kMaxLiveObjectsCap);
#else
        const std::size_t admitted = count;
#endif
        // The logical field bytes alone cross the whole resident bound eight
        // times, so a GREEN cannot pass without spilling to the backing.
        require(admitted * kLogicalBytesPerBar > 8 * kResidentBound,
                "population cannot cross its bound");
        heap_base = global_allocation::live_bytes;
        heap_peak = 0;
        operation_peak = 0;
        global_allocation::on_recorded = &allocated;
        if (native) {
            NativeFamily family;
            drive(family, count, count, std::string());
            OperationWindow window;
            family.finish();
        } else {
#if PF_UDT_HAS_GRAPH
            a5u_test::StoreFixture fixture(kResidentPages);
            a5u_test::CountingStore operations(*fixture.store);
            pineforge::detail::o1::UdtGraph graph(operations);
            const auto empty_live = fixture.store->stats().bytes_live;
            const auto initial = fixture.store->stats();
            require(initial.max_live_objects == pineforge::detail::o1::kMaxLiveObjectsCap &&
                        initial.objects_live == 1,
                    "growth Store does not hold only its control record under the default limit");
            GraphFamily family(graph, operations);
            drive(family, count, admitted, a5u_test::object_limit_text(initial.max_live_objects));
            const auto stats = fixture.store->stats();
            std::printf("UDT bars=%zu admitted=%zu live_objects=%llu live_object_limit=%llu "
                        "live=%llu resident=%llu backing=%llu store_bound=%llu\n",
                        count, admitted, static_cast<unsigned long long>(stats.objects_live),
                        static_cast<unsigned long long>(stats.max_live_objects),
                        static_cast<unsigned long long>(stats.bytes_live),
                        static_cast<unsigned long long>(stats.bytes_resident),
                        static_cast<unsigned long long>(stats.bytes_backing),
                        static_cast<unsigned long long>(kStoreBound));
            require(stats.objects_live == kFixedObjects + kNodesPerBar * admitted,
                    "live objects differ from the admitted bars");
            require(stats.bytes_resident == initial.bytes_resident &&
                        stats.bytes_resident <= kStoreBound,
                    "real store workspace grew with objects");
            require(stats.bytes_live >= admitted * kLogicalBytesPerBar &&
                        stats.bytes_backing > 8 * kResidentBound,
                    "live graph population did not spill beyond the resident window");
            {
                OperationWindow window;
                // Every admitted bar's nodes plus the dropped root.
                require(family.finish() == kNodesPerBar * admitted + 1,
                        "collection released a different node count than the graph holds");
                require(fixture.store->stats().bytes_live == empty_live,
                        "unreachable UDT objects leaked");
                graph.close();
                require(fixture.store->stats().bytes_live == 0, "graph close retained records");
            }
            require(heap_peak <= a5u_test::kOperationHeapBound,
                    "graph retained heap across operation windows");
            require(operation_peak <= a5u_test::kOperationHeapBound,
                    "one graph operation held heap beyond its Tx handles");
            require(stats.bytes_resident + heap_peak <= kResidentBound,
                    "graph residency exceeds the derived fixed bound");
#endif
        }
        std::printf("UDT bars=%zu admitted=%zu retained_heap_peak=%zu operation_heap_peak=%zu "
                    "bound=%llu mode=%s\n",
                    count, admitted, heap_peak, operation_peak,
                    static_cast<unsigned long long>(kResidentBound),
                    native ? "native-RED" : "store-GREEN");
        require(heap_peak <= kResidentBound, "UDT owning fields retain heap proportional to bars");
        global_allocation::on_recorded = nullptr;
        return 0;
    } catch (const std::exception& error) {
        global_allocation::on_recorded = nullptr;
        std::fprintf(stderr, "UDT growth: %s\n", error.what());
        return 1;
    }
}

} // namespace a5u_growth
