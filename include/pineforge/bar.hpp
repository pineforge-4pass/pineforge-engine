#pragma once
#include <cstdint>

namespace pineforge {

struct Bar {
    double open, high, low, close, volume;
    int64_t timestamp;  // Unix milliseconds
};

} // namespace pineforge
