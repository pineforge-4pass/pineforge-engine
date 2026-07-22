#include <pineforge/generic_matrix.hpp>
#include <pineforge/matrix.hpp>
#include <pineforge/na.hpp>

#include <cstdio>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>

using pineforge::PineGenericMatrix;
using pineforge::PineMatrix;

static void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

template <typename Fn>
static void require_runtime_error(Fn&& fn, const char* expected) {
    try {
        fn();
    } catch (const std::runtime_error& error) {
        require(std::string(error.what()) == expected,
                "runtime error message mismatch");
        return;
    }
    throw std::runtime_error("expected runtime error was not thrown");
}

static void test_float_assignment_aliases_and_explicit_copy_detaches() {
    static_assert(std::is_nothrow_default_constructible_v<PineMatrix>);
    static_assert(std::is_nothrow_copy_constructible_v<PineMatrix>);
    static_assert(std::is_nothrow_copy_assignable_v<PineMatrix>);
    static_assert(std::is_nothrow_move_constructible_v<PineMatrix>);
    static_assert(std::is_nothrow_move_assignable_v<PineMatrix>);

    auto original = PineMatrix::new_(1, 1, 1.0);
    auto alias = original;
    alias.set(0, 0, 2.0);
    require(original.get(0, 0) == 2.0,
            "ordinary float-matrix copy must alias");

    auto assigned = PineMatrix::new_(1, 1, 90.0);
    auto discarded_identity = assigned;
    assigned = original;
    assigned.set(0, 0, 3.0);
    require(original.get(0, 0) == 3.0,
            "ordinary float-matrix assignment must alias");
    require(discarded_identity.get(0, 0) == 90.0,
            "rebinding must not mutate the discarded identity");

    auto detached = original.copy();
    detached.set(0, 0, 4.0);
    require(original.get(0, 0) == 3.0,
            "matrix.copy must allocate an independent outer ID");
    require(detached.get(0, 0) == 4.0,
            "matrix.copy result must remain mutable");
}

static void test_float_moves_preserve_source_handle() {
    auto source = PineMatrix::new_(1, 1, 5.0);
    PineMatrix constructed(std::move(source));
    constructed.set(0, 0, 6.0);
    require(!source.is_na() && source.get(0, 0) == 6.0,
            "move construction must preserve the source matrix ID");

    auto assigned = PineMatrix::new_(1, 1, 70.0);
    auto old_assigned_identity = assigned;
    assigned = std::move(source);
    assigned.set(0, 0, 8.0);
    require(source.get(0, 0) == 8.0 && constructed.get(0, 0) == 8.0,
            "move assignment must preserve all aliases");
    require(old_assigned_identity.get(0, 0) == 70.0,
            "move assignment must only rebind the receiver");
}

template <typename Matrix, typename Value, typename Factory>
static void check_generic_assignment_copy_and_move(
        Value initial, Value aliased, Value detached_value, Factory&& make) {
    static_assert(std::is_nothrow_default_constructible_v<Matrix>);
    static_assert(std::is_nothrow_copy_constructible_v<Matrix>);
    static_assert(std::is_nothrow_copy_assignable_v<Matrix>);
    static_assert(std::is_nothrow_move_constructible_v<Matrix>);
    static_assert(std::is_nothrow_move_assignable_v<Matrix>);

    auto original = make(initial);
    auto alias = original;
    alias.set(0, 0, aliased);
    require(original.get(0, 0) == aliased,
            "ordinary generic-matrix copy must alias");

    auto detached = original.copy();
    detached.set(0, 0, detached_value);
    require(original.get(0, 0) == aliased,
            "generic matrix.copy must detach the outer ID");
    require(detached.get(0, 0) == detached_value,
            "detached generic matrix must hold its own value");

    Matrix moved(std::move(original));
    moved.set(0, 0, initial);
    require(!original.is_na() && original.get(0, 0) == initial,
            "generic-matrix move must preserve the source ID");
    require(alias.get(0, 0) == initial,
            "generic-matrix move must preserve existing aliases");
}

static void test_generic_assignment_copy_and_move_semantics() {
    check_generic_assignment_copy_and_move<PineGenericMatrix<int>>(
        1, 2, 3,
        [](int value) { return PineGenericMatrix<int>::new_(1, 1, value); });
    check_generic_assignment_copy_and_move<PineGenericMatrix<bool>>(
        false, true, false,
        [](bool value) { return PineGenericMatrix<bool>::new_(1, 1, value); });
    check_generic_assignment_copy_and_move<PineGenericMatrix<std::string>>(
        std::string("a"), std::string("b"), std::string("c"),
        [](const std::string& value) {
            return PineGenericMatrix<std::string>::new_(1, 1, value);
        });
}

