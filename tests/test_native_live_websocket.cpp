// SPDX-License-Identifier: Apache-2.0
#include "../runner/transport.hpp"

#include <curl/curl.h>
#include <openssl/evp.h>

#include <array>
#include <chrono>
#include <cstdio>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
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

bool supports_ws() {
    const auto* info = curl_version_info(CURLVERSION_NOW);
    for (const char* const* p = info ? info->protocols : nullptr; p && *p; ++p)
        if (std::string_view(*p) == "ws") return true;
    return false;
}

std::string frame(unsigned char opcode, const std::string& payload, bool final = true) {
    std::string result(1, static_cast<char>(opcode | (final ? 0x80 : 0)));
    if (payload.size() < 126) result.push_back(static_cast<char>(payload.size()));
    else if (payload.size() < 65536) {
        result.push_back(126);
        result.push_back(static_cast<char>(payload.size() >> 8));
        result.push_back(static_cast<char>(payload.size()));
    } else {
        result.push_back(127);
        for (int shift = 56; shift >= 0; shift -= 8)
            result.push_back(static_cast<char>(static_cast<std::uint64_t>(payload.size()) >> shift));
    }
    return result + payload;
}

void send_all(int socket, std::string_view bytes) {
    std::size_t sent = 0;
    while (sent < bytes.size()) {
        const auto n = send(socket, bytes.data() + sent, bytes.size() - sent,
#ifdef MSG_NOSIGNAL
                            MSG_NOSIGNAL
#else
                            0
#endif
        );
        if (n <= 0) return; // Some tests deliberately abort the client.
        sent += static_cast<std::size_t>(n);
    }
}

std::string read_exact(int socket, std::size_t length) {
    std::string result(length, '\0');
    std::size_t received = 0;
    while (received < length) {
        pollfd waiting {socket, POLLIN, 0};
        if (poll(&waiting, 1, 3000) <= 0) throw std::runtime_error("WS test receive timeout");
        const auto n = recv(socket, result.data() + received, length - received, 0);
        if (n <= 0) throw std::runtime_error("WS test receive failed");
        received += static_cast<std::size_t>(n);
    }
    return result;
}

std::pair<unsigned char, std::string> read_client_frame(int socket) {
    const auto first = read_exact(socket, 2);
    const auto opcode = static_cast<unsigned char>(first[0]);
    const auto second = static_cast<unsigned char>(first[1]);
    if (!(second & 0x80)) throw std::runtime_error("WS client frame is not masked");
    std::uint64_t length = second & 0x7f;
    if (length == 126 || length == 127) {
        const auto extension = read_exact(socket, length == 126 ? 2 : 8);
        length = 0;
        for (unsigned char byte : extension) length = (length << 8) | byte;
    }
    if (length > 1024 * 1024) throw std::runtime_error("WS test frame too large");
    const auto mask = read_exact(socket, 4);
    auto body = read_exact(socket, static_cast<std::size_t>(length));
    for (std::size_t i = 0; i < body.size(); ++i) body[i] ^= mask[i % 4];
    return {opcode, std::move(body)};
}

class WebSocketServer {
public:
    std::string request;
    std::pair<unsigned char, std::string> subscription;
    std::pair<unsigned char, std::string> pong;

