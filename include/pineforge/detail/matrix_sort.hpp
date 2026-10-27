#pragma once

#include <pineforge/detail/matrix_store.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <limits>
#include <stdexcept>

namespace pineforge::detail::matrix_store {
namespace sorting {

// std::sort's permutation lives in the same Store as the matrix. Dereferencing
// this iterator moves one index, never a native row or a growing index vector.
class IndexReference {
public:
    IndexReference(Cells& cells, std::uint64_t index) noexcept
        : cells_(&cells), index_(index) {}
    IndexReference(const IndexReference&) = default;
    operator std::uint64_t() const { return read_value<std::uint64_t>(*cells_, index_); }
    IndexReference& operator=(std::uint64_t value) {
        write_value(*cells_, index_, value);
        return *this;
    }
    IndexReference& operator=(const IndexReference& other) {
        return *this = static_cast<std::uint64_t>(other);
    }
    friend void swap(IndexReference first, IndexReference second) {
        const auto value = static_cast<std::uint64_t>(first);
        first = second;
        second = value;
    }
private:
    Cells* cells_;
    std::uint64_t index_;
};

class IndexIterator {
public:
    using iterator_category = std::random_access_iterator_tag;
    using value_type = std::uint64_t;
    using difference_type = std::ptrdiff_t;
    using reference = IndexReference;
    using pointer = void;

    IndexIterator() noexcept = default;
    IndexIterator(Cells& cells, difference_type index) noexcept
        : cells_(&cells), index_(index) {}
    reference operator*() const { return {*cells_, static_cast<std::uint64_t>(index_)}; }
    reference operator[](difference_type offset) const { return *(*this + offset); }
    IndexIterator& operator++() noexcept { ++index_; return *this; }
    IndexIterator& operator--() noexcept { --index_; return *this; }
    IndexIterator operator++(int) noexcept { auto old = *this; ++*this; return old; }
    IndexIterator operator--(int) noexcept { auto old = *this; --*this; return old; }
    IndexIterator& operator+=(difference_type offset) noexcept { index_ += offset; return *this; }
    IndexIterator& operator-=(difference_type offset) noexcept { index_ -= offset; return *this; }
    friend IndexIterator operator+(IndexIterator it, difference_type n) noexcept { return it += n; }
    friend IndexIterator operator+(difference_type n, IndexIterator it) noexcept { return it += n; }
    friend IndexIterator operator-(IndexIterator it, difference_type n) noexcept { return it -= n; }
    friend difference_type operator-(IndexIterator a, IndexIterator b) noexcept { return a.index_ - b.index_; }
    friend bool operator==(IndexIterator a, IndexIterator b) noexcept {
        return a.cells_ == b.cells_ && a.index_ == b.index_;
    }
    friend bool operator!=(IndexIterator a, IndexIterator b) noexcept { return !(a == b); }
    friend bool operator<(IndexIterator a, IndexIterator b) noexcept { return a.index_ < b.index_; }
    friend bool operator>(IndexIterator a, IndexIterator b) noexcept { return b < a; }
    friend bool operator<=(IndexIterator a, IndexIterator b) noexcept { return !(b < a); }
    friend bool operator>=(IndexIterator a, IndexIterator b) noexcept { return !(a < b); }
private:
    Cells* cells_ = nullptr;
    difference_type index_ = 0;
};

} // namespace sorting

// Compare takes two ORIGINAL row numbers. Matrix bytes stay untouched while
// sorting the permutation, so repeated keys follow the existing std::sort
// comparator path. The only cell workspace is Cells::copy_cell's fixed buffer.
// std::sort still supplies its own control stack. The allocation test checks
// heap workspace; a fixed stack-space certificate is a separate obligation.
template<class Compare>
inline void sort_rows(Cells& cells, Compare compare) {
    const auto rows = cells.rows(), columns = cells.columns();
    if (rows < 2) return;
    if (rows > static_cast<std::uint64_t>(std::numeric_limits<std::ptrdiff_t>::max()))
        throw std::overflow_error("matrix iterator address space exhausted");
    Scope transaction(cells.runtime());
    Cells order(cells.runtime(), rows, 1, sizeof(std::uint64_t));
    for (std::uint64_t r = 0; r < rows; ++r) write_value(order, r, r);
    std::sort(sorting::IndexIterator(order, 0),
              sorting::IndexIterator(order, static_cast<std::ptrdiff_t>(rows)), compare);
    const auto frozen = cells.clone();
    for (std::uint64_t r = 0; r < rows; ++r) {
        const auto from = read_value<std::uint64_t>(order, r);
        for (std::uint64_t c = 0; c < columns; ++c)
            cells.copy_cell(r * columns + c, *frozen, from * columns + c);
    }
    frozen->close();
    order.close();
    transaction.commit();
}

} // namespace pineforge::detail::matrix_store
