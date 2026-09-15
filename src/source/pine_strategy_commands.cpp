#include <pineforge/source/pine_strategy_host.hpp>

namespace pineforge::source {

void PineStrategyHost::strategy_entry(const std::string& id, bool is_long,
                                      double limit_price, double stop_price, double qty,
                                      const std::string& comment,
                                      const std::string& oca_name, int oca_type,
                                      int qty_type) {
    adapter_.entry(id, is_long, limit_price, stop_price, qty, comment, oca_name,
                   oca_type, qty_type);
}

void PineStrategyHost::strategy_close(const std::string& id, const std::string& comment,
                                      double qty, double qty_percent, bool immediately) {
    adapter_.close(id, comment, qty, qty_percent, immediately);
}

void PineStrategyHost::strategy_close(const std::string& id, const std::string& comment,
                                      double qty, double qty_percent, bool immediately,
                                      std::uint64_t callsite_token) {
    adapter_.close(id, comment, qty, qty_percent, immediately, callsite_token);
}

void PineStrategyHost::strategy_close_all() {
    adapter_.close_all();
}

void PineStrategyHost::strategy_exit(const std::string& id, const std::string& from_entry,
                                     double limit_price, double stop_price,
                                     double trail_points, double trail_offset,
                                     double trail_price, double qty_percent,
                                     const std::string& comment, double qty,
                                     const std::string& oca_name,
                                     double profit_ticks, double loss_ticks) {
    adapter_.exit(id, from_entry, limit_price, stop_price, trail_points, trail_offset,
                  trail_price, qty_percent, comment, qty, oca_name, profit_ticks,
                  loss_ticks);
}

void PineStrategyHost::strategy_exit_cancel_bracket(const std::string& exit_id,
                                                    const std::string& from_entry,
                                                    const std::string& comment) {
    adapter_.exit_cancel_bracket(exit_id, from_entry, comment);
}

void PineStrategyHost::strategy_cancel(const std::string& id) {
    adapter_.cancel(id);
}

void PineStrategyHost::strategy_cancel_all() {
    adapter_.cancel_all();
}

void PineStrategyHost::strategy_order(const std::string& id, bool is_long, double qty,
                                      double limit_price, double stop_price,
                                      const std::string& oca_name, int oca_type) {
    adapter_.order(id, is_long, qty, limit_price, stop_price, oca_name, oca_type);
}

} // namespace pineforge::source
