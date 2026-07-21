#include <pineforge/generic_matrix.hpp>
#include <pineforge/matrix.hpp>
#include <pineforge/na.hpp>

#include <stdexcept>
#include <string>

using pineforge::PineGenericMatrix;
using pineforge::PineMatrix;
using pineforge::is_na;

static void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

template <typename Fn>
static bool throws_na_id(Fn&& fn) {
    try {
        fn();
    } catch (const std::runtime_error& exc) {
        return std::string(exc.what()) == "matrix operation on na ID";
    }
    return false;
}

template <typename Matrix, typename Factory>
static void check_nullable_id_lifecycle(Factory&& make_valid) {
    Matrix state;
    require(is_na(state), "default matrix must be a Pine na ID");
    require(throws_na_id([&] { (void)state.rows(); }),
            "rows() on na matrix must fail");
    require(throws_na_id([&] { (void)state.get(-1, 0); }),
            "na matrix must fail before index validation");
    require(throws_na_id([&] { state.add_row(-1, {}); }),
            "na matrix add_row must fail before index validation");
    require(throws_na_id([&] { state.swap_rows(-1, 0); }),
            "na matrix swap_rows must fail before index validation");
    require(throws_na_id([&] { (void)state.submatrix(-1, 0, 0, 0); }),
            "na matrix submatrix must fail before index validation");
    require(throws_na_id([&] { (void)state.copy(); }),
            "copy() on na matrix must fail");

    Matrix copied_null = state;
    require(is_na(copied_null), "copying a na ID must preserve na");

    state = make_valid(0, 0);
    require(!is_na(state), "empty matrix must have a valid ID");
    require(state.rows() == 0, "empty matrix must have zero rows");
    require(state.columns() == 0, "empty matrix must have zero columns");

    state = make_valid(1, 1);
    require(!is_na(state), "new matrix must have a valid ID");
    require(state.rows() == 1, "new matrix must expose its row count");
    require(state.columns() == 1,
            "new matrix must expose its column count");
    require(!is_na(state.copy()), "matrix.copy() must return a valid ID");
    require(!is_na(state.transpose()),
            "matrix.transpose() must return a valid ID");
    require(!is_na(state.concat(make_valid(1, 1), true)),
            "matrix.concat() must return a valid ID");

    Matrix null_rhs;
    require(throws_na_id([&] { (void)state.concat(null_rhs, true); }),
            "matrix.concat() must reject a na RHS ID");

    state = pineforge::na<Matrix>();
    require(is_na(state), "reassigning na must clear the matrix ID");
    require(throws_na_id([&] { (void)state.elements_count(); }),
            "elements_count() on na matrix must fail");
}

int main() {
    check_nullable_id_lifecycle<PineMatrix>(
        [](int rows, int cols) {
            return PineMatrix::new_(rows, cols, 0.0);
        });
    check_nullable_id_lifecycle<PineGenericMatrix<int>>(
        [](int rows, int cols) {
            return PineGenericMatrix<int>::new_(rows, cols, 0);
        });
    check_nullable_id_lifecycle<PineGenericMatrix<std::string>>(
        [](int rows, int cols) {
            return PineGenericMatrix<std::string>::new_(
                rows, cols, std::string{});
        });
    check_nullable_id_lifecycle<PineGenericMatrix<bool>>(
        [](int rows, int cols) {
            return PineGenericMatrix<bool>::new_(rows, cols, false);
        });

    PineMatrix nonsquare = PineMatrix::new_(1, 2, 0.0);
    PineMatrix empty_inverse = nonsquare.inv();
    require(!is_na(empty_inverse),
            "a valid empty failure result must not become a na ID");
    require(empty_inverse.rows() == 0 && empty_inverse.columns() == 0,
            "non-square inverse must preserve its valid empty result");
}
