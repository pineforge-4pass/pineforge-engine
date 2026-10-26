#include "pine_value_admit_doors.hpp"
#include "pine_value_admission.hpp"

#include <cmath>
#include <cstddef>
#include <string_view>

namespace pineforge::source {
namespace {

// Unicode scalar UTF-8, using the string's length rather than a C terminator.
// NUL is ASCII; the first continuation bounds exclude overlong encodings,
// surrogate code points and values beyond U+10FFFF.
bool valid_utf8(std::string_view text) noexcept {
    std::size_t index = 0;
    while (index < text.size()) {
        const auto lead = static_cast<unsigned char>(text[index++]);
        if (lead < 0x80) continue;
        std::size_t tails = 0;
        if (lead >= 0xc2 && lead <= 0xdf) tails = 1;
        else if (lead >= 0xe0 && lead <= 0xef) tails = 2;
        else if (lead >= 0xf0 && lead <= 0xf4) tails = 3;
        else return false;
        if (tails > text.size() - index) return false;
        for (std::size_t tail = 0; tail < tails; ++tail) {
            const auto byte = static_cast<unsigned char>(text[index + tail]);
            if (byte < 0x80 || byte > 0xbf) return false;
            if (tail == 0) {
                if (lead == 0xe0 && byte < 0xa0) return false;
                if (lead == 0xed && byte > 0x9f) return false;
                if (lead == 0xf0 && byte < 0x90) return false;
                if (lead == 0xf4 && byte > 0x8f) return false;
            }
        }
        index += tails;
    }
    return true;
}

void admit_comment(const std::string& text, const char* field) {
    if (!valid_utf8(text))
        throw_strategy_value_invalid(field, StrategyValueReason::invalid_unicode);
}

void admit_text(const std::string& text, const char* field) {
    admit_comment(text, field);
    // IDs, entry selectors and OCA names feed separator-delimited adapter
    // keys. Inspect the entire string, including bytes after an embedded NUL.
    if (text.find('\x1f') != std::string::npos || text.find('\0') != std::string::npos) {
        const std::string_view name(field);
        const auto dot = name.find('.');
        throw coded<std::invalid_argument>(
            RunFailureCode::pine_invalid_argument,
            RunFailureArgs{{"function", std::string("strategy.") + std::string(name.substr(0, dot))},
                           {"argument", std::string(name.substr(dot + 1))},
                           {"rule", "unsupported_combination"}},
            "strategy identifiers cannot contain NUL or unit separator");
    }
}

void admit_level(double value, const char* field) {
    // D1: a raw NaN is the absent level. Finite arithmetic is lane S3's.
    if (std::isinf(value))
        throw_strategy_value_invalid(field, StrategyValueReason::nonfinite);
}

} // namespace

void admit_strategy_entry(const std::string& id, bool,
                          double limit_price, double stop_price, double,
                          const std::string& comment, const std::string& oca_name,
                          int, int) {
    admit_text(id, value_field::entry_id);
    admit_level(limit_price, value_field::entry_limit_price);
    admit_level(stop_price, value_field::entry_stop_price);
    admit_comment(comment, value_field::entry_comment);
    admit_text(oca_name, value_field::entry_oca_name);
    // Entry qty NaN and both infinities keep the documented default.
}

void admit_strategy_close(const std::string& id, const std::string& comment,
                          double, double, bool) {
    admit_text(id, value_field::close_id);
    admit_comment(comment, value_field::close_comment);
}

void admit_strategy_close_token(const std::string& id, const std::string& comment,
                                double qty, double qty_percent, bool immediately,
                                std::uint64_t) {
    admit_strategy_close(id, comment, qty, qty_percent, immediately);
}

void admit_strategy_close_all() noexcept {}

void admit_strategy_exit(const std::string& id, const std::string& from_entry,
                         double limit_price, double,
                         double, double trail_offset, double trail_price,
                         double, const std::string& comment, double,
                         const std::string& oca_name, double profit_ticks, double loss_ticks) {
    admit_text(id, value_field::exit_id);
    admit_text(from_entry, value_field::exit_from_entry);
    admit_level(limit_price, value_field::exit_limit_price);
    // Rule 4: test_fills_edge and its l4d twin pin infinity as actionable for
    // stop_price and trail_points. Those arguments remain unadmitted.
    admit_level(trail_offset, value_field::exit_trail_offset);
    admit_level(trail_price, value_field::exit_trail_price);
    admit_comment(comment, value_field::exit_comment);
    admit_text(oca_name, value_field::exit_oca_name);
    admit_level(profit_ticks, value_field::exit_profit_ticks);
    admit_level(loss_ticks, value_field::exit_loss_ticks);
    // Exit qty/qty_percent retain their non-explicit/default semantics.
}

void admit_strategy_exit_cancel_bracket(const std::string& exit_id,
                                        const std::string& from_entry,
                                        const std::string& comment) {
    admit_text(exit_id, value_field::exit_cancel_bracket_exit_id);
    admit_text(from_entry, value_field::exit_cancel_bracket_from_entry);
    admit_comment(comment, value_field::exit_cancel_bracket_comment);
}

void admit_strategy_cancel(const std::string& id) {
    admit_text(id, value_field::cancel_id);
}

void admit_strategy_cancel_all() noexcept {}

void admit_strategy_order(const std::string& id, bool, double,
                          double limit_price, double stop_price,
                          const std::string& oca_name, int) {
    admit_text(id, value_field::order_id);
    admit_level(limit_price, value_field::order_limit_price);
    admit_level(stop_price, value_field::order_stop_price);
    admit_text(oca_name, value_field::order_oca_name);
    // The CANCEL path retains non-finite qty for default host sizing and its
    // full-fill sibling cancellation predicate. Ordinary qty NaN is default.
}

} // namespace pineforge::source
