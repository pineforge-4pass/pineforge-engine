// This test compiles the production delivery worker (runner/delivery.cpp) into itself under other class names and
// gives it the fake transport defined below, instead of the one pineforge_live_support links. That lets it stop the
// worker at an exact point of its loop and hand it a chosen batch of completions, which no real HTTP receiver can.
// The renames keep the fake clear of the real WebhookMulti and DeliveryWorker; the fake must keep the interface
// delivery.cpp uses (the constructor, add and poll, and the completed-result fields).
#define WebhookMulti StopPollWebhookMulti
#define DeliveryWorker StopPollDeliveryWorker
#include "../runner/delivery.cpp"
#undef DeliveryWorker
#undef WebhookMulti

#include "service.hpp"
#include "store.hpp"
#include <cassert>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <thread>
#include <sys/stat.h>
#include <unistd.h>
#include <sqlite3.h>

namespace pineforge::live {
std::atomic<bool> stop_on_completed_poll{false};
bool discard_completed_poll = false;
std::size_t returned_completions = 0;
std::vector<std::pair<std::string, std::string>> polled_receipts;

struct StopPollWebhookMulti::Impl {
    std::vector<std::uint64_t> keys;
};

StopPollWebhookMulti::StopPollWebhookMulti() : impl_(std::make_unique<Impl>()) {}
StopPollWebhookMulti::~StopPollWebhookMulti() = default;

void StopPollWebhookMulti::add(std::uint64_t key, const HttpOptions&, const StoredEvent& event) {
    impl_->keys.push_back(key);
    polled_receipts.emplace_back(event.delivery_id, event.payload);
}

std::vector<CompletedWebhook> StopPollWebhookMulti::poll(int) {
    std::vector<CompletedWebhook> completed;
    for (const auto key : impl_->keys) completed.push_back({key, {204, true, false, ""}});
    impl_->keys.clear();
    returned_completions += completed.size();
    if (discard_completed_poll && !completed.empty()) stop_on_completed_poll = true;
    return completed;
}
}

using namespace pineforge::live;
namespace fs = std::filesystem;

void completed_poll_stop(const fs::path& root) {
    const auto path = (root / "completed-poll.sqlite").string();
    Ledger ledger(path, "deployment");
    ledger.commit_input(0, "{}", 123, {{"first", "{\"sequence\":1}", "main", "delivery-first"},
        {"second", "{\"sequence\":2}", "main", "delivery-second"},
        {"third", "{\"sequence\":3}", "main", "delivery-third"},
        {"fourth", "{\"sequence\":4}", "main", "delivery-fourth"}});
    const auto count = [&](const char* sql) {
        sqlite3* database = nullptr;
        assert(sqlite3_open(path.c_str(), &database) == SQLITE_OK);
        sqlite3_stmt* statement = nullptr;
        assert(sqlite3_prepare_v2(database, sql, -1, &statement, nullptr) == SQLITE_OK);
        assert(sqlite3_step(statement) == SQLITE_ROW);
        const auto result = sqlite3_column_int64(statement, 0);
        sqlite3_finalize(statement);
        sqlite3_close(database);
        return result;
    };
    DeliveryOptions settings;
    settings.max_in_flight = 4;
    discard_completed_poll = true;
    {
        StopPollDeliveryWorker worker(ledger, settings, {{"main", HttpOptions{}}}, std::nullopt,
            [] { return stop_on_completed_poll.load(); });
        worker.finish();
        assert(returned_completions == 4 && stop_on_completed_poll);
        assert(worker.delivered() == 0 && worker.failed() == 0);
    }
    assert(ledger.unsent_count() == 4);
    assert(count("SELECT count(*) FROM delivery_log WHERE phase='started'") == 4);
    assert(count("SELECT count(*) FROM delivery_log WHERE phase='completed'") == 0);
    assert(count("SELECT count(*) FROM events WHERE attempts=1") == 4);
    const auto original = polled_receipts;
    stop_on_completed_poll = false;
    discard_completed_poll = false;
    {
        StopPollDeliveryWorker worker(ledger, settings, {{"main", HttpOptions{}}});
        worker.finish();
        assert(worker.delivered() == 4 && worker.failed() == 0);
    }
    assert(ledger.unsent_count() == 0);
    assert(count("SELECT count(*) FROM delivery_log WHERE phase='completed' AND attempt=1") == 0);
    assert(count("SELECT count(*) FROM delivery_log WHERE phase='completed' AND attempt=2 AND success=1") == 4);
    assert(polled_receipts.size() == 8);
    assert(std::equal(original.begin(), original.end(), polled_receipts.begin() + 4));
    std::puts("PASS stop discards an already-returned completion batch; restart reuses keys and records one completion per attempt");
}

namespace pineforge::live {
// delivery.hpp makes this a friend: finish() cannot be called from the worker's own thread (it joins that thread),
// so the test sets the request the way finish() does, without the join.
struct DeliveryWorkerTestAccess {
    static void request_finish(StopPollDeliveryWorker& worker) { worker.finishing_ = true; }
};
}

