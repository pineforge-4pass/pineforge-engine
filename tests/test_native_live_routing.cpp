#include "routing.hpp"
#include "store.hpp"

#include <sqlite3.h>

#include <cassert>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <thread>
#include <unistd.h>

using namespace pineforge::live;

namespace {
const std::string routes = R"({"schema_version":1,"default_target":"main","targets":{
"main":{"url":"https://receiver.example/default","secret_env":"TEST_DEFAULT"},
"entries":{"url":"https://receiver.example/entries","secret_env":"TEST_ENTRIES"}},
"rules":[{"match":{"order_id":"Long","kind":"entry","side":"long"},"target":"entries"},
{"match":{"order_id":"Long"},"target":null},{"match":{"side":"short"},"target":"main"}]})";

template<class Callable>
void refuses(Callable&& callable, const std::string& expected = "") {
    bool refused = false;
    try { callable(); }
    catch (const std::exception& error) {
        refused = true;
        assert(expected.empty() || std::string(error.what()).find(expected) != std::string::npos);
    }
    assert(refused);
}

void configuration() {
    const auto config = parse_routes(routes, false);
    assert(config.routed);
    assert(config.select("Long", "entry", "long") == "entries");
    assert(!config.select("Long", "exit", "long"));
    assert(config.select("Other", "entry", "short") == "main");
    assert(config.select("long", "entry", "long") == "main");
    assert(config.delivery.max_in_flight == 8);
    assert(config.delivery.connect_timeout_ms == 2000);
    assert(config.delivery.total_timeout_ms == 5000);
    assert(config.delivery.transport_retries == 2);
    assert((config.delivery.retry_backoff_ms == std::vector<long>{1000, 2000}));
    assert(restore_routes(config.stored_document()).stored_document() == config.stored_document());
    assert(!single_target("", "", false).default_target);
    assert(single_target("https://receiver.example/", "", false).default_target == "default");
    assert(delivery_identity("event", "main") == sha256_hex("{\"event_id\":\"event\",\"target_id\":\"main\"}"));
    assert(delivery_identity("event", "main") != delivery_identity("event", std::nullopt));
    refuses([&] { parse_routes(routes, false, "https://wrong.example"); }, "agree");
    refuses([&] { parse_routes(routes, false, "", "WRONG_ENV"); }, "agree");
    for (const auto& selector : {R"({"alert_message":"message"})", R"({"kind":"close"})"}) {
        auto document = parse_json(routes);
        document.members["rules"].items[0].members["match"] = parse_json(selector);
        refuses([&] { parse_routes(document.dump(), false); }, "strategy-metadata extension");
    }
    for (const auto& selector : {R"({"kind":"ENTRY"})", R"({"side":"buy"})", R"({"order_id":3})", R"({"unknown":"x"})"}) {
        auto document = parse_json(routes);
        document.members["rules"].items[0].members["match"] = parse_json(selector);
        refuses([&] { parse_routes(document.dump(), false); });
    }
    for (const auto& delivery : {R"({"max_in_flight":0})", R"({"max_in_flight":1025})",
         R"({"connect_timeout_ms":0})", R"({"total_timeout_ms":1000})", R"({"transport_retries":3})",
         R"({"retry_backoff_ms":[1]})", R"({"retry_backoff_ms":[1,0]})", R"({"unknown":1})"}) {
        auto document = parse_json(routes);
        document.members["delivery"] = parse_json(delivery);
        refuses([&] { parse_routes(document.dump(), false); });
    }
    auto document = parse_json(routes);
    document.members["default_target"] = Json::string("absent");
    refuses([&] { parse_routes(document.dump(), false); }, "undefined");
    for (const auto& url : {"http://receiver.example/", "https://user:secret@receiver.example/", "https://receiver.example/#fragment", "file:///tmp/feed"}) {
        document = parse_json(routes);
        document.members["targets"].members["main"].members["url"] = Json::string(url);
        refuses([&] { parse_routes(document.dump(), false); });
    }
    document = parse_json(routes);
    document.members["delivery"] = parse_json(R"({"max_in_flight":3,"connect_timeout_ms":50,"total_timeout_ms":100,"transport_retries":1,"retry_backoff_ms":[10]})");
    const auto custom = parse_routes(document.dump(), false);
    assert(custom.delivery.max_in_flight == 3 && custom.delivery.total_timeout_ms == 100);
    assert(custom.delivery.transport_retries == 1 && custom.delivery.retry_backoff_ms[0] == 10);
}

void ledger_audit() {
    std::string pattern = (std::filesystem::temp_directory_path() / "pineforge-routing-XXXXXX").string();
    std::vector<char> name(pattern.begin(), pattern.end()); name.push_back('\0');
    const char* directory = mkdtemp(name.data());
    assert(directory);
    const auto path = std::string(directory) + "/ledger.sqlite";
    {
        Ledger ledger(path, "deployment");
        ledger.bind_routing(parse_routes(routes, false).stored_document());
        ledger.commit_input(0, "{}", 123, {{"first", "{\"sequence\":1}", "main", "delivery-first"},
                                         {"second", "{\"sequence\":2}", std::nullopt, "delivery-second"}});
        assert(ledger.unsent_count() == 1);
        const auto event = ledger.unsent_events(0).front();
        const auto attempt = ledger.start_attempt(event, 100);
        assert(attempt.attempt == 1);
        assert(ledger.unsent_count() == 1);
        ledger.finish_attempt(attempt, 101, 500, false, "http_status");
        assert(ledger.unsent_count() == 0);
        LedgerView view(path);
        assert(view.actions_after(0).size() == 2);
        assert(view.actions_after(1).front().target_id == std::nullopt);
        assert(view.redelivery_events("main", 1, true).size() == 1);
        const auto retry = ledger.start_attempt(event, 200);
        assert(retry.attempt == 2 && retry.event.delivery_id == attempt.event.delivery_id);
        ledger.finish_attempt(retry, 201, 204, true, "");
        assert(view.redelivery_events("main", 1, true).empty());
        const auto status = parse_json(view.status_json()).at("targets").at("main");
        assert(status.at("sent").integer<int>() == 1 && status.at("failed").integer<int>() == 1);
        std::thread writer([&] {
            for (std::uint64_t index = 1; index <= 50; ++index)
                ledger.commit_input(index, "{}", index, {});
        });
        for (int iteration = 0; iteration < 50; ++iteration) {
            assert(ledger.input(0)->state_hash == "123");
            assert(view.actions_after(0).size() == 2);
        }
        writer.join();
        assert(ledger.input_count() == 51);
    }
    {
        Ledger ledger(path, "deployment");
        assert(ledger.unsent_count() == 0);
    }
    sqlite3* database = nullptr;
    assert(sqlite3_open(path.c_str(), &database) == SQLITE_OK);
    assert(sqlite3_exec(database, "UPDATE delivery_log SET http_status=200", nullptr, nullptr, nullptr) != SQLITE_OK);
    assert(sqlite3_exec(database, "DELETE FROM delivery_log", nullptr, nullptr, nullptr) != SQLITE_OK);
    sqlite3_close(database);
    std::filesystem::remove_all(directory);
}
}

int main() {
    configuration();
    ledger_audit();
    std::cout << "PASS native routing configuration, identity, append-only delivery audit and serialized ledger\n";
}
