#pragma once

#include <cstdint>

namespace pineforge::detail::selected_capacity {
// Mirrors CsvReader's non-strict field tokenizer. Peeks never enter consume.
class RecordTracker {
public:
    explicit RecordTracker(std::uint64_t maximum) noexcept : maximum_(maximum) {}

    void begin_get() noexcept {
        if (ended_ && !after_cr_) next_record();
    }

    bool consume(unsigned char octet) noexcept {
        if (ended_ && !(after_cr_ && octet == '\n')) {
            next_record();
        }
        if (octets_ == maximum_) return false;
        ++octets_;
        if (after_cr_) {
            after_cr_ = false;
            return true;  // LF is the second octet of the same terminator.
        }
        if (field_ == Field::Quoted) {
            if (octet == '"') field_ = Field::QuoteSeen;
            return true;
        }
        if (field_ == Field::QuoteSeen) {
            if (octet == '"') {
                field_ = Field::Quoted;
                return true;
            }
            field_ = Field::Unquoted;  // Re-examine the octet outside quotes.
        }
        if (octet == '\n' || octet == '\r') {
            ended_ = true;
            after_cr_ = octet == '\r';
            field_ = Field::Start;
        } else if (octet == ',') {
            field_ = Field::Start;
        } else if (octet == '"' && field_ == Field::Start) {
            field_ = Field::Quoted;
        } else {
            field_ = Field::Unquoted;
        }
        return true;
    }

    std::uint64_t record_octets() const noexcept { return octets_; }
    std::uint64_t row_index() const noexcept { return row_; }

private:
    enum class Field { Start, Unquoted, Quoted, QuoteSeen };
    void next_record() noexcept {
        octets_ = 0;
        ++row_;
        field_ = Field::Start;
        ended_ = after_cr_ = false;
    }
    std::uint64_t maximum_, octets_ = 0, row_ = 0;
    Field field_ = Field::Start;
    bool ended_ = false, after_cr_ = false;
};
}  // namespace pineforge::detail::selected_capacity
