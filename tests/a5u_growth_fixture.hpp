// Behavioral fail-before: without udt_graph.hpp this compiles and drives the
// owning generated-container idiom. It reaches every bar and late read, then
// fails the resident bound. --native runs that same control on the new tree.
#pragma once

#define PINEFORGE_TEST_ALLOCATION_BYTES
#include "global_allocation_replacement.hpp"
#include "a5u_test_store.hpp"
#include <pineforge/native_host.hpp>

#if __has_include(<pineforge/detail/udt_graph.hpp>)
#include <pineforge/detail/udt_graph.hpp>
#define PF_A5U_HAS_GRAPH 1
#else
#define PF_A5U_HAS_GRAPH 0
#endif

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace a5u_growth {
using a5u_test::require;

inline double price(std::size_t bar) {
    return 100.0 + static_cast<double>(bar) * 2.0 +
           static_cast<double>((bar * 37) % 8) * 0.125;
}
inline std::array<double, 4> history(std::size_t bar) {
    return {{price(bar), -0.0, -price(bar), static_cast<double>(bar) * 0.25}};
}
inline unsigned char text_byte(std::uint64_t at) {
    return static_cast<unsigned char>((at * 131 + at / 251) % 256);
}
inline std::array<unsigned char, 32> label(std::size_t bar) {
    std::array<unsigned char, 32> result{};
    for (std::size_t i = 0; i < result.size(); ++i) result[i] = text_byte(bar * result.size() + i);
    return result;
}

inline std::size_t peak_heap = 0;
inline void allocated(std::size_t) {
    if (global_allocation::live_bytes > peak_heap) peak_heap = global_allocation::live_bytes;
}
struct OperationWindow {
    bool previous = global_allocation::recording;
    OperationWindow() { global_allocation::recording = true; }
    ~OperationWindow() { global_allocation::recording = previous; }
};

// Same owning fields as tests/test_generic_matrix_udt.cpp's Pivot, plus an
// explicit previous-object index so native destruction needs no deep stack.
class NativeFamily {
    struct Udt {
        std::int64_t bar;
        double price;
        std::int64_t previous;
        std::vector<double> history;
        std::string label;
    };
    std::vector<Udt> objects_;
    std::string text_;
public:
    void append(std::size_t bar, double value) {
        const auto numbers = history(bar);
        const auto bytes = label(bar);
        objects_.push_back({static_cast<std::int64_t>(bar), value,
                            static_cast<std::int64_t>(bar) - 1,
                            std::vector<double>(numbers.begin(), numbers.end()),
                            std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size())});
        for (unsigned i = 0; i < 8; ++i)
            text_.push_back(static_cast<char>(text_byte(bar * 8 + i)));
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
        require(text_.size() == count * 8, "native growing string size");
        for (std::size_t i = 0; i < text_.size(); ++i)
            require(static_cast<unsigned char>(text_[i]) == text_byte(i), "native old growing string byte");
    }
    void finish() {
        std::vector<Udt> empty;
        empty.swap(objects_);
        std::string empty_text;
        empty_text.swap(text_);
    }
};