template <typename Matrix, typename Value, typename Factory>
static void check_snapshot_identity(Value initial, Value mutated,
                                    Value replacement_value,
                                    Factory&& make) {
    Matrix null_matrix;
    require_runtime_error(
        [&] { (void)null_matrix.snapshot(); },
        "matrix operation on na ID");

    auto live = make(initial);
    auto alias = live;
    auto snapshot = live.snapshot();

    alias.set(0, 0, mutated);
    alias.add_row(1, {mutated});
    require(alias.rows() == 2, "snapshot test mutation must change shape");

    auto replacement = make(replacement_value);
    auto replacement_alias = replacement;
    live = replacement;
    live.restore(snapshot);

    require(live.rows() == 1 && live.columns() == 1,
            "restore must recover the snapshotted shape");
    require(live.get(0, 0) == initial && alias.get(0, 0) == initial,
            "restore must mutate the original identity in place");
    live.set(0, 0, mutated);
    require(alias.get(0, 0) == mutated,
            "restored receiver must reattach to the original identity");
    require(replacement_alias.get(0, 0) == replacement_value,
            "restore must not mutate a replacement identity");

    live = Matrix{};
    require(live.is_na(), "test receiver must be rebound to na");
    live.restore(snapshot);
    require(!live.is_na() && live.get(0, 0) == initial,
            "restore must reattach a receiver rebound to na");
    require(alias.get(0, 0) == initial,
            "reused snapshot must roll back existing aliases again");

    auto active_snapshot = std::move(snapshot);
    require_runtime_error(
        [&] { live.restore(snapshot); },
        "matrix restore from invalid snapshot");
    live.restore(active_snapshot);
    require(live.get(0, 0) == initial,
            "moved snapshot must retain its rollback state");
}

static void test_snapshot_restores_contents_and_identity() {
    check_snapshot_identity<PineMatrix>(
        1.0, 2.0, 9.0,
        [](double value) { return PineMatrix::new_(1, 1, value); });
    check_snapshot_identity<PineGenericMatrix<int>>(
        1, 2, 9,
        [](int value) { return PineGenericMatrix<int>::new_(1, 1, value); });
    check_snapshot_identity<PineGenericMatrix<bool>>(
        false, true, true,
        [](bool value) { return PineGenericMatrix<bool>::new_(1, 1, value); });
}

template <typename Matrix, typename Value, typename Factory>
static void check_two_alias_snapshots_restore_one_identity(
        Value initial, Value changed, Factory&& make) {
    auto first = make(initial);
    auto second = first;
    auto first_snapshot = first.snapshot();
    auto second_snapshot = second.snapshot();

    first.set(0, 0, changed);
    first = make(changed);
    second = make(changed);

    first.restore(first_snapshot);
    second.restore(second_snapshot);
    second.set(0, 0, changed);
    require(first.get(0, 0) == changed,
            "two restored aliases must converge on one original identity");
}

static void test_two_alias_snapshots_restore_one_identity() {
    check_two_alias_snapshots_restore_one_identity<PineMatrix>(
        1.0, 2.0,
        [](double value) { return PineMatrix::new_(1, 1, value); });
    check_two_alias_snapshots_restore_one_identity<PineGenericMatrix<int>>(
        1, 2,
        [](int value) { return PineGenericMatrix<int>::new_(1, 1, value); });
}

static void test_snapshot_keeps_original_identity_alive() {
    auto snapshot = [] {
        auto ephemeral = PineMatrix::new_(1, 1, 7.0);
        return ephemeral.snapshot();
    }();

    PineMatrix restored;
    restored.restore(snapshot);
    require(restored.get(0, 0) == 7.0,
            "snapshot must keep an otherwise-unreferenced ID alive");
}

struct Holder {
    int scalar{0};
    PineMatrix nested;
};

static void test_generic_copy_is_outer_deep_and_nested_handle_shallow() {
    auto nested = PineMatrix::new_(1, 1, 10.0);
    auto outer = PineGenericMatrix<Holder>::new_(
        1, 1, Holder{1, nested});
    auto copied = outer.copy();

    Holder copied_holder = copied.get(0, 0);
    copied_holder.scalar = 2;
    copied_holder.nested.set(0, 0, 20.0);
    copied.set(0, 0, copied_holder);

    const Holder original_holder = outer.get(0, 0);
    require(original_holder.scalar == 1,
            "matrix.copy must detach the outer element buffer");
    require(original_holder.nested.get(0, 0) == 20.0,
            "matrix.copy must shallow-copy nested matrix handles");

    auto replacement = PineMatrix::new_(1, 1, 30.0);
    copied_holder.nested = replacement;
    copied.set(0, 0, copied_holder);
    require(outer.get(0, 0).nested.get(0, 0) == 20.0,
            "rebinding a copied cell must not replace the original cell");
    require(copied.get(0, 0).nested.get(0, 0) == 30.0,
            "copied outer matrix must accept an independent nested binding");
}

int main() {
    test_float_assignment_aliases_and_explicit_copy_detaches();
    test_float_moves_preserve_source_handle();
    test_generic_assignment_copy_and_move_semantics();
    test_snapshot_restores_contents_and_identity();
    test_two_alias_snapshots_restore_one_identity();
    test_snapshot_keeps_original_identity_alive();
    test_generic_copy_is_outer_deep_and_nested_handle_shallow();
    std::printf("All matrix identity tests passed.\n");
    return 0;
}
