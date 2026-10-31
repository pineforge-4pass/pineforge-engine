#pragma once

#include <pineforge/bar.hpp>

#include <cstddef>
#include <cstdint>
#include <stdexcept>

namespace pineforge {

// A feed is addressed by its logical bar index, independently of residency.
// read() copies exact records into caller storage. A valid cold index must
// read from the provider; only an invalid range or an actual provider failure
// may fail. No pointer into a provider's transfer/cache window escapes.
//
// A reader installed in a projection is an immutable snapshot: its size and
// records stay fixed for that snapshot's lifetime. A changed feed uses a new
// reader. Implementations preserve their typed backing I/O failures.
template<class Row>
class FeedReader {
public:
    virtual ~FeedReader() = default;
    virtual std::size_t size() const noexcept = 0;
    virtual void read(std::size_t first, Row* destination,
                      std::size_t count) const = 0;

    Row at(std::size_t index) const {
        Row row{};
        read(index, &row, 1);
        return row;
    }
};

struct FeedBar {
    Bar bar{};
    std::int64_t close_ms = 0;
};

// An opt-in bridge for an existing caller-owned pair of arrays. It neither
// copies nor takes ownership; the caller retains both arrays, unchanged, for
// the reader's lifetime. Existing raw-array engine entry points keep their
// existing ownership contracts.
class BorrowedBarFeedReader final : public FeedReader<FeedBar> {
public:
    BorrowedBarFeedReader(const Bar* bars, const std::int64_t* close_ms,
                          std::size_t count)
        : bars_(bars), close_ms_(close_ms), count_(count) {
        if (count && (!bars || !close_ms))
            throw std::invalid_argument("feed reader arrays");
    }
    std::size_t size() const noexcept override { return count_; }
    void read(std::size_t first, FeedBar* destination,
              std::size_t count) const override {
        if (first > count_ || count > count_ - first)
            throw std::out_of_range("feed reader range");
        if (count && !destination)
            throw std::invalid_argument("feed reader destination");
        for (std::size_t i = 0; i < count; ++i)
            destination[i] = FeedBar{bars_[first + i], close_ms_[first + i]};
    }
private:
    const Bar* bars_;
    const std::int64_t* close_ms_;
    std::size_t count_;
};

} // namespace pineforge
