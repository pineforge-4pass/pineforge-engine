#include "service.hpp"
#include "store.hpp"
#include <cassert>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <thread>
#include <unistd.h>

using namespace pineforge::live;
namespace fs = std::filesystem;

int main() {
    const auto root = fs::temp_directory_path() / ("pineforge-service-" + std::to_string(getpid()));
    fs::create_directory(root);
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
        std::this_thread::sleep_for(std::chrono::milliseconds(40));
        const auto periodic = read();
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
