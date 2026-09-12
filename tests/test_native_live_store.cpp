// SPDX-License-Identifier: Apache-2.0
#include "../runner/store.hpp"
#include "../runner/transport.hpp"

#include <sqlite3.h>

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <functional>
#include <limits>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

using namespace pineforge::live;

namespace {
int failures = 0;
#define CHECK(cond) do { if (!(cond)) { \
    std::fprintf(stderr, "FAIL %s:%d %s\n", __FILE__, __LINE__, #cond); ++failures; } } while (0)

template<class F> bool throws(F&& f) {
    try { f(); } catch (const std::exception&) { return true; }
    return false;
}

struct TempDir {
    std::string path;
    TempDir() {
        std::string pattern = (std::filesystem::temp_directory_path() / "pineforge-native-store-XXXXXX").string();
        std::vector<char> value(pattern.begin(), pattern.end());
        value.push_back('\0');
        const auto* result = mkdtemp(value.data());
        if (!result) throw std::runtime_error("cannot create test directory");
        path = result;
    }
    ~TempDir() { std::error_code ec; std::filesystem::remove_all(path, ec); }
    std::string file(const std::string& name) const { return path + "/" + name; }
};

void execute_sql(const std::string& path, const char* sql) {
    sqlite3* db = nullptr;
    if (sqlite3_open(path.c_str(), &db) != SQLITE_OK) throw std::runtime_error("test SQL open failed");
    const auto rc = sqlite3_exec(db, sql, nullptr, nullptr, nullptr);
    sqlite3_close(db);
    if (rc != SQLITE_OK) throw std::runtime_error("test SQL failed");
}

std::string scalar_text(const std::string& path, const char* sql) {
    sqlite3* db = nullptr;
    if (sqlite3_open_v2(path.c_str(), &db, SQLITE_OPEN_READONLY, nullptr) != SQLITE_OK)
        throw std::runtime_error("test SQL open failed");
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK || sqlite3_step(stmt) != SQLITE_ROW)
        throw std::runtime_error("test SQL scalar failed");
    const std::string result(reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0)));
    sqlite3_finalize(stmt);
    sqlite3_close(db);
    return result;
}

