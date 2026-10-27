#include "equity_writer_test_access.hpp"
#include <pineforge/source/pine_strategy_host.hpp>

// Match tutorial/macd/generated.cpp's generated-strategy inheritance.
class GeneratedStrategy final : public pineforge::source::PineStrategyHost {
public:
    void on_source_bar(const pineforge::Bar&) override {
        (void)equity_writer_test::last(*this);
    }
};
