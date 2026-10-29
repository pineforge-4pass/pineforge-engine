// Compile the real scheduler against a scripted transport, as test_native_live_service.cpp does.
// Only barriers, operation counts, thread identities and durable SQL decide these rows.
#define WebhookMulti ProgressWebhookMulti
#define DeliveryWorker ProgressDeliveryWorker
#include "../runner/delivery.cpp"
#undef DeliveryWorker
#undef WebhookMulti

#include <condition_variable>
#include <cstdio>
#include <filesystem>
#include <mutex>
#include <sqlite3.h>

namespace pineforge::live {
namespace {
void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

struct Receipt {
    std::uint64_t key;
    StoredEvent event;
};

struct PollStep {
    std::size_t count;
    DeliveryResult result;
    bool throws = false;
};

struct TransportScript {
    explicit TransportScript(std::size_t initial) : initial(initial) {}

    const std::size_t initial;
    std::mutex mutex;
    std::condition_variable changed;
    // All fields below are accessed under mutex. The fake's request keys belong to its owner alone.
    std::set<std::thread::id> owners, actors;
    std::size_t constructed = 0, destroyed = 0, polls = 0, gated_polls = 0;
    bool polled_initial = false, parked = false, actor_released = false;
    bool gated = false, automatic = false, aborted = false;
    bool gate_retry_add = false, retry_add_waiting = false, retry_add_released = false;
    std::vector<Receipt> receipts;
    std::vector<CompletedWebhook> returned;
    std::vector<std::size_t> live_at_gate;
    std::deque<PollStep> steps;

    template<class Predicate> void await(Predicate ready) {
        std::unique_lock<std::mutex> lock(mutex);
        changed.wait(lock, [&] { return ready(*this); });
    }

    template<class Visitor> auto inspect(Visitor visitor) {
        std::lock_guard<std::mutex> lock(mutex);
        return visitor(*this);
    }

    bool stop_callback() {
        std::unique_lock<std::mutex> lock(mutex);
        actors.insert(std::this_thread::get_id());
        if (!parked && receipts.size() >= initial && polled_initial) {
            parked = true;
            gated = true;
            changed.notify_all();
            changed.wait(lock, [&] { return actor_released || aborted; });
        }
        return false;
    }

    void step(std::size_t count, DeliveryResult result, bool throws = false) {
        std::lock_guard<std::mutex> lock(mutex);
        steps.push_back({count, std::move(result), throws});
        changed.notify_all();
    }

    void resume(bool complete) {
        std::lock_guard<std::mutex> lock(mutex);
        automatic = complete;
        gated = false;
        actor_released = true;
        changed.notify_all();
    }

