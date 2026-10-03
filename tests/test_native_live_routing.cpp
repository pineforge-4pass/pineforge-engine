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
        ledger.commit_input(51, "{}", 51, {{"third", "{\"sequence\":3}", "main", "delivery-third"},
                                          {"fourth", "{\"sequence\":4}", "main", "delivery-fourth"}});
        const auto concurrent = ledger.unsent_events(2);
        assert(concurrent.size() == 2);
        const auto older = ledger.start_attempt(concurrent[0], 300);
        const auto newer = ledger.start_attempt(concurrent[1], 400);
        ledger.finish_attempt(newer, 401, 204, true, "");
        ledger.finish_attempt(older, 500, 500, false, "http_status");
        const auto unordered = parse_json(view.status_json()).at("targets").at("main");
        assert(unordered.at("last_attempt").integer<std::uint64_t>() == 400);
        assert(unordered.at("last_success").integer<std::uint64_t>() == 401);
        assert(unordered.at("last_error").at("at").integer<std::uint64_t>() == 500);
        assert(unordered.at("sent").integer<int>() == 2 && unordered.at("failed").integer<int>() == 2);
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

void incremental_scan_and_migration() {
    std::string pattern = (std::filesystem::temp_directory_path() / "pineforge-scan-XXXXXX").string();
    std::vector<char> name(pattern.begin(), pattern.end()); name.push_back('\0');
    const char* directory = mkdtemp(name.data());
    assert(directory);
    const auto path = std::string(directory) + "/scan.sqlite";
    {
        Ledger ledger(path, "scan");
        std::vector<Event> events;
        for (unsigned index = 0; index < 4096; ++index)
            events.push_back({"journal-" + std::to_string(index), "{}", std::nullopt, ""});
        events.push_back({"failed", "{}", "main", "failed-delivery"});
        ledger.commit_input(0, "{}", 1, events);
        const auto failed = ledger.unsent_events(0).front();
        const auto attempt = ledger.start_attempt(failed, 1);
        ledger.finish_attempt(attempt, 2, 500, false, "http_status");
        std::uint64_t low = 0, visited = 0, steps = 0;
        while (const auto next = ledger.next_delivery_event(low, &steps)) {
            assert(steps < 150);
            assert(!next->unsent);
            low = next->event.ordinal;
            ++visited;
        }
        assert(visited == events.size());
        for (unsigned iteration = 0; iteration < 100; ++iteration) {
            assert(!ledger.next_delivery_event(low, &steps));
            assert(steps < 25);
        }
        ledger.commit_input(1, "{}", 2, {{"new", "{}", "main", "new-delivery"}});
        const auto next = ledger.next_delivery_event(low, &steps);
        assert(next && next->unsent && next->event.ordinal == low + 1 && steps < 150);
        assert(!ledger.next_delivery_event(next->event.ordinal, &steps) && steps < 25);
    }
    const auto legacy = std::string(directory) + "/phase-a.sqlite";
    sqlite3* database = nullptr;
    assert(sqlite3_open(legacy.c_str(), &database) == SQLITE_OK);
    assert(sqlite3_exec(database,
        "CREATE TABLE metadata(singleton INTEGER PRIMARY KEY,schema_version INTEGER,identity TEXT);"
        "CREATE TABLE inputs(input_index INTEGER PRIMARY KEY,canonical_json TEXT,state_hash TEXT);"
        "CREATE TABLE events(ordinal INTEGER PRIMARY KEY,input_index INTEGER,input_position INTEGER,event_id TEXT UNIQUE,"
        "payload TEXT,attempts INTEGER,acknowledged INTEGER,last_error TEXT);"
        "INSERT INTO metadata VALUES(1,1,'phase-a'); INSERT INTO inputs VALUES(0,'{}','123');"
        "INSERT INTO events VALUES(1,0,0,'ack','{\"sequence\":1}',2,1,'');"
        "INSERT INTO events VALUES(2,0,1,'unack','{ \"sequence\" : 2 }',2,0,'timeout');",
        nullptr, nullptr, nullptr) == SQLITE_OK);
    sqlite3_close(database);
    {
        Ledger ledger(legacy, "phase-a");
        const auto events = ledger.input(0)->events;
        assert(events.size() == 2 && events[0].id == "ack" && events[1].id == "unack");
        assert(events[1].payload == "{ \"sequence\" : 2 }");
        assert(ledger.unsent_count() == 1);
        const auto pending = ledger.unsent_events(0).front();
        assert(pending.id == "unack" && pending.delivery_id == "unack" && pending.attempts == 2);
        const auto attempt = ledger.start_attempt(pending, 3);
        assert(attempt.attempt == 3);
        ledger.finish_attempt(attempt, 4, 204, true, "");
        assert(ledger.unsent_count() == 0);
        assert(ledger.input(0)->events[1].payload == events[1].payload);
        LedgerView view(legacy);
        const auto status = parse_json(view.status_json()).at("targets").at("default");
        assert(status.at("sent").integer<int>() == 2 && status.at("unsent").integer<int>() == 0);
    }
    std::filesystem::remove_all(directory);
    std::cout << "PASS incremental delivery scan visits only new rows and phase-A three-table migration preserves bytes and attempts\n";
}
}

int main() {
    configuration();
    ledger_audit();
    incremental_scan_and_migration();
    std::cout << "PASS native routing configuration, identity, append-only delivery audit and serialized ledger\n";
}
