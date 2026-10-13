#pragma once

// Internal source-door admission. Each signature mirrors the raw host command;
// ignored quantity/percent/default operands still travel unchanged to the adapter.
// The L0 throw helper and its closed field vocabulary live in the other header.
#include <cstdint>
#include <string>

namespace pineforge::source {

void admit_strategy_entry(const std::string& id, bool is_long,
                          double limit_price, double stop_price, double qty,
                          const std::string& comment, const std::string& oca_name,
                          int oca_type, int qty_type);
void admit_strategy_close(const std::string& id, const std::string& comment,
                          double qty, double qty_percent, bool immediately);
void admit_strategy_close_token(const std::string& id, const std::string& comment,
                                double qty, double qty_percent, bool immediately,
                                std::uint64_t callsite_token);
void admit_strategy_close_all() noexcept;
void admit_strategy_exit(const std::string& id, const std::string& from_entry,
                         double limit_price, double stop_price,
                         double trail_points, double trail_offset, double trail_price,
                         double qty_percent, const std::string& comment, double qty,
                         const std::string& oca_name, double profit_ticks, double loss_ticks);
void admit_strategy_exit_cancel_bracket(const std::string& exit_id,
                                        const std::string& from_entry,
                                        const std::string& comment);
void admit_strategy_cancel(const std::string& id);
void admit_strategy_cancel_all() noexcept;
void admit_strategy_order(const std::string& id, bool is_long, double qty,
                          double limit_price, double stop_price,
                          const std::string& oca_name, int oca_type);

} // namespace pineforge::source
