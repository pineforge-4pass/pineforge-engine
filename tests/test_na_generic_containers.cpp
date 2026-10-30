#include <pineforge/na.hpp>
#include <pineforge/array.hpp>
#include <pineforge/drawing.hpp>
#include <pineforge/generic_matrix.hpp>
#include <pineforge/map.hpp>
#include <pineforge/matrix.hpp>

#include "array_store_fixture.hpp"

#include <cmath>
#include <cstdio>
#include <functional>
#include <limits>
#include <map>
#include <memory>
#include <string>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>

// Match generated translation units: engine names and generated free
// overloads are visible together at namespace scope.
using namespace pineforge;

template <class T>
struct CollectionTraits;

template <class E, class A>
struct CollectionTraits<std::vector<E, A>> {
    static bool is_na(const std::vector<E, A>&) { return false; }
};

template <class T>
struct ArrayHistoryValue {
    std::shared_ptr<T> value;
    bool is_na() const { return !value; }
    operator T&() const { return *value; }
};

template <class T>
bool is_na(const ArrayHistoryValue<T>& value) { return value.is_na(); }

// Also check a free vector overload, a stricter collision test than the
// generated CollectionTraits static member above.
template <class E, class A>
bool is_na(const std::vector<E, A>& value) {
    return CollectionTraits<std::vector<E, A>>::is_na(value);
}

struct GeneratedObject {
    std::int32_t id = -1;
};

inline bool is_na(const GeneratedObject& value) { return value.id < 0; }

