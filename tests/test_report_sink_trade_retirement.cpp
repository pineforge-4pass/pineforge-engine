#include "report_sink_trade_retirement_fixture.hpp"

extern "C" __attribute__((visibility("default")))
int strategy_reads_trade_history() { return 0; }

int main(int argc, char** argv) {
    return trade_retirement_fixture::main_impl(argc, argv, false, true);
}
