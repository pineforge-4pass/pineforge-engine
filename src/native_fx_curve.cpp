#include <pineforge/native_fx_curve.hpp>

#include <cmath>
#include <cstring>

namespace pineforge {
inline namespace native_fx_curve_v1 {

NativeFxCurveValidation validate_native_fx_curve(const NativeFxCurve& curve) noexcept {
    if (curve.effective_from_ms.size() != curve.account_per_quote.size())
        return {NativeFxCurveError::LengthMismatch, 0};

    for (std::size_t i = 0; i < curve.effective_from_ms.size(); ++i) {
        if (i > 0 && curve.effective_from_ms[i] <= curve.effective_from_ms[i - 1])
            return {NativeFxCurveError::NotStrictlyIncreasing, i};
        if (!std::isfinite(curve.account_per_quote[i]) || curve.account_per_quote[i] <= 0.0)
            return {NativeFxCurveError::NotFinitePositive, i};
    }
    return {};
}

std::uint64_t native_fx_curve_digest(const NativeFxCurve& curve) noexcept {
    // Match the native consumer's FNV seed and exact object-byte word fold.
    std::uint64_t hash = 1469598103934665603ULL;
    const auto fold = [&hash](std::uint64_t word) noexcept {
        const auto* bytes = reinterpret_cast<const unsigned char*>(&word);
        for (std::size_t i = 0; i < sizeof(word); ++i) {
            hash ^= bytes[i];
            hash *= 1099511628211ULL;
        }
    };

    const auto timestamps = curve.effective_from_ms.size();
    const auto rates = curve.account_per_quote.size();
    fold(static_cast<std::uint64_t>(timestamps));
    fold(static_cast<std::uint64_t>(rates));
    const auto pairs = timestamps < rates ? timestamps : rates;
    for (std::size_t i = 0; i < pairs; ++i) {
        std::uint64_t timestamp_bits;
        std::uint64_t rate_bits;
        static_assert(sizeof(timestamp_bits) == sizeof(curve.effective_from_ms[i]));
        static_assert(sizeof(rate_bits) == sizeof(curve.account_per_quote[i]));
        std::memcpy(&timestamp_bits, &curve.effective_from_ms[i], sizeof(timestamp_bits));
        std::memcpy(&rate_bits, &curve.account_per_quote[i], sizeof(rate_bits));
        fold(timestamp_bits);
        fold(rate_bits);
    }
    return hash;
}

} // inline namespace native_fx_curve_v1
} // namespace pineforge
