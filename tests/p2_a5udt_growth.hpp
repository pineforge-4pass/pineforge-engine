#pragma once

#include <pineforge/source/pine_strategy_host.hpp>
#include <algorithm>
#include <array>
#include <cinttypes>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <malloc.h>
#include <stdexcept>
#include <string>
#include <vector>
#if __has_include(<pineforge/detail/udt_graph.hpp>)
#include "p2_a5matudt_store.hpp"
#endif

namespace p2_udt_growth {
inline void require(bool ok, const char* why) { if (!ok) throw std::runtime_error(why); }
inline std::int64_t heap() {
    const auto info = ::mallinfo2();
    return static_cast<std::int64_t>(info.uordblks) + static_cast<std::int64_t>(info.hblkhd);
}
inline unsigned char text_byte(std::uint64_t index) {
    return static_cast<unsigned char>((index * 17 + 23) % 251);
}
inline std::array<double, 4> history(int bar) {
    return {{static_cast<double>(bar), -0.0, 0.125, static_cast<double>(bar) + 0.5}};
}

#if __has_include(<pineforge/detail/udt_graph.hpp>)
// Generated-code analogue. Native fields contain ids only. All old UDTs stay
// reachable from the list; the string is a separate UDT-owned growing field.
class Family {
    using Id = pineforge::detail::o1::Id;
    pineforge::detail::o1::UdtGraph& graph_;
    Id list_, list_root_, string_owner_, string_root_, text_;
public:
    explicit Family(pineforge::detail::o1::UdtGraph& graph)
        : graph_(graph), list_(graph.make(0)), list_root_(graph.root(list_)),
          string_owner_(graph.make(0, 1)), string_root_(graph.root(string_owner_)),
          text_(graph.make(0)) { graph_.set_edge(string_owner_, 0, text_); }
    void append_vector(int bar) {
        const auto values = history(bar);
        const Id vector = graph_.make(sizeof(values));
        graph_.write(vector, 0, values.data(), sizeof(values));
        const Id udt = graph_.make(sizeof(std::int64_t), 1);
        graph_.set_scalar(udt, static_cast<std::int64_t>(bar));
        graph_.set_edge(udt, 0, vector);
        graph_.resize_edges(list_, static_cast<std::uint64_t>(bar) + 1);
        graph_.set_edge(list_, bar, udt);
    }
    void append_string(int bar) {
        std::array<unsigned char, 8> bytes{};
        const auto offset = static_cast<std::uint64_t>(bar) * bytes.size();
        for (std::size_t j = 0; j < bytes.size(); ++j) bytes[j] = text_byte(offset + j);
        graph_.resize_payload(text_, offset + bytes.size());
        graph_.write(text_, offset, bytes.data(), bytes.size());
    }
    void verify(int count, bool string_case) const {
        if (string_case) {
            require(graph_.payload_size(text_) == static_cast<std::uint64_t>(count) * 8,
                    "nested string truncated");
            std::array<unsigned char, 256> bytes{};
            for (std::uint64_t off = 0, end = graph_.payload_size(text_); off < end;) {
                const auto n = static_cast<std::size_t>(std::min<std::uint64_t>(bytes.size(), end - off));
                graph_.read(text_, off, bytes.data(), n);
                for (std::size_t j = 0; j < n; ++j)
                    require(bytes[j] == text_byte(off + j), "old nested string byte changed");
                off += n;
            }
        } else {
            require(graph_.edge_count(list_) == static_cast<std::uint64_t>(count), "UDT population truncated");
            for (int bar = 0; bar < count; ++bar) {
                const Id udt = graph_.edge(list_, bar);
                require(graph_.scalar<std::int64_t>(udt) == bar, "old UDT scalar changed");
                std::array<double, 4> values{};
                graph_.read(graph_.edge(udt, 0), 0, values.data(), sizeof(values));
                const auto expected = history(bar);
                require(std::memcmp(values.data(), expected.data(), sizeof(values)) == 0,
                        "old nested vector changed");
            }
        }
    }
};
#else
// Frozen2 generated-struct analogue, not a claim that a graph API existed.
// The growth assertion below really fails on these owning native containers.
class Family {
    struct Udt { std::int64_t bar; std::vector<double> history; };
    std::vector<Udt> rows_;
    struct StringUdt { std::string text; } string_;
public:
    void append_vector(int bar) {
        const auto values = history(bar);
        rows_.push_back({bar, std::vector<double>(values.begin(), values.end())});
    }
    void append_string(int bar) {
        for (unsigned j = 0; j < 8; ++j)
            string_.text.push_back(static_cast<char>(text_byte(static_cast<std::uint64_t>(bar) * 8 + j)));
    }
    void verify(int count, bool string_case) const {
        if (string_case) {
            require(string_.text.size() == static_cast<std::uint64_t>(count) * 8, "native string population");
            for (std::size_t j = 0; j < string_.text.size(); ++j)
                require(static_cast<unsigned char>(string_.text[j]) == text_byte(j), "native old string byte");
        } else {
            require(rows_.size() == static_cast<std::size_t>(count), "native UDT population");
            for (int bar = 0; bar < count; ++bar) {
                require(rows_[bar].bar == bar, "native old UDT scalar");
                const auto expected = history(bar);
                require(rows_[bar].history.size() == expected.size(), "native old vector size");
                require(std::memcmp(rows_[bar].history.data(), expected.data(), sizeof(expected)) == 0,
                        "native old vector bytes");
            }
        }
    }
};
#endif

class Host final : public pineforge::source::PineStrategyHost {
public:
    Family family;
    bool string_case;
    int callbacks = 0;
    std::int64_t retained = 0, peak = 0;
#if __has_include(<pineforge/detail/udt_graph.hpp>)
    Host(pineforge::detail::o1::UdtGraph& graph, bool text) : family(graph), string_case(text) {}
#else
    explicit Host(bool text) : string_case(text) {}
#endif
    void on_source_bar(const pineforge::Bar&) override {
        require(pine_bar_index() == callbacks, "one generated UDT operation per real bar required");
        const auto before = heap();
        if (string_case) family.append_string(callbacks);
        else family.append_vector(callbacks);
        retained += heap() - before;
        peak = std::max(peak, retained);
        ++callbacks;
    }
};

inline int run(int count, bool string_case) {
    try {
        std::vector<pineforge::Bar> bars;
        bars.reserve(static_cast<std::size_t>(count));
        for (int i = 0; i < count; ++i) {
            const double open = 100.0 + static_cast<double>(i % 71) * 0.125;
            bars.push_back({open, open + 1, open - 1, open + 0.5, 1000,
                            INT64_C(1700000100000) + i * INT64_C(60000)});
        }
#if __has_include(<pineforge/detail/udt_graph.hpp>)
        p2_a5::StoreFixture store;
        Host host(store.runtime.graph(), string_case);
#else
        Host host(string_case);
#endif
        host.run(bars.data(), count, "1", "1", false);
        require(host.last_error().empty(), host.last_error().c_str());
        require(host.callbacks == count, "UDT workload did not reach all synthetic bars");
#if __has_include(<pineforge/detail/udt_graph.hpp>)
        store.runtime.collect();
#endif
        host.family.verify(count, string_case);
        // Same derivation as the matrix row: 64 cache pages, 6 store work
        // pages, 1 metadata page, 4 fixed graph/matrix chunk pages, slot and
        // hash-bucket records, and a 1024+16 byte object-layer chunk buffer.
        constexpr std::int64_t bound = (64 + 6 + 1 + 4) * 4096
            + 64 * 32 + 128 * 4 + 1024 + 16 + sizeof(Family);
        std::printf("P2-A5 UDT kind=%s bars=%d retained_delta=%" PRId64
                    " peak_delta=%" PRId64 " bound=%" PRId64 "\n",
                    string_case ? "nested-string" : "nested-vector", count, host.retained, host.peak, bound);
#if __has_include(<pineforge/detail/udt_graph.hpp>)
        const auto stats = store.store->stats();
        std::printf("P2-A5 UDT store bars=%d live=%" PRIu64
                    " resident=%" PRIu64 " backing=%" PRIu64 " digest=%016" PRIx64 "\n",
                    count, stats.bytes_live, stats.bytes_resident, stats.bytes_backing,
                    store.runtime.graph().hash_state());
#endif
        std::fflush(stdout);
        require(host.peak <= bound, "UDT owned payload retained heap grows with bars");
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "P2-A5 UDT growth: %s\n", error.what());
        return 1;
    }
}
} // namespace p2_udt_growth
