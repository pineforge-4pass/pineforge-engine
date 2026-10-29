#include "metrics_only_strategy_fixture.hpp"

namespace metrics_only_fixture {
class UncertifiedHost : public TradingHost {};
} // namespace metrics_only_fixture

// Deliberately no MetricsPlainHostV1 specialization. Trading, marks and
// presentation are identical to the certified module; admission must differ.
PINEFORGE_EXPORT_NATIVE_STRATEGY(metrics_only_fixture::UncertifiedHost);