// The runner calls finish() right after the last input's commit, and the worker must send that action however the
// commit falls against its loop. It used to decide that nothing was left from a ledger scan at the top of an
// iteration and to read the finish request after the scans, so an action committed in between was skipped and the
// run exited 0 with it unsent. The stop callback runs on the worker thread, after the delivery scan and before the
// exit test, so committing the action and requesting the finish from inside it reproduces a descheduled worker
// deterministically. The callback fires at each position of the loop in turn, so the row does not depend on how
// many callbacks one iteration makes.
void commit_races_finish(const fs::path& root) {
    for (unsigned position = 1; position <= 9; ++position) {
        const auto path = (root / ("finish-" + std::to_string(position) + ".sqlite")).string();
        Ledger ledger(path, "deployment");
        std::atomic<StopPollDeliveryWorker*> armed{nullptr};
        std::atomic<bool> fired{false};
        unsigned seen = 0;
        StopPollDeliveryWorker worker(ledger, DeliveryOptions{}, {{"main", HttpOptions{}}}, std::nullopt, [&] {
            auto* self = armed.load();
            if (!self || fired.load() || ++seen < position) return false;
            ledger.commit_input(0, "{}", 1, {{"last", "{\"sequence\":1}", "main", "delivery-last"}});
            DeliveryWorkerTestAccess::request_finish(*self);
            fired = true;
            return false;
        });
        armed = &worker;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (!fired && std::chrono::steady_clock::now() < deadline)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        worker.finish();
        assert(fired);
        if (ledger.unsent_count() != 0 || worker.delivered() != 1 || worker.failed() != 0)
            std::fprintf(stderr, "FAIL commit at stop-callback position %u: %llu unsent, %llu delivered, %llu failed\n",
                         position, static_cast<unsigned long long>(ledger.unsent_count()),
                         static_cast<unsigned long long>(worker.delivered()),
                         static_cast<unsigned long long>(worker.failed()));
        assert(ledger.unsent_count() == 0 && worker.delivered() == 1 && worker.failed() == 0);
    }
    std::puts("PASS an action committed just before finish() is sent at every position of the delivery loop");
}

