#include <pineforge/native_fx_curve.hpp>

#include <cmath>
#include <cstring>
#include <iostream>
#include <limits>
#include <type_traits>
#include <utility>

namespace {
using namespace pineforge::native_fx_curve_v1;
using Error = NativeFxCurveError;

static_assert(std::is_same_v<std::underlying_type_t<Error>, std::uint8_t>);
static_assert(static_cast<std::uint8_t>(Error::None) == 0);
static_assert(static_cast<std::uint8_t>(Error::LengthMismatch) == 1);
static_assert(static_cast<std::uint8_t>(Error::NotStrictlyIncreasing) == 2);
static_assert(static_cast<std::uint8_t>(Error::NotFinitePositive) == 3);
static_assert(static_cast<std::uint8_t>(Error::AllocationFailure) == 4);
static_assert(static_cast<std::uint8_t>(Error::WrongPhase) == 5);
static_assert(noexcept(validate_native_fx_curve(std::declval<const NativeFxCurve&>())));
static_assert(noexcept(native_fx_curve_digest(std::declval<const NativeFxCurve&>())));

int checks = 0;
int failures = 0;

void check(bool passed, const char* description) {
    ++checks;
    if (!passed) {
        ++failures;
        std::cerr << "FAIL: " << description << '\n';
    }
}

std::uint64_t rate_bits(double rate) noexcept {
    std::uint64_t bits;
    static_assert(sizeof(bits) == sizeof(rate));
    std::memcpy(&bits, &rate, sizeof(bits));
    return bits;
}

void expect_validation(const NativeFxCurve& curve, Error error, std::size_t index,
                       const char* description) {
    const auto before = curve;
    const auto result = validate_native_fx_curve(curve);
    check(result.error == error && result.index == index, description);
    bool unchanged = curve.effective_from_ms == before.effective_from_ms
        && curve.account_per_quote.size() == before.account_per_quote.size();
    for (std::size_t i = 0; i < curve.account_per_quote.size(); ++i)
        unchanged = unchanged
            && rate_bits(curve.account_per_quote[i]) == rate_bits(before.account_per_quote[i]);
    check(unchanged, "validation preserves all timestamp and rate bits");
}

void validation_rows() {
    const NativeFxCurveValidation default_result;
    check(default_result.error == Error::None && default_result.index == 0,
          "default validation is None/index 0");
    expect_validation({}, Error::None, 0, "empty curve is valid for clearing");
    expect_validation({{-1}, {1.0}}, Error::None, 0, "a negative effective timestamp is valid");
    expect_validation({{0}, {1.0}}, Error::None, 0, "a single epoch-zero row is valid");
    expect_validation({{std::numeric_limits<std::int64_t>::min(), -1, 0,
                        std::numeric_limits<std::int64_t>::max()},
                       {std::numeric_limits<double>::denorm_min(),
                        std::numeric_limits<double>::min(), 1.0,
                        std::numeric_limits<double>::max()}},
                      Error::None, 0, "full timestamp range and finite positive rate extremes are valid");

    expect_validation({{0}, {}}, Error::LengthMismatch, 0, "missing rate is a length mismatch");
    expect_validation({{}, {1.0}}, Error::LengthMismatch, 0, "missing timestamp is a length mismatch");
    expect_validation({{0, 1}, {1.0}}, Error::LengthMismatch, 0, "shorter rates are refused");
    expect_validation({{0}, {1.0, 2.0}}, Error::LengthMismatch, 0, "longer rates are refused");
    expect_validation({{2, 1}, {-1.0}}, Error::LengthMismatch, 0,
                      "length mismatch precedes timestamp and rate checks");

    expect_validation({{1, 1}, {1.0, 2.0}}, Error::NotStrictlyIncreasing, 1,
                      "duplicate timestamps are refused at their first duplicate");
    expect_validation({{1, 2, 1}, {1.0, 2.0, 3.0}}, Error::NotStrictlyIncreasing, 2,
                      "decreasing timestamp reports its array index");
    expect_validation({{1, 1}, {1.0, -1.0}}, Error::NotStrictlyIncreasing, 1,
                      "timestamp failure precedes rate failure at the same index");
    expect_validation({{0, 1, 1}, {1.0, -1.0, 1.0}}, Error::NotFinitePositive, 1,
                      "an earlier invalid rate precedes a later timestamp failure");
    expect_validation({{0, 0, 1}, {1.0, 1.0, -1.0}}, Error::NotStrictlyIncreasing, 1,
                      "an earlier timestamp failure precedes a later invalid rate");
    expect_validation({{1, 1}, {-1.0, 1.0}}, Error::NotFinitePositive, 0,
                      "validation scans rows before checking later timestamps");

    for (const double rate : {0.0, -0.0, -1.0,
                              std::numeric_limits<double>::quiet_NaN(),
                              std::numeric_limits<double>::infinity(),
                              -std::numeric_limits<double>::infinity()}) {
        expect_validation({{0}, {rate}}, Error::NotFinitePositive, 0,
                          "nonfinite and nonpositive first rate is refused");
        expect_validation({{0, 1, 2}, {1.0, 2.0, rate}}, Error::NotFinitePositive, 2,
                          "nonfinite and nonpositive later rate reports its index");
        expect_validation({{0, 1, 1}, {1.0, 2.0, rate}}, Error::NotStrictlyIncreasing, 2,
                          "timestamp failure precedes every invalid-rate category");
    }
    expect_validation({{0, 1, 2}, {1.0, -1.0, -2.0}}, Error::NotFinitePositive, 1,
                      "multiple rate failures report the first index");
}

void digest_rows() {
    const NativeFxCurve empty;
    const NativeFxCurve curve{{-1000, 0, 1000}, {0.5, 1.0, 2.0}};
    const auto digest = native_fx_curve_digest(curve);
    check(digest == 0x21FD430CDDD42428ULL,
          "validated curve has the stable length-prefixed FNV digest");
    check(digest == native_fx_curve_digest(curve), "digest is repeatable");
    auto copy = curve;
    check(copy.account_per_quote.data() != curve.account_per_quote.data(),
          "copy uses independent storage");
    copy.effective_from_ms.reserve(32);
    copy.account_per_quote.reserve(64);
    check(digest == native_fx_curve_digest(copy), "digest ignores addresses and vector capacities");
    check(digest != native_fx_curve_digest(empty), "empty and populated curves differ");

    for (std::size_t i = 0; i < curve.effective_from_ms.size(); ++i) {
        auto changed = curve;
        ++changed.effective_from_ms[i];
        check(digest != native_fx_curve_digest(changed), "every timestamp contributes to the digest");
        changed = curve;
        changed.account_per_quote[i] = std::nextafter(changed.account_per_quote[i], 3.0);
        check(rate_bits(changed.account_per_quote[i]) != rate_bits(curve.account_per_quote[i]),
              "adjacent-rate witness changes the exact bits");
        check(digest != native_fx_curve_digest(changed), "every rate contributes its exact bits");
    }
    copy = curve;
    std::swap(copy.account_per_quote[0], copy.account_per_quote[2]);
    check(digest != native_fx_curve_digest(copy), "rate-to-timestamp pairing and array order are retained");
    copy = curve;
    copy.effective_from_ms.push_back(2000);
    copy.account_per_quote.push_back(2.0);
    check(digest != native_fx_curve_digest(copy), "appending a pair changes the digest");
    copy = curve;
    copy.effective_from_ms.push_back(2000);
    const auto timestamp_length_mismatch = native_fx_curve_digest(copy);
    check(timestamp_length_mismatch != digest,
          "an extra timestamp changes the length-prefixed digest");
    check(timestamp_length_mismatch == native_fx_curve_digest(copy),
          "timestamp-length mismatch digest is deterministic");
    copy = curve;
    copy.account_per_quote.push_back(3.0);
    const auto rate_length_mismatch = native_fx_curve_digest(copy);
    check(rate_length_mismatch != digest,
          "an extra rate changes the length-prefixed digest");
    check(rate_length_mismatch == native_fx_curve_digest(copy),
          "rate-length mismatch digest is deterministic");
    copy.effective_from_ms.clear();
    copy.account_per_quote.clear();
    check(native_fx_curve_digest(empty) == 0xA31E272015F12C43ULL,
          "empty curve folds two zero lengths");
    check(native_fx_curve_digest(copy) == native_fx_curve_digest(empty),
          "cleared arrays have the empty-curve digest");
    const NativeFxCurve tiny{{0}, {std::numeric_limits<double>::denorm_min()}};
    copy = tiny;
    copy.account_per_quote[0] = std::nextafter(copy.account_per_quote[0], 1.0);
    check(native_fx_curve_digest(tiny) != native_fx_curve_digest(copy),
          "adjacent subnormal positive rates remain distinct");
}
} // namespace

int main() {
    validation_rows();
    digest_rows();
    std::cout << (checks - failures) << '/' << checks << " checks passed; "
              << failures << " failed\n";
    return failures == 0 ? 0 : 1;
}