    void release_all() {
        std::lock_guard<std::mutex> lock(mutex);
        aborted = true;
        gated = false;
        actor_released = true;
        retry_add_released = true;
        changed.notify_all();
    }
};

std::atomic<TransportScript*> current_script{nullptr};
}

struct ProgressWebhookMulti::Impl {
    TransportScript& script;
    std::deque<std::uint64_t> keys;
    Impl() : script(*current_script.load(std::memory_order_acquire)) {}
};

ProgressWebhookMulti::ProgressWebhookMulti() : impl_(std::make_unique<Impl>()) {
    auto& script = impl_->script;
    std::lock_guard<std::mutex> lock(script.mutex);
    script.owners.insert(std::this_thread::get_id());
    ++script.constructed;
    script.changed.notify_all();
}

ProgressWebhookMulti::~ProgressWebhookMulti() {
    auto& script = impl_->script;
    std::lock_guard<std::mutex> lock(script.mutex);
    script.owners.insert(std::this_thread::get_id());
    ++script.destroyed;
    script.changed.notify_all();
}

void ProgressWebhookMulti::add(std::uint64_t key, const HttpOptions&, const StoredEvent& event) {
    auto& script = impl_->script;
    std::unique_lock<std::mutex> lock(script.mutex);
    script.owners.insert(std::this_thread::get_id());
    impl_->keys.push_back(key);
    script.receipts.push_back({key, event});
    if (script.gate_retry_add && script.receipts.size() == 2) {
        script.retry_add_waiting = true;
        script.changed.notify_all();
        script.changed.wait(lock, [&] { return script.retry_add_released || script.aborted; });
    }
    script.changed.notify_all();
}

std::vector<CompletedWebhook> ProgressWebhookMulti::poll(int) {
    auto& script = impl_->script;
    std::unique_lock<std::mutex> lock(script.mutex);
    script.owners.insert(std::this_thread::get_id());
    ++script.polls;
    if (script.receipts.size() >= script.initial) script.polled_initial = true;
    script.changed.notify_all();
    if (script.gated) {
        ++script.gated_polls;
        script.live_at_gate.push_back(impl_->keys.size());
        script.changed.notify_all();
        script.changed.wait(lock, [&] { return !script.gated || !script.steps.empty() || script.aborted; });
    }
    if (script.aborted) return {};
    PollStep step{0, {204, true, false, ""}};
    if (!script.steps.empty()) {
        step = std::move(script.steps.front());
        script.steps.pop_front();
    } else if (script.automatic) step.count = impl_->keys.size();
    if (step.throws) throw std::runtime_error("scripted transport owner failure");
    std::vector<CompletedWebhook> completed;
    while (step.count && !impl_->keys.empty()) {
        completed.push_back({impl_->keys.front(), step.result});
        impl_->keys.pop_front();
        --step.count;
    }
    script.returned.insert(script.returned.end(), completed.begin(), completed.end());
    script.changed.notify_all();
    return completed;
}

struct DeliveryWorkerTestAccess {
    static void cancel(ProgressDeliveryWorker& worker) { worker.cancelling_.store(true); }
};
}

using namespace pineforge::live;
namespace fs = std::filesystem;

namespace {
// Cleanup releases every fake/callback gate before joining, including when the ownership check is RED.
class WorkerFixture {
public:
    TransportScript script;
    std::unique_ptr<ProgressDeliveryWorker> worker;

    WorkerFixture(Ledger& ledger, std::size_t initial, DeliveryOptions settings = {}, bool gate_retry = false)
        : script(initial) {
        script.gate_retry_add = gate_retry;
        current_script.store(&script, std::memory_order_release);
        try {
            worker = std::make_unique<ProgressDeliveryWorker>(ledger, settings,
                std::map<std::string, HttpOptions>{{"main", HttpOptions{}}}, std::nullopt,
                [this] { return script.stop_callback(); });
        } catch (...) {
            current_script.store(nullptr, std::memory_order_release);
            throw;
        }
    }

    ~WorkerFixture() {
        script.release_all();
        if (worker) {
            try { worker->finish(true); } catch (...) {}
            worker.reset();
        }
        current_script.store(nullptr, std::memory_order_release);
    }

    void park_and_check_owner() {
        script.await([](const auto& s) { return s.parked; });
        script.inspect([](const auto& s) {
            const bool distinct = s.owners.size() == 1 && s.actors.size() == 1 &&
                *s.owners.begin() != *s.actors.begin();
            if (!distinct)
                std::fprintf(stderr, "transport ownership: owners=%zu actors=%zu owner=%zu actor=%zu adds=%zu polls=%zu\n",
                    s.owners.size(), s.actors.size(),
                    s.owners.empty() ? 0 : std::hash<std::thread::id>{}(*s.owners.begin()),
                    s.actors.empty() ? 0 : std::hash<std::thread::id>{}(*s.actors.begin()),
                    s.receipts.size(), s.polls);
            require(distinct,
                "transport owner count: expected exactly one transport thread distinct from the delivery actor");
            require(s.constructed == 1 && s.receipts.size() == s.initial && s.polled_initial,
                "ownership probe must include construction, add and poll of the initial requests");
        });
        await_poll(1);
    }

    void await_poll(std::size_t count) {
        script.await([&](const auto& s) { return s.gated_polls >= count; });
    }

