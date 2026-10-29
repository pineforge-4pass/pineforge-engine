#include "metrics_only_strategy_fixture.hpp"

namespace metrics_only_fixture {
class CertifiedHost : public TradingHost {};
} // namespace metrics_only_fixture

namespace pineforge::native_module {
template <>
struct MetricsPlainHostV1<metrics_only_fixture::CertifiedHost> {
    static std::optional<MetricsPlainDeclarationV1> declaration() {
        MetricsPlainDeclarationV1 out;
        out.builtin_presenter = out.builtin_marks = out.builtin_scheduler = 1;
        // TradingHost uses the inherited presenter and bar-close scheduler,
        // with one builtin mark per input. It declares no security, lower
        // timeframe, magnifier or history-tick calculation sites. Its sole
        // state resets at begin, so identical frozen inputs/settings replay
        // deterministically on a fresh handle.
        return out;
    }
};
} // namespace pineforge::native_module

PINEFORGE_EXPORT_NATIVE_STRATEGY(metrics_only_fixture::CertifiedHost);