void ledger_transactions() {
    TempDir temp;
    const auto path = temp.file("orders.sqlite");
    const std::vector<Event> events{{"first", "{\"order\":1}"}, {"second", "{\"order\":2}"}};
    {
        Ledger ledger(path, "deployment-v1");
        CHECK(ledger.input_count() == 0);
        CHECK(!ledger.input(0));
        CHECK(!ledger.pending_event());
        CHECK(throws([&] { Ledger second(path, "deployment-v1"); }));
        std::filesystem::create_symlink(path, temp.file("alias.sqlite"));
        CHECK(throws([&] { Ledger alias(temp.file("alias.sqlite"), "deployment-v1"); }));
        CHECK(throws([&] { ledger.commit_input(1, "{}", 1, {}); }));
        ledger.commit_input(0, "{\"type\":\"tick\"}", std::numeric_limits<std::uint64_t>::max(), events);
        CHECK(ledger.input_count() == 1);
        CHECK(ledger.input(0)->state_hash == "18446744073709551615");
        CHECK(ledger.pending_count() == 2);
        CHECK(ledger.pending_event()->ordinal == 1);
        ledger.commit_input(0, "{\"type\":\"tick\"}", std::numeric_limits<std::uint64_t>::max(), events);
        CHECK(ledger.pending_count() == 2);
        CHECK(throws([&] { ledger.commit_input(0, "{}", std::numeric_limits<std::uint64_t>::max(), events); }));
        CHECK(throws([&] { ledger.commit_input(0, "{\"type\":\"tick\"}", 1, events); }));
        CHECK(throws([&] { ledger.commit_input(0, "{\"type\":\"tick\"}",
            std::numeric_limits<std::uint64_t>::max(), {events[1], events[0]}); }));
        // A duplicate id fails after INSERT inputs, proving transaction rollback.
        CHECK(throws([&] { ledger.commit_input(1, "{\"type\":\"time\"}", 2,
                                                   {{"third", "{}"}, {"first", "{}"}}); }));
        CHECK(ledger.input_count() == 1);
        CHECK(ledger.pending_count() == 2);
        CHECK(!ledger.input(1));
        CHECK(throws([&] { ledger.acknowledge("first"); }));
        CHECK(throws([&] { ledger.begin_delivery("second"); }));
        ledger.begin_delivery("first");
        ledger.record_delivery_failure("first", "timeout");
        CHECK(ledger.pending_event()->id == "first");
        CHECK(ledger.pending_event()->attempts == 1);
        ledger.begin_delivery("first");
        ledger.record_delivery_failure("first", "https://private/token?secret=do-not-store");
        CHECK(ledger.pending_event()->attempts == 2);
        CHECK(ledger.pending_event()->payload == events[0].payload);
    }
    CHECK(scalar_text(path, "SELECT last_error FROM events WHERE event_id='first'") == "delivery_failed");
    CHECK(throws([&] { Ledger wrong(path, "different-deployment"); }));
    {
        Ledger recovered(path, "deployment-v1");
        CHECK(recovered.pending_event()->attempts == 2);
        recovered.begin_delivery("first");
        recovered.acknowledge("first");
        CHECK(recovered.pending_event()->id == "second");
        CHECK(recovered.pending_event()->ordinal == 2);
        recovered.begin_delivery("second");
        recovered.acknowledge("second");
        CHECK(!recovered.pending_event());
        CHECK(recovered.pending_count() == 0);
        CHECK(recovered.input(0)->events.size() == 2);
        CHECK(recovered.input(0)->events[0].attempts == 3);
        // Zero-event inputs are still durable and participate in recovery.
        recovered.commit_input(1, "{\"type\":\"time\"}", 2, {});
        CHECK(recovered.input_count() == 2);
    }
    {
        Ledger recovered(path, "deployment-v1");
        CHECK(recovered.input_count() == 2);
        CHECK(recovered.pending_count() == 0);
    }
    struct stat st {};
    CHECK(stat(path.c_str(), &st) == 0 && (st.st_mode & 0777) == 0600);
    execute_sql(path, "DELETE FROM inputs WHERE input_index=0");
    CHECK(throws([&] { Ledger broken(path, "deployment-v1"); }));
    execute_sql(temp.file("unrelated.sqlite"), "CREATE TABLE unrelated(value TEXT)");
    CHECK(throws([&] { Ledger unrelated(temp.file("unrelated.sqlite"), "deployment-v1"); }));
    {
        Ledger ordered(temp.file("event-order.sqlite"), "ordered");
        ordered.commit_input(0, "{}", 1, {{"one", "{}"}, {"two", "{}"}});
    }
    execute_sql(temp.file("event-order.sqlite"),
        "UPDATE events SET input_position=2 WHERE event_id='one';"
        "UPDATE events SET input_position=0 WHERE event_id='two';"
        "UPDATE events SET input_position=1 WHERE event_id='one';");
    CHECK(throws([&] { Ledger reordered(temp.file("event-order.sqlite"), "ordered"); }));
}

[[noreturn]] void crash_writer(const char* path) {
    try {
        Ledger ledger(path, "crash-v1");
        ledger.commit_input(0, "{\"synthetic\":true}", 42, {{"crash-event", "{\"qty\":1}"}});
        ledger.begin_delivery("crash-event");
        _exit(0); // Deliberately skip destructors/checkpoint/acknowledgement.
    } catch (const std::exception& e) {
        std::fprintf(stderr, "crash writer: %s\n", e.what());
        _exit(1);
    } catch (...) { _exit(1); }
}

