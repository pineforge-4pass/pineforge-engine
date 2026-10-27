#include "report_sink_trade_retirement_fixture.hpp"

extern "C" __attribute__((visibility("default")))
int pf_strategy_reads_history_by_index_v1() { return 1; }

int main(int argc, char** argv) {
    return trade_retirement_fixture::main_impl(argc, argv, true, false);
}