namespace {

int failures = 0;

void check(const char* row, bool ok) {
    if (!ok) {
        std::printf("FAIL %s\n", row);
        ++failures;
    }
}

template <class T, class = void>
struct HasEngineIsNa : std::false_type {};

template <class T>
struct HasEngineIsNa<T, std::void_t<
    decltype(pineforge::is_na(std::declval<const T&>()))>>
    : std::true_type {};

template <class T, class = void>
struct HasUnqualifiedIsNa : std::false_type {};

template <class T>
struct HasUnqualifiedIsNa<T, std::void_t<
    decltype(is_na(std::declval<const T&>()))>> : std::true_type {};

// Every engine call is inside a dependent, detected branch. The previous
// header must build this test and fail rows, rather than fail compilation.
template <class T>
void check_value(const char* row, const T& value, bool expected) {
    if constexpr (HasEngineIsNa<T>::value) {
        check(row, pineforge::is_na(value) == expected);
    } else {
        check(row, HasEngineIsNa<T>::value);
    }
}

template <class T>
void check_present_value(const char* row, const T& value) {
    // DEFINED MEANING (stated in na.hpp): a plain value, including an empty
    // container, is present and never na, whatever its size or elements. The
    // expectation is therefore the literal false. An empty-as-na mutant fails
    // every empty collection row.
    check_value(row, value, false);
}

template <class T>
void check_default_value(const char* row) {
    if constexpr (HasEngineIsNa<T>::value) {
        const auto value = pineforge::na<T>();
        check(row, value == T{});
        check_present_value(row, value);
    } else {
        check(row, HasEngineIsNa<T>::value);
    }
}

struct FlaggedHandle {
    bool missing = true;
    bool is_na() const { return missing; }
};

struct FactoryHandle {
    bool missing = false;
    static FactoryHandle na() { return {true}; }
    bool is_na() const { return missing; }
};

struct FactoryOnlyHandle {
    bool missing;
    FactoryOnlyHandle() = delete;
    explicit FactoryOnlyHandle(bool flag) : missing(flag) {}
    static FactoryOnlyHandle na() { return FactoryOnlyHandle(true); }
    bool is_na() const { return missing; }
};

struct PlainObject {
    int number = std::numeric_limits<int>::min();
    std::vector<std::string> items;
    bool operator==(const PlainObject& other) const {
        return number == other.number && items == other.items;
    }
};

struct NumericView {
    double number;
    operator double() const { return number; }
};

template <class Handle>
void check_flag_flip(const char* row, Handle value) {
    if constexpr (HasEngineIsNa<Handle>::value) {
        const auto before = pineforge::is_na(value);
        check(row, before == value.missing);
        value.missing = !value.missing;
        const auto after = pineforge::is_na(value);
        check(row, after == value.missing && after != before);
        // Either constant-result mutant must disagree with one flag state.
    } else {
        check(row, HasEngineIsNa<Handle>::value);
    }
}

template <class Handle>
void check_default_handle(const char* row) {
    if constexpr (HasEngineIsNa<Handle>::value) {
        const Handle expected{};
        const auto value = pineforge::na<Handle>();
        check_value(row, value, expected.is_na());
    } else {
        check(row, HasEngineIsNa<Handle>::value);
    }
}

template <class Handle>
void check_factory_handle(const char* row) {
    if constexpr (HasEngineIsNa<Handle>::value) {
        const auto expected = Handle::na();
        const auto value = pineforge::na<Handle>();
        check_value(row, value, expected.missing);
    } else {
        check(row, HasEngineIsNa<Handle>::value);
    }
}

template <class Handle>
void check_constant_false_mutant(const char* row, Handle value) {
    if constexpr (HasEngineIsNa<Handle>::value) {
        const auto mutant = [](const Handle&) { return false; };
        check(row, pineforge::is_na(value) != mutant(value));
        value.missing = !value.missing;
        check_value(row, value, value.missing);
    } else {
        check(row, HasEngineIsNa<Handle>::value);
    }
}

template <class Container>
void check_empty_mutant(const char* row, const Container& value) {
    if constexpr (HasEngineIsNa<Container>::value) {
        const auto mutant = [](const Container& input) { return input.empty(); };
        check(row, pineforge::is_na(value) != mutant(value));
    } else {
        check(row, HasEngineIsNa<Container>::value);
    }
}

template <class T>
void check_integer(const char* row) {
    const T values[] = {std::numeric_limits<T>::min(), T{},
                        std::numeric_limits<T>::max()};
    for (const auto value : values)
        check_value(row, value, value == std::numeric_limits<T>::min());
}

template <class T>
void check_floating_conversion(const char* row) {
    const T values[] = {static_cast<T>(std::numeric_limits<double>::quiet_NaN()),
                        T{}, std::numeric_limits<T>::infinity()};
    for (const auto value : values)
        check_value(row, value, std::isnan(static_cast<double>(value)));
}

template <class T>
void check_coexistence(const char* row, const T& value, bool expected) {
    if constexpr (HasEngineIsNa<T>::value && HasUnqualifiedIsNa<T>::value) {
        check(row, is_na(value) == expected);
    } else {
        check(row, HasEngineIsNa<T>::value && HasUnqualifiedIsNa<T>::value);
    }
}

template <class Matrix>
void check_matrix(const char* row) {
    const Matrix missing{};
    check_default_handle<Matrix>(row);
    check_value(row, missing, missing.is_na());
    // These two constructed handles own storage even at zero dimensions.
    const auto empty = Matrix::new_(0, 0);
    const auto populated = Matrix::new_(1, 1);
    check_value(row, empty, empty.is_na());
    check_value(row, populated, populated.is_na());
    check_coexistence(row, missing, missing.is_na());
    check_coexistence(row, empty, empty.is_na());
}

template <class Array>
void check_store_array(const char* row) {
    if constexpr (HasEngineIsNa<Array>::value) {
        array_test::StoreFixture fixture;
        const Array missing{};
        check_value(row, missing, !missing.store_backed());
        const auto typed_na = pineforge::na<Array>();
        check_value(row, typed_na, !missing.store_backed());
        Array empty(*fixture.store);
        check_value(row, empty, !empty.store_backed());
        empty.push_back(typename Array::value_type{});
        check_value(row, empty, !empty.store_backed());
    } else {
        check(row, HasEngineIsNa<Array>::value);
    }
}

void check_scalars() {
    check_floating_conversion<double>("scalar_double");
    check_floating_conversion<float>("scalar_float_conversion");
    check_floating_conversion<long double>("scalar_long_double_conversion");
    check_integer<int>("scalar_int");
    check_integer<long long>("scalar_long_long");
    check_integer<std::int64_t>("scalar_int64");
    check_integer<unsigned>("scalar_unsigned");
    const bool booleans[] = {std::numeric_limits<bool>::min(),
                             std::numeric_limits<bool>::max()};
    for (const auto value : booleans)
        check_value("scalar_bool", value, std::isnan(static_cast<double>(value)));

    const double numbers[] = {std::numeric_limits<double>::quiet_NaN(), 0.0};
    for (const auto number : numbers)
        check_value("numeric_view_conversion", NumericView{number}, std::isnan(number));

    if constexpr (HasEngineIsNa<double>::value)
        check("na_double", std::isnan(pineforge::na<double>()));
    if constexpr (HasEngineIsNa<int>::value)
        check("na_int", pineforge::na<int>() == std::numeric_limits<int>::min());
    if constexpr (HasEngineIsNa<std::int64_t>::value)
        check("na_int64", pineforge::na<std::int64_t>() ==
                          std::numeric_limits<std::int64_t>::min());
    if constexpr (HasEngineIsNa<bool>::value)
        check("na_bool", pineforge::na<bool>() == std::numeric_limits<bool>::min());
    // long long has no explicit na specialization unless it aliases int64_t.
    // Preserve that platform-dependent behavior, including the T{} fallback.
    if constexpr (HasEngineIsNa<long long>::value)
        check("na_long_long", pineforge::na<long long>() ==
              (std::is_same_v<long long, std::int64_t>
                   ? std::numeric_limits<long long>::min()
                   : static_cast<long long>(int{})));
}

} // namespace

