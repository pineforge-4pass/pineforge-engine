// B2-13: the existing removal contract remains -3 while a scratch-backed
// generation is live. This pins the reachable guard after the dead branch is
// removed.
#include "report_modes_fix_fixture.hpp"

using namespace report_modes_fix;

int main() {
    try {
        Host host;
        configure(host);
        host.hold_lots = 2;
        const auto path = sink_path("p2-removal-guard");
        install_sink(host, path);
        run(host, tape(8));
        require(host.set_report_sink_v1(nullptr) == -3,
                "scratch-backed sink removal did not retain the -3 guard");
        std::puts("B2-13 scratch-backed sink removal: -3");
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "B2-13: %s\n", error.what());
        return 1;
    }
}