    void check_joined() {
        script.inspect([](const auto& s) {
            require(s.constructed == 1 && s.destroyed == 1 && s.owners.size() == 1,
                "the same sole transport owner must destroy its transport before join returns");
        });
    }
};

std::int64_t count(const std::string& path, const char* sql) {
    sqlite3* raw = nullptr;
    const auto opened = sqlite3_open_v2(path.c_str(), &raw, SQLITE_OPEN_READONLY | SQLITE_OPEN_FULLMUTEX, nullptr);
    std::unique_ptr<sqlite3, decltype(&sqlite3_close_v2)> database(raw, &sqlite3_close_v2);
    require(opened == SQLITE_OK, "cannot open SQL observation connection");
    sqlite3_stmt* statement = nullptr;
    const auto prepared = sqlite3_prepare_v2(database.get(), sql, -1, &statement, nullptr);
    std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)> query(statement, &sqlite3_finalize);
    require(prepared == SQLITE_OK && sqlite3_step(query.get()) == SQLITE_ROW, "cannot read SQL observation");
    return sqlite3_column_int64(query.get(), 0);
}

void admit(Ledger& ledger, unsigned size) {
    std::vector<Event> events;
    for (unsigned index = 1; index <= size; ++index) {
        const auto suffix = std::to_string(index);
        events.push_back({"event-" + suffix, "{\"sequence\":" + suffix + "}", "main", "delivery-" + suffix});
    }
    ledger.commit_input(0, "{}", 123, events);
}

void unacknowledged(const std::string& path, std::int64_t started, std::int64_t events) {
    require(count(path, "SELECT count(*) FROM delivery_log WHERE phase='started'") == started,
        "unexpected durable start count while the delivery actor is parked");
    require(count(path, "SELECT count(*) FROM delivery_log WHERE phase='completed'") == 0,
        "an in-memory completion must not create a durable finish");
    require(count(path, "SELECT count(*) FROM events WHERE acknowledged=0") == events,
        "an in-memory completion must not acknowledge an event");
}

void one_pair_each(const std::string& path, std::int64_t events) {
    require(count(path, "SELECT count(*) FROM events WHERE acknowledged=1 AND attempts=1 AND last_error=''") == events,
        "normal drain must acknowledge every event exactly once");
    require(count(path, "SELECT count(*) FROM delivery_log") == events * 2,
        "normal drain must leave exactly one started/completed pair per event");
    require(count(path, "SELECT count(*) FROM delivery_log s JOIN delivery_log c "
        "ON s.event_id=c.event_id AND s.attempt=c.attempt WHERE s.phase='started' AND c.phase='completed' "
        "AND c.success=1 AND s.started_at=c.started_at AND s.target_id=c.target_id "
        "AND s.delivery_id=c.delivery_id AND s.request_id=c.request_id") == events,
        "durable completions must preserve their attempt and delivery identity");
}

void progress_while_parked(const fs::path& root) {
    const auto path = (root / "progress.sqlite").string();
    Ledger ledger(path, "deployment");
    admit(ledger, 2);
    WorkerFixture fixture(ledger, 2);
    fixture.park_and_check_owner(); // The first verdict on the old worker is the owner-count failure.
    unacknowledged(path, 2, 2);
    fixture.script.step(1, {201, true, false, ""});
    fixture.await_poll(2); // The first result has returned; another poll has begun with D still parked.
    fixture.script.inspect([](const auto& s) {
        require(!s.actor_released && s.gated_polls == 2 && s.live_at_gate == std::vector<std::size_t>{2, 1},
            "transport must poll the second handle after returning the first result while D is parked");
        require(s.returned.size() == 1 && s.returned[0].result.status == 201 && s.returned[0].result.success,
            "the first completion must be an immutable success");
    });
    unacknowledged(path, 2, 2);
    fixture.script.step(1, {202, true, false, ""});
    fixture.await_poll(3);
    unacknowledged(path, 2, 2);
    fixture.script.resume(true);
    fixture.worker->finish();
    fixture.check_joined();
    one_pair_each(path, 2);
    require(fixture.worker->delivered() == 2 && fixture.worker->failed() == 0 && fixture.worker->queue_bytes() == 0,
        "drain counters must include both fixed successful completions");
    require(count(path, "SELECT count(*) FROM delivery_log WHERE phase='completed' AND "
        "((event_id='event-1' AND http_status=201) OR (event_id='event-2' AND http_status=202))") == 2,
        "SQL must retain the exact results returned before D resumed");
}

