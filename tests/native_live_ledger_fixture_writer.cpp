// SPDX-License-Identifier: Apache-2.0
// Fixture producer for cross-version lab verification. This uses only the
// previous runner API and can be compiled against the previous support library.
#include "store.hpp"

#include <cstdio>
#include <exception>
#include <string>

using namespace pineforge::live;

int main(int argc, char** argv) {
    // Settle a prefix of a CLI-produced ledger through the actual writer API.
    // Source inputs/reports/actions and the last K routed events are untouched.
    if (argc == 4 && std::string(argv[1]) == "--settle-prefix") {
        try {
            const auto keep = std::stoull(argv[3]);
            LedgerView view(argv[2]);
            std::vector<StoredEvent> routed;
            std::uint64_t after = 0;
            for (;;) {
                const auto rows = view.actions_after(after);
                if (rows.empty()) break;
                for (const auto& row : rows) {
                    after = row.ordinal;
                    if (row.target_id) routed.push_back(row);
                }
            }
            if (routed.size() <= keep + 1)
                throw std::runtime_error("fixture needs routed history and a tail");
            Ledger ledger(argv[2], view.identity());
            for (std::size_t i = 0; i < routed.size() - keep; ++i) {
                const auto attempt = ledger.start_attempt(routed[i], 100 + i);
                const bool success = i + 1 != routed.size() - keep;
                ledger.finish_attempt(attempt, 200 + i, success ? 204 : 503,
                    success, success ? "" : "http_status");
            }
            return 0;
        } catch (const std::exception& error) {
            std::fprintf(stderr, "fixture prefix failed: %s\n", error.what());
            return 1;
        }
    }
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
