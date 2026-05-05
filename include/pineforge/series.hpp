#pragma once
#include <deque>
#include "na.hpp"

namespace pineforge {

/*
 * PineScript history indexing for transpiled series (close, open, user vars, etc.)
 *
 * Pine uses expr[k] with k = 0 on the current bar, k >= 1 meaning k bars into the past
 * (close[1] = previous bar). Newest values live at the front of the deque; operator[](k)
 * reads k steps into the past. Codegen emits the same k for _s_close[k] and Series vars.
 *
 * push() — new bar; update() — same bar (magnifier intrabar). Negative or out-of-range
 * offsets return na (insufficient history).
 */

template<typename T>
class Series {
    std::deque<T> buf;
    int max_len;

public:
    explicit Series(int max_len = 500) : max_len(max_len) {}

    void push(T value) {
        buf.push_front(value);
        if ((int)buf.size() > max_len) {
            buf.pop_back();
        }
    }

    void update(T value) {
        if (buf.empty()) {
            push(value);
        } else {
            buf.front() = value;
        }
    }

    T operator[](int offset) const {
        if (offset < 0 || offset >= (int)buf.size()) {
            return na<T>();
        }
        return buf[offset];
    }

    T current() const {
        return (*this)[0];
    }

    int size() const {
        return (int)buf.size();
    }

    void clear() {
        buf.clear();
    }
};

} // namespace pineforge
