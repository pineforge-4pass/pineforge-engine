#pragma once
#include <pineforge/detail/udt_graph.hpp>

namespace p2_a5 {
// Compiler-emitted analogue: integer-key, insertion-ordered map, with UDT
// values in owning edge slots. No native table or whole-map materialization.
class IntMap {
    using Graph = pineforge::detail::o1::UdtGraph;
    using Id = pineforge::detail::o1::Id;
public:
    explicit IntMap(Graph& graph) : graph_(graph), id_(graph.make(0)) {}
    Id id() const noexcept { return id_; }
    std::uint64_t size() const { return graph_.edge_count(id_); }
    Id get(std::int64_t key) const {
        const auto at = find(key);
        return at == size() ? 0 : graph_.edge(id_, at);
    }
    void put(std::int64_t key, Id value) {
        auto at = find(key);
        if (at == size()) {
            graph_.resize_payload(id_, (at + 1) * sizeof(key));
            graph_.resize_edges(id_, at + 1);
            graph_.set_scalar(id_, key, at * sizeof(key));
        }
        graph_.set_edge(id_, at, value);
    }
    bool erase(std::int64_t key) {
        const auto at = find(key), count = size();
        if (at == count) return false;
        for (auto i = at + 1; i < count; ++i) {
            graph_.set_scalar(id_, graph_.scalar<std::int64_t>(id_, i * sizeof(key)),
                              (i - 1) * sizeof(key));
            graph_.set_edge(id_, i - 1, graph_.edge(id_, i));
        }
        graph_.resize_edges(id_, count - 1);
        graph_.resize_payload(id_, (count - 1) * sizeof(key));
        return true;
    }
private:
    std::uint64_t find(std::int64_t key) const {
        for (std::uint64_t i = 0, count = size(); i < count; ++i)
            if (graph_.scalar<std::int64_t>(id_, i * sizeof(key)) == key) return i;
        return size();
    }
    Graph& graph_;
    const Id id_;
};
} // namespace p2_a5
