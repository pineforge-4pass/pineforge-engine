#pragma once

namespace pineforge::execution {
inline namespace reverse_to_v1 {

// One resolved execution closes the opposite live book and opens this exact
// signed exposure. This is not a signed transaction flow or a queued request.
// The value is local to inspection/projection/settlement and is never retained.
struct ReverseTo {
    double signed_units = 0.0;
};

} // inline namespace reverse_to_v1
} // namespace pineforge::execution
