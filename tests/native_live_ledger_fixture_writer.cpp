// SPDX-License-Identifier: Apache-2.0
// Fixture producer for cross-version lab verification. This uses only the
// previous runner API and can be compiled against the previous support library.
#include "store.hpp"

#include <cstdio>
#include <exception>

using namespace pineforge::live;

int main(int argc, char** argv) {
    if (argc != 2) {
        std::fprintf(stderr, "usage: native_live_ledger_fixture_writer NEW_LEDGER\n");
        return 2;
    }
    try {
        Ledger ledger(argv[1], "previous-produced");
        if (ledger.input_count()) {
            std::fprintf(stderr, "fixture output must be a fresh ledger\n");
            return 2;
        }
        ledger.commit_input(0, "{\"input\":0}", 11, {
            {"legacy", "{ \"keep\" : 1 }", "main", "d1"},
            {"unknown", "{}", "main", "d2"}, {"failed", "{}", "main", "d3"}});
        ledger.commit_input(1, "{\"input\":1}", 12, {
            {"success", "{}", "side", "d4"}, {"known", "{}", "side", "d5"},
            {"journal", "{}", std::nullopt, "d6"}});
        ledger.begin_delivery("legacy");
        ledger.acknowledge("legacy");
        const auto first = ledger.input(0)->events;
        const auto failed = ledger.start_attempt(first[2], 100);
        ledger.finish_attempt(failed, 110, 503, false, "http_status");
        const auto second = ledger.input(1)->events;
        const auto success = ledger.start_attempt(second[0], 180);
        ledger.finish_attempt(success, 190, 204, true, "");
    } catch (const std::exception& error) {
        std::fprintf(stderr, "fixture write failed: %s\n", error.what());
        return 1;
    }
    return 0;
}