    WebSocketServer(std::vector<std::string> frames, bool read_subscription = false,
                    bool read_pong = false, int delay_ms = 0, int hold_ms = 0)
        : frames_(std::move(frames)), read_subscription_(read_subscription),
          read_pong_(read_pong), delay_ms_(delay_ms), hold_ms_(hold_ms) {
        listener_ = socket(AF_INET, SOCK_STREAM, 0);
        if (listener_ < 0) throw std::runtime_error("WS test socket failed");
        sockaddr_in address {};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        if (bind(listener_, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0 ||
            listen(listener_, 8) != 0) throw std::runtime_error("WS test bind failed");
        socklen_t size = sizeof(address);
        if (getsockname(listener_, reinterpret_cast<sockaddr*>(&address), &size) != 0)
            throw std::runtime_error("WS test getsockname failed");
        port_ = ntohs(address.sin_port);
        thread_ = std::thread([this] { serve(); });
    }
    ~WebSocketServer() {
        shutdown(listener_, SHUT_RDWR);
        if (thread_.joinable()) thread_.join();
        close(listener_);
    }
    std::string url() const { return "ws://127.0.0.1:" + std::to_string(port_) + "/feed"; }
    void finish() { if (thread_.joinable()) thread_.join(); CHECK(error_.empty()); }
private:
    int listener_ = -1;
    unsigned short port_ = 0;
    std::thread thread_;
    std::vector<std::string> frames_;
    bool read_subscription_ = false;
    bool read_pong_ = false;
    int delay_ms_ = 0;
    int hold_ms_ = 0;
    std::string error_;

    void serve() noexcept {
        int client = -1;
        try {
            pollfd waiting {listener_, POLLIN, 0};
            if (poll(&waiting, 1, 3000) <= 0) throw std::runtime_error("WS test accept timeout");
            client = accept(listener_, nullptr, nullptr);
            if (client < 0) throw std::runtime_error("WS test accept failed");
#ifdef SO_NOSIGPIPE
            int enabled = 1;
            setsockopt(client, SOL_SOCKET, SO_NOSIGPIPE, &enabled, sizeof(enabled));
#endif
            while (request.find("\r\n\r\n") == std::string::npos) {
                request += read_exact(client, 1);
                if (request.size() > 65536) throw std::runtime_error("WS test headers too large");
            }
            const std::string field = "Sec-WebSocket-Key: ";
            const auto begin = request.find(field);
            if (begin == std::string::npos) throw std::runtime_error("WS test key missing");
            const auto end = request.find("\r\n", begin);
            const auto seed = request.substr(begin + field.size(), end - begin - field.size()) +
                              "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
            std::array<unsigned char, EVP_MAX_MD_SIZE> digest {};
            unsigned length = 0;
            if (EVP_Digest(seed.data(), seed.size(), digest.data(), &length, EVP_sha1(), nullptr) != 1)
                throw std::runtime_error("WS test SHA1 failed");
            std::array<unsigned char, 64> encoded {};
            const int encoded_size = EVP_EncodeBlock(encoded.data(), digest.data(), static_cast<int>(length));
            const std::string accept(reinterpret_cast<char*>(encoded.data()), static_cast<std::size_t>(encoded_size));
            send_all(client, "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
                             "Sec-WebSocket-Accept: " + accept + "\r\n\r\n");
            if (read_subscription_) subscription = read_client_frame(client);
            if (delay_ms_) std::this_thread::sleep_for(std::chrono::milliseconds(delay_ms_));
            for (const auto& bytes : frames_) send_all(client, bytes);
            if (read_pong_) pong = read_client_frame(client);
            if (hold_ms_) std::this_thread::sleep_for(std::chrono::milliseconds(hold_ms_));
        } catch (const std::exception& e) { error_ = e.what(); }
        if (client >= 0) close(client);
    }
};

void websocket_tests() {
    HttpOptions options;
    options.allow_insecure_http = true;
    options.connect_timeout_ms = 500;
    options.total_timeout_ms = 1000;
    options.hmac_secret = "never-a-feed-header";
    std::vector<std::string> received;
    auto capture = [&](std::string_view bytes) { received.emplace_back(bytes); return false; };
    auto running = [] { return false; };
    WebSocketServer fragmented({frame(1, "{\"x\":", false), frame(9, "alive"), frame(0, "1}")}, true, true);
    options.url = fragmented.url();
    const std::string subscription = "{\"subscribe\":\"ETH\"}";
    receive_websocket(options, subscription, capture, running);
    fragmented.finish();
    CHECK(received == std::vector<std::string>{"{\"x\":1}"});
    CHECK(fragmented.subscription.first == 0x81 && fragmented.subscription.second == subscription);
    CHECK(fragmented.pong.first == 0x8a && fragmented.pong.second == "alive");
    CHECK(fragmented.request.find("X-PineForge-") == std::string::npos);
    CHECK(fragmented.request.find("Idempotency-Key") == std::string::npos);
    CHECK(fragmented.request.find("never-a-feed-header") == std::string::npos);

    const std::string large(1024 * 1024, 'x');
    WebSocketServer large_message({frame(1, large)}, true);
    options.url = large_message.url();
    received.clear();
    receive_websocket(options, large, capture, running);
    large_message.finish();
    CHECK(received.size() == 1 && received[0] == large);
    CHECK(large_message.subscription.first == 0x81 && large_message.subscription.second == large);

    WebSocketServer empty_final({frame(1, large, false), frame(9, "alive"), frame(0, "")});
    options.url = empty_final.url();
    received.clear();
    receive_websocket(options, "", capture, running);
    empty_final.finish();
    CHECK(received == std::vector<std::string>{large});

    for (const auto& bad : {frame(2, "binary"), frame(1, std::string("\xc0\x80", 2)),
                            frame(1, large + "x"), frame(1, "unfinished", false),
                            std::string("\x81\x0a", 2) + "abc", frame(8, "")}) {
        WebSocketServer server({bad});
        options.url = server.url();
        received.clear();
        CHECK(throws([&] { receive_websocket(options, "", capture, running); }));
        CHECK(received.empty());
        server.finish();
    }
    WebSocketServer unfinished_ping({frame(1, "unfinished", false), frame(9, "alive"),
                                    frame(0, "still unfinished", false), frame(9, "again")},
                                   false, false, 0, 100);
    options.url = unfinished_ping.url();
    options.connect_timeout_ms = 20;
    options.total_timeout_ms = 30;
    received.clear();
    bool timeout_error = false;
    try { receive_websocket(options, "", capture, running); }
    catch (const std::exception& error) {
        timeout_error = std::string(error.what()).find("timeout") != std::string::npos;
    }
    CHECK(timeout_error);
    CHECK(received.empty());
    unfinished_ping.finish();
    WebSocketServer idle({}, false, false, 100);
    options.url = idle.url();
    options.connect_timeout_ms = 20;
    options.total_timeout_ms = 30;
    CHECK(throws([&] { receive_websocket(options, "", capture, running); }));
    idle.finish();
    WebSocketServer stop({}, false, false, 200);
    options.url = stop.url();
    options.total_timeout_ms = 1000;
    const auto start = std::chrono::steady_clock::now();
    receive_websocket(options, "", capture, [&] {
        return std::chrono::steady_clock::now() - start > std::chrono::milliseconds(50);
    });
    stop.finish();
}

} // namespace