int main() {
    const auto root = fs::temp_directory_path() / ("pineforge-service-" + std::to_string(getpid()));
    fs::create_directory(root);
    completed_poll_stop(root);
    commit_races_finish(root);
    const auto status = (root / "status.json").string();
    auto healthy = Json::object({{"schema_version", Json::string("pineforge-live-status/v1")},
        {"ready", Json::boolean(true)}, {"liveness", Json::object({{"alive", Json::boolean(true)},
            {"control_loop_heartbeat_ms", Json::number(std::to_string(wall_time_ms()))}})}});
    assert(probe_status(healthy, 1000, true));
    healthy.members["ready"] = Json::boolean(false);
    assert(probe_status(healthy, 1000, false));
    assert(!probe_status(healthy, 1000, true));
    healthy.members["liveness"].members["control_loop_heartbeat_ms"] = Json::number("1");
    assert(!probe_status(healthy, 1000, false));
    {
        ServiceFile writer(status, 10);
        writer.publish(healthy, true);
        const auto read = [&] {
            std::ifstream file(status);
            return parse_json(std::string(std::istreambuf_iterator<char>(file), {}));
        };
        const auto initial = read();
        auto periodic = initial;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (periodic.at("written_at_ms").value == initial.at("written_at_ms").value &&
               std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            periodic = read();
        }
        assert(periodic.at("written_at_ms").integer<std::uint64_t>() > initial.at("written_at_ms").integer<std::uint64_t>());
        assert(periodic.at("liveness").at("control_loop_heartbeat_ms").value == initial.at("liveness").at("control_loop_heartbeat_ms").value);
        writer.stop("storage_budget");
        assert(!probe_status(read(), 1000, false));
    }
    ControlDirectory controls((root / "control").string());
    const auto request = [&](const std::string& identifier) {
        return Json::object({{"schema_version", Json::string("pineforge-redelivery-request/v1")},
            {"deployment", Json::string("deployment")}, {"request_id", Json::string(identifier)},
            {"target", Json::string("default")}, {"from", Json::number("1")}, {"failed_only", Json::boolean(true)}});
    };
    bool called = false;
    const auto accept = [&](const Json&) { called = true; return std::uint64_t{2}; };
    const std::string first(64, 'a');
    controls.submit(request(first));
    controls.poll("deployment", accept);
    assert(called && fs::exists(root / "control" / (first + ".ack.json")));
    called = false;
    const std::string publishing(64, 'd');
    const auto staging = root / "control" / "publishing.new";
    std::ofstream(staging) << request(publishing).dump();
    fs::create_hard_link(staging, root / "control" / (publishing + ".request.json"));
    controls.poll("deployment", accept);
    assert(called && fs::exists(root / "control" / (publishing + ".ack.json")));
    fs::remove(staging);
    for (const auto invalid : {Json::number("0"), Json::string("1"), Json::number("18446744073709551615")}) {
        called = false;
        auto malformed = request(std::string(64, 'b'));
        malformed.members["from"] = invalid;
        controls.submit(malformed);
        controls.poll("deployment", accept);
        assert(!called);
    }
    called = false;
    controls.submit(request(std::string(64, 'c')));
    controls.poll("other", accept);
    assert(!called);
    bool missing_refused = false;
    try { ControlDirectory missing((root / "missing").string(), false); }
    catch (const std::runtime_error& error) {
        missing_refused = std::string(error.what()) == "control directory does not exist";
    }
    assert(missing_refused && !fs::exists(root / "missing"));
    const std::string transient(64, '9');
    controls.submit(request(transient));
    controls.poll("deployment", [](const Json&) -> std::uint64_t {
        throw std::runtime_error("temporary storage failure with private details");
    });
    {
        std::ifstream file(root / "control" / (transient + ".ack.json"));
        const auto acknowledgement = parse_json(std::string(std::istreambuf_iterator<char>(file), {}));
        assert(acknowledgement.at("accepted").value == "false");
        assert(acknowledgement.at("reason").text() == "accept_failure");
        assert(acknowledgement.at("selected").integer<std::uint64_t>() == 0);
    }
    assert(!fs::exists(root / "control" / (transient + ".request.json")));
    const auto directory = root / "control";
    fs::create_directory(directory / (std::string(64, '1') + ".request.json"));
    fs::create_symlink(root / "outside", directory / (std::string(64, '2') + ".request.json"));
    const auto unreadable = directory / (std::string(64, '3') + ".request.json");
    std::ofstream(unreadable) << "{}";
    chmod(unreadable.c_str(), 0);
    std::ofstream(directory / (std::string(64, '4') + ".request.json")) << std::string(20000, 'x');
    std::ofstream(directory / (std::string(64, '5') + ".request.json")) << "{";
    controls.poll("deployment", accept);
    assert(controls.errors() == 5);
    controls.poll("deployment", accept);
    assert(controls.errors() == 5);
    chmod(unreadable.c_str(), 0600);
    for (unsigned index = 0; index < 270; ++index) {
        auto identifier = std::string(61, 'e') + std::to_string(100 + index);
        std::ofstream(directory / (identifier + ".ack.json")) << "{}";
    }
    controls.poll("deployment", accept);
    std::size_t acknowledgements = 0;
    for (const auto& entry : fs::directory_iterator(directory))
        if (entry.path().filename().string().find(".ack.json") != std::string::npos) ++acknowledgements;
    assert(acknowledgements <= 256);
    const std::string denied(64, 'f');
    controls.submit(request(denied));
    chmod(directory.c_str(), 0500);
    controls.poll("deployment", accept);
    chmod(directory.c_str(), 0700);
    assert(controls.errors() == 6 && fs::exists(directory / (denied + ".request.json")));
    fs::remove_all(directory);
    controls.poll("deployment", accept);
    assert(controls.errors() == 7);
    controls.poll("deployment", accept);
    assert(controls.errors() == 7);
    fs::create_directory(directory);
    chmod(directory.c_str(), 0755);
    controls.poll("deployment", accept);
    assert(controls.errors() == 8);
    chmod(directory.c_str(), 0700);
    controls.poll("deployment", accept);
    assert(controls.errors() == 8);
    fs::remove_all(directory);
    controls.poll("deployment", accept);
    assert(controls.errors() == 9);
    {
        Ledger ledger((root / "ledger.sqlite3").string(), "deployment");
        StoredEvent event;
        event.id = "event"; event.payload = "{}"; event.target_id = "default"; event.delivery_id = "delivery";
        ledger.commit_input(0, "{}", 1, {event}, "{}");
        assert(ledger.report_cursor() == 1);
        auto stored = ledger.input(0)->events.front();
        auto attempt = ledger.start_attempt(stored, wall_time_ms());
        ledger.finish_attempt(attempt, wall_time_ms(), 503, false, "http_status");
        assert(ledger.request_redelivery(first, "default", 1, true) == 1);
        assert(ledger.request_redelivery(first, "default", 1, true) == 1);
        const auto selected = ledger.next_redelivery_event(0, 0);
        assert(selected && selected->event.delivery_id == stored.delivery_id && selected->request_id == first);
        attempt = ledger.start_attempt(selected->event, wall_time_ms(), first);
        ledger.finish_attempt(attempt, wall_time_ms(), 204, true, "");
        assert(!ledger.next_redelivery_event(0, 0));
        assert(ledger.request_redelivery(first, "default", 1, true) == 1);
        assert(ledger_bytes((root / "ledger.sqlite3").string()) > 0);
    }
    fs::remove_all(root);
}