#if PF_A5U_HAS_GRAPH
class GraphFamily {
    using Id = pineforge::detail::o1::Id;
    pineforge::detail::o1::UdtGraph& graph_;
    Id holder_;
    Id root_;
public:
    explicit GraphFamily(pineforge::detail::o1::UdtGraph& graph)
        : graph_(graph), holder_(graph.make(0, 2)), root_(graph.root(holder_)) {}
    void append(std::size_t bar, double value) {
        auto transaction = graph_.begin();
        const auto numbers = history(bar);
        const auto bytes = label(bar);
        const Id vector = graph_.make(sizeof(numbers));
        graph_.write(vector, 0, numbers.data(), sizeof(numbers));
        const Id string = graph_.make(bytes.size());
        graph_.write(string, 0, bytes.data(), bytes.size());
        const Id object = graph_.make(2 * sizeof(std::uint64_t), 4);
        graph_.set_scalar(object, static_cast<std::int64_t>(bar));
        graph_.set_scalar(object, value, sizeof(std::int64_t));
        graph_.set_edge(object, 0, graph_.edge(holder_, 0));
        graph_.set_edge(object, 1, vector);
        graph_.set_edge(object, 2, string);
        graph_.set_edge(object, 3, vector); // The same child, counted once.
        graph_.set_edge(holder_, 0, object);
        // A generated unbounded string view streams fixed-size graph leaves.
        // Never decode the growing value to a whole native std::string.
        const Id text = graph_.make(8, 1);
        const auto off = bar * 8;
        std::array<unsigned char, 8> appended{};
        for (std::size_t i = 0; i < appended.size(); ++i) appended[i] = text_byte(off + i);
        graph_.write(text, 0, appended.data(), appended.size());
        graph_.set_edge(text, 0, graph_.edge(holder_, 1));
        graph_.set_edge(holder_, 1, text);
        transaction->commit();
    }
    void verify(std::size_t count) const {
        Id current = graph_.edge(holder_, 0);
        for (std::size_t remaining = count; remaining > 0; --remaining) {
            const auto bar = remaining - 1;
            require(graph_.scalar<std::int64_t>(current) == static_cast<std::int64_t>(bar) &&
                        graph_.scalar<double>(current, sizeof(std::int64_t)) == price(bar),
                    "old UDT scalar read");
            require(graph_.edge(current, 1) == graph_.edge(current, 3), "late read split a UDT alias");
            const auto expected_numbers = history(bar);
            std::array<double, 4> numbers{};
            const auto vector = graph_.edge(current, 1);
            require(graph_.payload_size(vector) == sizeof(numbers), "old nested vector size");
            graph_.read(vector, 0, numbers.data(), sizeof(numbers));
            require(std::memcmp(numbers.data(), expected_numbers.data(), sizeof(numbers)) == 0,
                    "old nested vector bits");
            const auto expected_bytes = label(bar);
            std::array<unsigned char, 32> bytes{};
            graph_.read(graph_.edge(current, 2), 0, bytes.data(), bytes.size());
            require(bytes == expected_bytes, "old nested string bytes");
            current = graph_.edge(current, 0);
        }
        require(current == 0, "UDT chain lost its na terminator or contains extra objects");
        Id text = graph_.edge(holder_, 1);
        std::array<unsigned char, 8> bytes{};
        for (std::size_t remaining = count; remaining > 0; --remaining) {
            require(graph_.payload_size(text) == bytes.size(), "growing string chunk extent");
            graph_.read(text, 0, bytes.data(), bytes.size());
            const auto off = (remaining - 1) * bytes.size();
            for (std::size_t i = 0; i < bytes.size(); ++i)
                require(bytes[i] == text_byte(off + i), "old byte in growing string");
            text = graph_.edge(text, 0);
        }
        require(text == 0, "growing string dropped or added chunks");
    }
    void finish() {
        graph_.drop_root(root_);
        graph_.collect();
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
        spec.identity = {"u07-growth", 1};
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

template<class Family>
void drive(Family& family, std::size_t count) {
    std::vector<pineforge::Bar> bars(count);
    for (std::size_t i = 0; i < count; ++i) {
        const double value = price(i);
        bars[i] = {value, value + 1, value - 1, value, 1000,
                   INT64_C(1736121600000) + static_cast<std::int64_t>(i) * 60000};
    }
    Host<Family> host(family);
    host.run(bars.data(), static_cast<int>(count), "1", "1");
    require(host.last_error().empty(), host.last_error().c_str());
    require(host.callbacks() == count, "UDT workload did not reach every synthetic bar");
    OperationWindow window;
    family.verify(count); // Every old object and byte, after the last bar.
}

inline int run(std::size_t count, int argc, char** argv) {
    try {
        require(count * sizeof(history(0)) > a5u_test::kResidentBound, "population cannot cross its bound");
        require(argc == 1 || (argc == 2 && std::strcmp(argv[1], "--native") == 0),
                "usage: test_a5u_growth [--native]");
        const bool native = !PF_A5U_HAS_GRAPH || argc == 2;
        global_allocation::on_recorded = &allocated;
        if (native) {
            NativeFamily family;
            drive(family, count);
            {
                OperationWindow window;
                family.finish();
            }
        } else {
#if PF_A5U_HAS_GRAPH
            a5u_test::StoreFixture fixture;
            pineforge::detail::o1::UdtGraph graph(*fixture.store);
            const auto empty_live = fixture.store->stats().bytes_live;
            GraphFamily family(graph);
            const auto initial = fixture.store->stats();
            drive(family, count);
            const auto stats = fixture.store->stats();
            require(stats.bytes_resident == initial.bytes_resident &&
                        stats.bytes_resident <= a5u_test::kStoreBound,
                    "real store workspace grew with objects");
            require(stats.bytes_live >= count * (sizeof(history(0)) + sizeof(label(0)) + 16) &&
                        stats.bytes_backing > stats.bytes_live &&
                        stats.bytes_backing > a5u_test::kResidentBound,
                    "live graph population did not spill beyond the resident window");
            {
                OperationWindow window;
                family.finish();
                require(fixture.store->stats().bytes_live == empty_live, "unreachable UDT objects leaked");
                graph.close();
                require(fixture.store->stats().bytes_live == 0, "graph close retained records");
            }
            std::printf("A5U bars=%zu live=%llu resident=%llu backing=%llu\n", count,
                        static_cast<unsigned long long>(stats.bytes_live),
                        static_cast<unsigned long long>(stats.bytes_resident),
                        static_cast<unsigned long long>(stats.bytes_backing));
            require(stats.bytes_resident + peak_heap <= a5u_test::kResidentBound,
                    "graph heap outside the real store exceeds the derived fixed bound");
#endif
        }
        std::printf("A5U bars=%zu operation_heap_peak=%zu bound=%llu mode=%s\n", count, peak_heap,
                    static_cast<unsigned long long>(a5u_test::kResidentBound),
                    native ? "native-RED" : "store-GREEN");
        require(peak_heap <= a5u_test::kResidentBound, "UDT owning fields retain heap proportional to bars");
        global_allocation::on_recorded = nullptr;
        return 0;
    } catch (const std::exception& error) {
        global_allocation::on_recorded = nullptr;
        std::fprintf(stderr, "A5U growth: %s\n", error.what());
        return 1;
    }
}
} // namespace a5u_growth
