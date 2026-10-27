#pragma once

#include <pineforge/detail/udt_graph.hpp>

namespace a5u_test {

// Synthetic generated map of integer keys to UDT references. Keys are payload
// bytes; values are owning graph edges. This is a codegen fixture, not a new
// production map implementation.
class IdMap {
    using Graph = pineforge::detail::o1::UdtGraph;
    using Id = pineforge::detail::o1::Id;
    Graph& graph_;
    Id id_;
    std::uint64_t find(std::int64_t key) const {
        const auto count = size();
        for (std::uint64_t i = 0; i < count; ++i)
            if (graph_.scalar<std::int64_t>(id_, i * sizeof(key)) == key) return i;
        return count;
    }
public:
    explicit IdMap(Graph& graph) : graph_(graph), id_(graph.make(0)) {}
    Id id() const noexcept { return id_; }
    std::uint64_t size() const { return graph_.edge_count(id_); }
    Id get(std::int64_t key) const {
        const auto at = find(key);
        return at == size() ? 0 : graph_.edge(id_, at);
    }
    // A container operation joins the caller's open transaction (no handle);
    // with none open it owns one, and its graph calls join that.
    void put(std::int64_t key, Id value) {
        auto transaction = graph_.store().scope();
        const auto at = find(key);
        if (at == size()) {
            graph_.resize_payload(id_, (at + 1) * sizeof(key));
            graph_.resize_edges(id_, at + 1);
            graph_.set_scalar(id_, key, at * sizeof(key));
        }
        graph_.set_edge(id_, at, value);
        transaction.commit();
    }
    bool erase(std::int64_t key) {
        const auto at = find(key), count = size();
        if (at == count) return false;
        auto transaction = graph_.store().scope();
        for (std::uint64_t i = at + 1; i < count; ++i) {
            graph_.set_scalar(id_, graph_.scalar<std::int64_t>(id_, i * sizeof(key)),
                              (i - 1) * sizeof(key));
            graph_.set_edge(id_, i - 1, graph_.edge(id_, i));
        }
        graph_.resize_payload(id_, (count - 1) * sizeof(key));
        graph_.resize_edges(id_, count - 1);
        transaction.commit();
        return true;
    }
};

} // namespace a5u_test
