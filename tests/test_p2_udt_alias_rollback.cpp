// Frozen2 control: a shallow generated checkpoint can retain the right matrix
// slots while losing the pre-rollback value of their shared UDT child. This
// demonstrates the compiler's graph obligation, not a change to matrix<T>'s
// deliberately shallow reference-element snapshot contract.
#include <pineforge/generic_matrix.hpp>
#include <cstdio>
#include <memory>
#include <stdexcept>
#if __has_include(<pineforge/detail/udt_graph.hpp>)
#include "p2_a5matudt_store.hpp"
#endif

int main() {
    try {
        double restored = 0;
#if __has_include(<pineforge/detail/udt_graph.hpp>)
        p2_a5::StoreFixture fixture;
        auto& graph = fixture.runtime.graph();
        const auto child = graph.make(sizeof(double)), parent = graph.make(0, 2);
        graph.set_scalar(child, 17.25);
        graph.set_edge(parent, 0, child); graph.set_edge(parent, 1, child);
        const auto root = graph.root(parent);
        auto checkpoint = graph.begin();
        graph.set_scalar(graph.edge(parent, 0), 99.0);
        checkpoint->rollback();
        p2_a5::require(graph.edge(parent, 0) == graph.edge(parent, 1), "rollback split child identity");
        restored = graph.scalar<double>(graph.edge(parent, 1));
        graph.drop_root(root);
#else
        struct Child { double value = 17.25; };
        const auto child = std::make_shared<Child>();
        auto matrix = pineforge::GenericMatrix<std::shared_ptr<Child>>::new_(1, 2, child);
        const auto checkpoint = matrix.snapshot();
        matrix.get(0, 0)->value = 99.0;
        matrix.restore(checkpoint);
        if (matrix.get(0, 0) != matrix.get(0, 1)) throw std::runtime_error("native checkpoint split alias");
        restored = matrix.get(0, 1)->value;
#endif
        std::printf("P2-A5 alias rollback restored=%.2f expected=17.25\n", restored);
        if (restored != 17.25) throw std::runtime_error("shared child mutation escaped rollback");
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "P2-A5 alias rollback: %s\n", error.what());
        return 1;
    }
}
