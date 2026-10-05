#include <pineforge/source/pine_strategy_host.hpp>
#include <fstream>
#include <iomanip>

extern "C" int lv_d8_export_events(void* state, const char* path) {
    auto* host = dynamic_cast<pineforge::source::PineStrategyHost*>(
        static_cast<pineforge::BacktestEngine*>(state));
    if (!host || !path) return -1;
    std::ofstream output(path);
    output << std::setprecision(17);
    namespace orders = pineforge::native_order;
    for (const auto& event : host->native_events(0)) {
        if (!event.command) continue;
        if (const auto* rejected = std::get_if<orders::MatchRejectedEvent>(&*event.command)) {
            output << "{\"kind\":\"match-rejected\",\"incarnation\":"
                   << rejected->handle().incarnation << ",\"reason\":"
                   << static_cast<int>(rejected->reason) << ",\"timestamp\":"
                   << rejected->cursor.point.effective_time_ms << "}\n";
        } else if (const auto* terms = std::get_if<orders::TermsResolvedEvent>(&*event.command)) {
            output << "{\"kind\":\"terms\",\"incarnation\":"
                   << terms->handle().incarnation << ",\"timestamp\":"
                   << terms->cursor.point.effective_time_ms << ",\"raw_price\":"
                   << terms->input.raw_price << ",\"price\":"
                   << terms->input.terms.resolved_price << ",\"units\":"
                   << terms->input.terms.units.value_or(-1.0) << "}\n";
        }
    }
    return output ? 0 : -1;
}