int main() {
    try {
        const auto* info = curl_version_info(CURLVERSION_NOW);
        if (!info || info->version_num < 0x080e01) {
            for (const auto* url : {"ws://127.0.0.1:1/feed", "wss://127.0.0.1:1/feed"}) {
                HttpOptions options;
                options.url = url;
                options.allow_insecure_http = true;
                bool clear_error = false;
                try { validate_websocket(options); }
                catch (const std::exception& error) {
                    clear_error = std::string(error.what()).find("libcurl 8.14.1 or newer") != std::string::npos;
                }
                CHECK(clear_error);
                bool called = false;
                CHECK(throws([&] {
                    receive_websocket(options, "", [&](std::string_view) { called = true; return false; },
                                      [] { return false; });
                }));
                CHECK(!called);
            }
            std::puts("native WebSocket: unqualified-runtime refusal verified; no message callback");
            return failures ? 1 : 0;
        }
        if (!supports_ws()) {
            HttpOptions options;
            options.url = "wss://127.0.0.1:1/feed";
            bool clear_error = false;
            try { receive_websocket(options, "", [](std::string_view) { return true; }, [] { return false; }); }
            catch (const std::exception& e) {
                clear_error = std::string(e.what()).find("WS/WSS support enabled") != std::string::npos;
            }
            CHECK(clear_error);
            std::puts("native WebSocket: unsupported-build refusal verified; loopback tests skipped (no WS in libcurl)");
            return failures ? 1 : 77;
        }
        websocket_tests();
    } catch (const std::exception& e) {
        std::fprintf(stderr, "UNEXPECTED: %s\n", e.what());
        ++failures;
    }
    std::printf("native WebSocket: %d failure(s)\n", failures);
    return failures ? 1 : 0;
}
