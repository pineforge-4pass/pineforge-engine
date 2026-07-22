#include <pineforge/generic_matrix.hpp>
#include <pineforge/matrix.hpp>

// Keep this executable's assertions active in Release builds so the
// compatibility gate cannot become vacuous under -DNDEBUG.
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>

using pineforge::PineGenericMatrix;
using pineforge::PineMatrix;

template <typename Matrix>
void require_na_snapshot_error(const Matrix& matrix) {
    try {
        (void)matrix.snapshot();
        assert(false && "snapshot() on na must throw");
    } catch (const std::runtime_error& error) {
        assert(std::string(error.what()) == "matrix operation on na ID");
    }
}

template <typename T>
void check_generic_snapshot_round_trip(const T& initial, const T& changed) {
    using Matrix = PineGenericMatrix<T>;
    using Snapshot = typename Matrix::Snapshot;

    static_assert(std::is_copy_constructible_v<Snapshot>);
    static_assert(std::is_copy_assignable_v<Snapshot>);
    static_assert(std::is_move_constructible_v<Snapshot>);
    static_assert(std::is_move_assignable_v<Snapshot>);
    static_assert(!std::is_default_constructible_v<Snapshot>);

    Matrix matrix = Matrix::new_(1, 1, initial);
    Snapshot snapshot = matrix.snapshot();
    Snapshot copied = snapshot;
    Snapshot moved = std::move(copied);

    matrix.set(0, 0, changed);
    matrix.restore(snapshot);
    assert(matrix.get(0, 0) == initial);

    matrix = Matrix::new_(2, 1, changed);
    matrix.restore(moved);
    assert(matrix.rows() == 1);
    assert(matrix.columns() == 1);
    assert(matrix.get(0, 0) == initial);

    Matrix receiver;
    receiver.restore(snapshot);
    assert(!receiver.is_na());
    assert(receiver.get(0, 0) == initial);

    receiver.set(0, 0, changed);
    receiver.restore(snapshot);
    assert(receiver.get(0, 0) == initial);

    Matrix empty = Matrix::new_(0, 0, initial);
    Snapshot empty_snapshot = empty.snapshot();
    Matrix empty_receiver;
    empty_receiver.restore(empty_snapshot);
    assert(!empty_receiver.is_na());
    assert(empty_receiver.rows() == 0);
    assert(empty_receiver.columns() == 0);

    require_na_snapshot_error(Matrix{});
}

int main() {
    using Snapshot = PineMatrix::Snapshot;
    static_assert(std::is_copy_constructible_v<Snapshot>);
    static_assert(std::is_copy_assignable_v<Snapshot>);
    static_assert(std::is_move_constructible_v<Snapshot>);
    static_assert(std::is_move_assignable_v<Snapshot>);
    static_assert(!std::is_default_constructible_v<Snapshot>);

    PineMatrix matrix = PineMatrix::new_(1, 1, 7.0);
    PineMatrix assigned = matrix;
    assigned.set(0, 0, 13.0);
    assert(matrix.get(0, 0) == 7.0);
    assert(assigned.get(0, 0) == 13.0);

    PineMatrix explicit_copy = matrix.copy();
    explicit_copy.set(0, 0, 99.0);
    assert(matrix.get(0, 0) == 7.0);
    assert(explicit_copy.get(0, 0) == 99.0);

    Snapshot snapshot = matrix.snapshot();
    Snapshot copied = snapshot;
    Snapshot moved = std::move(copied);
    matrix.set(0, 0, 3.0);
    matrix.restore(snapshot);
    assert(matrix.get(0, 0) == 7.0);

    matrix = PineMatrix::new_(2, 1, 5.0);
    matrix.restore(moved);
    assert(matrix.rows() == 1);
    assert(matrix.columns() == 1);
    assert(matrix.get(0, 0) == 7.0);

    PineMatrix receiver;
    receiver.restore(snapshot);
    assert(!receiver.is_na());
    assert(receiver.get(0, 0) == 7.0);
    receiver.set(0, 0, 8.0);
    receiver.restore(snapshot);
    assert(receiver.get(0, 0) == 7.0);

    PineMatrix empty = PineMatrix::new_(0, 0, 0.0);
    Snapshot empty_snapshot = empty.snapshot();
    PineMatrix empty_receiver;
    empty_receiver.restore(empty_snapshot);
    assert(!empty_receiver.is_na());
    assert(empty_receiver.rows() == 0);
    assert(empty_receiver.columns() == 0);

    require_na_snapshot_error(PineMatrix{});

    PineGenericMatrix<int> generic = PineGenericMatrix<int>::new_(1, 1, 7);
    PineGenericMatrix<int> generic_assigned = generic;
    generic_assigned.set(0, 0, 13);
    assert(generic.get(0, 0) == 7);
    assert(generic_assigned.get(0, 0) == 13);
    PineGenericMatrix<int> generic_copy = generic.copy();
    generic_copy.set(0, 0, 99);
    assert(generic.get(0, 0) == 7);
    assert(generic_copy.get(0, 0) == 99);

    check_generic_snapshot_round_trip<int>(7, 13);
    check_generic_snapshot_round_trip<std::string>("alpha", "beta");
    check_generic_snapshot_round_trip<bool>(true, false);
}
