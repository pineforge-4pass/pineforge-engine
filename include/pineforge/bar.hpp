#pragma once
#include <cstdint>

namespace pineforge {

struct Bar {
    double open, high, low, close, volume;
    int64_t timestamp;  // Unix milliseconds
};

// One exchange trade update consumed by BacktestEngine's realtime stream.
// trade_id is optional (0 means unavailable); when present it must increase
// strictly so callers cannot silently replay or reorder exchange prints.
struct TradeTick {
    int64_t timestamp;       // Unix milliseconds
    uint64_t trade_id;
    double price;
    double qty;              // base-asset quantity, accumulated into volume
    bool is_buyer_maker;
};

} // namespace pineforge