void crash_recovery(const char* executable) {
    TempDir temp;
    const auto path = temp.file("crash.sqlite");
    const pid_t child = fork();
    if (child < 0) throw std::runtime_error("test fork failed");
    if (child == 0) {
        // SQLite's platform logging state is not safe to reuse after fork.
        // Start a fresh writer process, then exercise the same abrupt exit.
        execl(executable, executable, "--crash-writer", path.c_str(),
              static_cast<char*>(nullptr));
        _exit(127);
    }
    int status = 0;
    CHECK(waitpid(child, &status, 0) == child);
    CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);
    Ledger recovered(path, "crash-v1");
    CHECK(recovered.input_count() == 1);
    const auto pending = recovered.pending_event();
    CHECK(pending.has_value());
    if (pending) {
        CHECK(pending->id == "crash-event");
        CHECK(pending->attempts == 1);
        CHECK(pending->payload == "{\"qty\":1}");
    }
}

struct Reply {
    int status = 200;
    std::string body;
    std::string extra_headers;
    int delay_ms = 0;
};

class Receiver {
public:
    std::vector<std::string> requests;
    explicit Receiver(std::vector<Reply> replies) : replies_(std::move(replies)) {
        listener_ = socket(AF_INET, SOCK_STREAM, 0);
        if (listener_ < 0) throw std::runtime_error("test socket failed");
        sockaddr_in address {};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        if (bind(listener_, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0 ||
            listen(listener_, 8) != 0) throw std::runtime_error("test bind failed");
        socklen_t length = sizeof(address);
        if (getsockname(listener_, reinterpret_cast<sockaddr*>(&address), &length) != 0)
            throw std::runtime_error("test getsockname failed");
        port_ = ntohs(address.sin_port);
        thread_ = std::thread([this] { serve(); });
    }
    ~Receiver() {
        shutdown(listener_, SHUT_RDWR);
        if (thread_.joinable()) thread_.join();
        close(listener_);
    }
    std::string url() const { return "http://127.0.0.1:" + std::to_string(port_) + "/synthetic"; }
    void finish() { if (thread_.joinable()) thread_.join(); CHECK(error_.empty()); }
private:
    int listener_ = -1;
    unsigned short port_ = 0;
    std::vector<Reply> replies_;
    std::thread thread_;
    std::string error_;

    static std::string read_request(int client) {
        std::string request;
        std::size_t expected = std::string::npos;
        while (expected == std::string::npos || request.size() < expected) {
            pollfd wait {client, POLLIN, 0};
            if (poll(&wait, 1, 2000) <= 0) throw std::runtime_error("test receive timeout");
            char buffer[4096];
            const auto count = recv(client, buffer, sizeof(buffer), 0);
            if (count <= 0) throw std::runtime_error("test receive failed");
            request.append(buffer, static_cast<std::size_t>(count));
            const auto end = request.find("\r\n\r\n");
            if (end == std::string::npos) continue;
            std::string headers = request.substr(0, end);
            std::transform(headers.begin(), headers.end(), headers.begin(), [](unsigned char c) {
                return static_cast<char>(std::tolower(c));
            });
            const auto position = headers.find("\r\ncontent-length:");
            std::size_t length = 0;
            if (position != std::string::npos) length = std::stoull(headers.substr(position + 17));
            expected = end + 4 + length;
        }
        return request;
    }
    void serve() noexcept {
        try {
            for (const auto& reply : replies_) {
                pollfd wait {listener_, POLLIN, 0};
                if (poll(&wait, 1, 5000) <= 0) throw std::runtime_error("test accept timeout");
                const int client = accept(listener_, nullptr, nullptr);
                if (client < 0) throw std::runtime_error("test accept failed");
#ifdef SO_NOSIGPIPE
                int enabled = 1;
                setsockopt(client, SOL_SOCKET, SO_NOSIGPIPE, &enabled, sizeof(enabled));
#endif
                requests.push_back(read_request(client));
                if (reply.delay_ms) std::this_thread::sleep_for(std::chrono::milliseconds(reply.delay_ms));
                const auto data = "HTTP/1.1 " + std::to_string(reply.status) + " Test\r\nContent-Length: " +
                    std::to_string(reply.body.size()) + "\r\nConnection: close\r\n" + reply.extra_headers +
                    "\r\n" + reply.body;
                std::size_t sent = 0;
                while (sent < data.size()) {
                    const auto count = send(client, data.data() + sent, data.size() - sent,
#ifdef MSG_NOSIGNAL
                                            MSG_NOSIGNAL
#else
                                            0
#endif
                    );
                    if (count <= 0) break; // Expected when a bounded client aborts.
                    sent += static_cast<std::size_t>(count);
                }
                close(client);
            }
        } catch (const std::exception& e) { error_ = e.what(); }
    }
};

void native_http() {
    CHECK(sha256_hex("abc") == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    CHECK(hmac_sha256_hex("key", "The quick brown fox jumps over the lazy dog") ==
          "f7bc83f430538424b13298e6aa6fb143ef4d59a14946175997479dbc2d1a3cd8");
    StoredEvent event;
    event.id = "stable-id";
    event.payload = "{\"action\":\"buy\",\"quantity\":1}";
    Receiver server({{503, "retry", "", 0}, {202, "accepted", "", 0},
                     {302, "redirect", "Location: http://127.0.0.1:1/do-not-follow\r\n", 0},
                     {200, "{\"type\":\"time\",\"ts\":60000}\n", "", 0}});
    HttpOptions options;
    options.url = server.url();
    options.hmac_secret = "runtime-secret";
    CHECK(throws([&] { post_webhook(options, event); }));
    options.allow_insecure_http = true;
    const auto first = post_webhook(options, event);
    CHECK(!first.success && first.status == 503 && first.error == "http_status");
    const auto second = post_webhook(options, event);
    CHECK(second.success && second.status == 202 && second.error.empty());
    const auto redirect = post_webhook(options, event);
    CHECK(!redirect.success && redirect.status == 302);
    CHECK(get_feed_snapshot(options) == "{\"type\":\"time\",\"ts\":60000}\n");
    server.finish();
    CHECK(server.requests.size() == 4);
    CHECK(server.requests[0] == server.requests[1]);
    CHECK(server.requests[0].find("Idempotency-Key: stable-id\r\n") != std::string::npos);
    CHECK(server.requests[0].find("X-PineForge-Event-Id: stable-id\r\n") != std::string::npos);
    CHECK(server.requests[0].find("X-PineForge-Signature: sha256=" +
          hmac_sha256_hex("runtime-secret", event.payload) + "\r\n") != std::string::npos);
    CHECK(server.requests[0].substr(server.requests[0].find("\r\n\r\n") + 4) == event.payload);
    CHECK(server.requests[3].find("GET /synthetic HTTP/") == 0);
    CHECK(server.requests[3].find("X-PineForge-") == std::string::npos);
    CHECK(server.requests[3].find("Idempotency-Key") == std::string::npos);
    auto invalid = options;
    invalid.url = "file:///etc/passwd";
    CHECK(throws([&] { get_feed_snapshot(invalid); }));
    invalid.url = "http://user:password@127.0.0.1:1/";
    CHECK(throws([&] { get_feed_snapshot(invalid); }));
    invalid.url = options.url;
    invalid.total_timeout_ms = 0;
    CHECK(throws([&] { post_webhook(invalid, event); }));
    auto injected = event;
    injected.id = "id\r\nInjected: value";
    CHECK(throws([&] { post_webhook(options, injected); }));
    Receiver huge({{200, std::string(4 * 1024 * 1024 + 1, 'x'), "", 0}});
    options.url = huge.url();
    CHECK(throws([&] { get_feed_snapshot(options); }));
    huge.finish();
    Receiver slow({{200, "", "", 120}});
    options.url = slow.url();
    options.connect_timeout_ms = 20;
    options.total_timeout_ms = 30;
    const auto timeout = post_webhook(options, event);
    CHECK(!timeout.success && timeout.error == "timeout");
    slow.finish();
}

} // namespace

int main(int argc, char** argv) {
    if (argc == 3 && std::strcmp(argv[1], "--crash-writer") == 0)
        crash_writer(argv[2]);
    try {
        ledger_transactions();
        crash_recovery(argv[0]);
        native_http();
    } catch (const std::exception& e) {
        std::fprintf(stderr, "UNEXPECTED: %s\n", e.what());
        ++failures;
    }
    std::printf("native live store/transport: %d failure(s)\n", failures);
    return failures ? 1 : 0;
}
