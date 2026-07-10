#pragma once
#include <cstdint>

namespace pineforge {

struct Bar {
    double open, high, low, close, volume;
    int64_t timestamp;  // Unix milliseconds
};

// One provider-neutral executed-trade update consumed by BacktestEngine's
// realtime stream. `sequence` is assigned by the normalized data source: zero
// means unavailable, while non-zero values must increase strictly so callers
// cannot silently replay or reorder records.
struct TradeTick {
    int64_t timestamp;       // Unix milliseconds
    uint64_t sequence;
    double price;
    double quantity;         // normalized size, accumulated into bar volume
};

} // namespace pineforge