void full_result_backlog(const fs::path& root) {
    const auto path = (root / "backlog.sqlite").string();
    Ledger ledger(path, "deployment");
    admit(ledger, 257);
    DeliveryOptions settings;
    settings.max_in_flight = 256;
    WorkerFixture fixture(ledger, 256, settings);
    fixture.park_and_check_owner();
    fixture.script.step(256, {207, true, false, ""});
    fixture.await_poll(2);
    fixture.script.inspect([](const auto& s) {
        require(!s.actor_released && s.receipts.size() == 256 && s.returned.size() == 256,
            "all 256 channel credits must tolerate pending completions without D consuming them");
        require(s.live_at_gate == std::vector<std::size_t>{256, 0},
            "the owner must keep polling after publishing a full result backlog");
    });
    unacknowledged(path, 256, 257);
    require(count(path, "SELECT count(*) FROM events WHERE attempts=0") == 1,
        "pending completions must retain their outstanding credits");
    fixture.script.resume(true);
    fixture.worker->finish();
    fixture.check_joined();
    one_pair_each(path, 257);
    require(count(path, "SELECT count(*) FROM delivery_log WHERE phase='completed' AND http_status=207") == 256,
        "the entire queued result backlog must survive until SQL persistence");
    require(fixture.worker->delivered() == 257 && fixture.worker->failed() == 0,
        "released credits must admit the remaining event");
}

void cancellation_with_queued_result(const fs::path& root) {
    const auto path = (root / "cancel.sqlite").string();
    Ledger ledger(path, "deployment");
    admit(ledger, 2);
    std::vector<Receipt> original;
    {
        WorkerFixture fixture(ledger, 2);
        fixture.park_and_check_owner();
        fixture.script.step(1, {201, true, false, ""});
        fixture.await_poll(2);
        unacknowledged(path, 2, 2);
        original = fixture.script.inspect([](const auto& s) { return s.receipts; });
        DeliveryWorkerTestAccess::cancel(*fixture.worker);
        fixture.script.resume(false);
        fixture.worker->finish(true);
        fixture.check_joined();
        unacknowledged(path, 2, 2);
        require(fixture.worker->delivered() == 0 && fixture.worker->failed() == 0,
            "cancellation must discard a queued completion without classifying it");
    }
    {
        WorkerFixture fixture(ledger, 2);
        fixture.park_and_check_owner();
        fixture.script.resume(true);
        fixture.worker->finish();
        fixture.check_joined();
        fixture.script.inspect([&](const auto& s) {
            require(s.receipts.size() == original.size(), "restart must send each unfinished event once");
            for (std::size_t index = 0; index < original.size(); ++index)
                require(s.receipts[index].event.delivery_id == original[index].event.delivery_id &&
                    s.receipts[index].event.payload == original[index].event.payload,
                    "restart must reuse immutable delivery bytes");
        });
    }
    require(count(path, "SELECT count(*) FROM delivery_log WHERE phase='completed' AND attempt=1") == 0,
        "a cancelled in-memory result must never acquire a completion row later");
    require(count(path, "SELECT count(*) FROM delivery_log WHERE phase='started' AND attempt=2") == 2 &&
        count(path, "SELECT count(*) FROM delivery_log WHERE phase='completed' AND attempt=2 AND success=1") == 2 &&
        count(path, "SELECT count(*) FROM events WHERE attempts=2 AND acknowledged=1") == 2,
        "restart must durably finish attempt two for both cancelled events");
}