int main() {
    check_present_value("vector_string_empty", std::vector<std::string>{});
    check_present_value("vector_string_nonempty", std::vector<std::string>{"item"});
    check_present_value("vector_double_empty", std::vector<double>{});
    check_present_value("vector_double_nonempty",
                        std::vector<double>{std::numeric_limits<double>::quiet_NaN()});
    check_present_value("vector_int_empty", std::vector<int>{});
    check_present_value("vector_int_nonempty",
                        std::vector<int>{std::numeric_limits<int>::min()});
    check_present_value("vector_nested_empty", std::vector<std::vector<int>>{});
    check_present_value("vector_nested_nonempty", std::vector<std::vector<int>>{{}});
    check_present_value("vector_bool_empty", std::vector<bool>{});
    check_present_value("vector_bool_nonempty", std::vector<bool>{false});
    check_present_value("vector_udt_empty", std::vector<FlaggedHandle>{});
    check_present_value("vector_udt_nonempty", std::vector<FlaggedHandle>{{}});
    check_present_value("vector_drawing_empty", std::vector<Line>{});
    check_present_value("vector_drawing_nonempty", std::vector<Line>{{}});
    check_present_value("string_empty", std::string{});
    check_present_value("string_nonempty", std::string{"item"});
    check_present_value("script_string_empty", ScriptString{});
    check_present_value("map_empty", std::map<std::string, double>{});
    check_present_value("map_nonempty", std::map<std::string, double>{
        {"item", std::numeric_limits<double>::quiet_NaN()}});
    check_present_value("map_custom_compare",
                        std::map<std::string, int, std::greater<std::string>>{});
    check_present_value("unordered_map_empty",
                        std::unordered_map<std::string, std::string>{});
    check_present_value("unordered_map_nonempty",
                        std::unordered_map<std::string, std::string>{{"item", ""}});
    check_present_value("udt_without_na", PlainObject{});
    PlainObject fields;
    check_present_value("udt_array_field_empty", fields.items);
    fields.items.push_back("item");
    check_present_value("udt_array_field_nonempty", fields.items);

    check_default_value<std::vector<std::string>>("na_vector");
    check_default_value<std::map<std::string, double>>("na_map");
    check_default_value<std::unordered_map<std::string, std::string>>("na_unordered_map");
    check_default_value<std::string>("na_string");
    check_default_value<PlainObject>("na_plain_udt");
    check_flag_flip("member_flag_flip", FlaggedHandle{});
    check_default_handle<FlaggedHandle>("member_na_roundtrip");
    check_flag_flip("factory_member_flag_flip", FactoryHandle{});
    check_factory_handle<FactoryHandle>("factory_na_roundtrip");
    check_factory_handle<FactoryOnlyHandle>("factory_without_default_constructor");
    check_constant_false_mutant("mutant_constant_false_member", FlaggedHandle{});
    check_empty_mutant("mutant_empty_vector_is_na", std::vector<std::string>{});
    check_scalars();

    const std::vector<std::string> empty;
    const std::vector<std::string> populated{"item"};
    // Defined meaning: a plain vector value is present, so the generated
    // free overload and the engine agree on false.
    check_coexistence("generated_vector_empty", empty, false);
    check_coexistence("generated_vector_nonempty", populated, false);
    ArrayHistoryValue<std::vector<std::string>> history;
    check_coexistence("generated_history_missing", history, !history.value);
    check_value("engine_history_missing", history, !history.value);
    history.value = std::make_shared<std::vector<std::string>>();
    check_coexistence("generated_history_empty", history, !history.value);
    check_value("engine_history_empty", history, !history.value);
    check_default_handle<ArrayHistoryValue<std::vector<std::string>>>(
        "history_na_roundtrip");
    GeneratedObject object;
    check_coexistence("generated_udt_missing", object, object.id < 0);
    check_value("engine_generated_udt_missing", object, object.id < 0);
    check_value("generated_udt_na_roundtrip", pineforge::na<GeneratedObject>(),
                object.id < 0);
    object.id = 0;
    check_coexistence("generated_udt_present", object, object.id < 0);
    check_value("engine_generated_udt_present", object, object.id < 0);

    check_matrix<NumericMatrix>("numeric_matrix");
    check_matrix<GenericMatrix<double>>("matrix_double");
    check_matrix<GenericMatrix<int>>("matrix_int");
    check_matrix<GenericMatrix<std::int64_t>>("matrix_color_int64");
    check_matrix<GenericMatrix<bool>>("matrix_bool");
    check_matrix<GenericMatrix<std::string>>("matrix_string");
    check_matrix<GenericMatrix<PlainObject>>("matrix_udt");
    using Map = PineMap<std::string, double>;
    const Map missing_map;
    check_default_handle<Map>("pine_map_na_roundtrip");
    check_value("pine_map_missing", missing_map, missing_map.is_na());
    const auto empty_map = Map::new_();
    check_value("pine_map_empty", empty_map, empty_map.is_na());
    check_coexistence("pine_map_existing_overload", empty_map, empty_map.is_na());
    check_store_array<ScriptArray<int>>("store_array");
    check_store_array<ScriptArray<std::string>>("store_string_array");
    check_default_handle<NumericMatrix>("matrix_na_roundtrip");
    check_value("drawing_existing_overload", Line{}, Line{}.id < 0);
    check_value("drawing_existing_valid", Line{0}, Line{0}.id < 0);

    if (failures != 0) {
        std::printf("generic na: %d failed\n", failures);
        return 1;
    }
    std::printf("generic na: all passed\n");
    return 0;
}