void owner_exception(const fs::path& root) {
    const auto path = (root / "exception.sqlite").string();
    Ledger ledger(path, "deployment");
    admit(ledger, 2);
    WorkerFixture fixture(ledger, 2);
    fixture.park_and_check_owner();
    fixture.script.step(0, {}, true);
    fixture.script.await([](const auto& s) { return s.destroyed == 1; });
    unacknowledged(path, 2, 2);
    fixture.script.resume(false);
    bool finish_failed = false;
    try { fixture.worker->finish(); }
    catch (const std::runtime_error& error) {
        finish_failed = std::string(error.what()) == "scripted transport owner failure";
    }
    require(finish_failed, "finish must propagate the transport owner's exception");
    fixture.check_joined();
    bool check_failed = false;
    try { fixture.worker->check(); }
    catch (const std::runtime_error& error) {
        check_failed = std::string(error.what()) ==
            "webhook delivery worker failed; committed actions remain in the ledger";
    }
    require(check_failed, "owner exceptions must enter the existing worker-failure path");
    unacknowledged(path, 2, 2);
}

void retry_after_durable_failure(const fs::path& root) {
    const auto path = (root / "retry.sqlite").string();
    Ledger ledger(path, "deployment");
    admit(ledger, 1);
    DeliveryOptions settings;
    settings.transport_retries = 1;
    settings.retry_backoff_ms = {0}; // No timing verdict: the second add is the barrier.
    WorkerFixture fixture(ledger, 1, settings, true);
    fixture.park_and_check_owner();
    fixture.script.step(1, {0, false, true, "timeout"});
    fixture.await_poll(2);
    unacknowledged(path, 1, 1);
    fixture.script.resume(false);
    fixture.script.await([](const auto& s) { return s.retry_add_waiting; });
    require(count(path, "SELECT count(*) FROM delivery_log WHERE phase='completed' AND attempt=1 "
        "AND http_status=0 AND success=0 AND error_category='timeout'") == 1 &&
        count(path, "SELECT count(*) FROM events WHERE attempts=2 AND acknowledged=0 AND last_error='timeout'") == 1,
        "credit reuse and a retry send must follow the durable failure");
    fixture.script.inspect([](auto& s) {
        require(s.receipts.size() == 2 && s.receipts[0].key != s.receipts[1].key &&
            s.receipts[0].event.id == s.receipts[1].event.id &&
            s.receipts[0].event.delivery_id == s.receipts[1].event.delivery_id &&
            s.receipts[0].event.payload == s.receipts[1].event.payload,
            "a retry must use a fresh transport key with the same immutable delivery bytes");
        s.automatic = true;
        s.retry_add_released = true;
        s.changed.notify_all();
    });
    fixture.worker->finish();
    fixture.check_joined();
    require(fixture.worker->delivered() == 1 && fixture.worker->failed() == 1 &&
        fixture.worker->queue_bytes() == 0 && ledger.unsent_count() == 0,
        "normal drain must retain the retry until its successful durable finish");
    require(count(path, "SELECT count(*) FROM delivery_log") == 4 &&
        count(path, "SELECT count(*) FROM delivery_log WHERE phase='completed' AND attempt=2 AND success=1 AND http_status=204") == 1 &&
        count(path, "SELECT count(*) FROM events WHERE attempts=2 AND acknowledged=1 AND last_error=''") == 1,
        "retry must leave exactly two complete durable attempt pairs and one acknowledgement");
}
}

int main() {
    const auto root = fs::temp_directory_path() / ("pineforge-transport-progress-" + std::to_string(getpid()));
    fs::create_directory(root);
    try {
        progress_while_parked(root);
        full_result_backlog(root);
        cancellation_with_queued_result(root);
        owner_exception(root);
        retry_after_durable_failure(root);
    } catch (const std::exception& error) {
        std::fprintf(stderr, "FAIL %s\n", error.what());
        fs::remove_all(root);
        return 1;
    }
    fs::remove_all(root);
    std::puts("PASS transport ownership, ledger-independent progress, result credits, cancellation, exception and retry");
}
