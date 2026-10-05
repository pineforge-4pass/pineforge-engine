// SPDX-License-Identifier: Apache-2.0
#include "transport.hpp"

#include <curl/curl.h>
#include <openssl/evp.h>
#include <openssl/hmac.h>

#include <array>
#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <exception>
#include <limits>
#include <map>
#include <memory>
#include <stdexcept>
#include <utility>

#include <poll.h>

namespace pineforge::live {
namespace {

constexpr std::size_t max_feed_bytes = 4 * 1024 * 1024;
constexpr std::size_t max_event_bytes = 1024 * 1024;
std::string http_proxy, https_proxy, all_proxy, no_proxy;

std::string hex(const unsigned char* data, std::size_t length) {
    constexpr char digits[] = "0123456789abcdef";
    std::string result(length * 2, '0');
    for (std::size_t i = 0; i < length; ++i) {
        result[2 * i] = digits[data[i] >> 4];
        result[2 * i + 1] = digits[data[i] & 15];
    }
    return result;
}

struct CurlGlobal {
    CurlGlobal() {
        if (curl_global_init(CURL_GLOBAL_DEFAULT) != CURLE_OK)
            throw std::runtime_error("native HTTP initialization failed");
    }
    ~CurlGlobal() { curl_global_cleanup(); }
};

void initialize_curl() {
    static CurlGlobal global;
    (void)global;
}

using CurlHandle = std::unique_ptr<CURL, decltype(&curl_easy_cleanup)>;
using UrlHandle = std::unique_ptr<CURLU, decltype(&curl_url_cleanup)>;

template<class T>
void option(CURL* curl, CURLoption name, T value) {
    if (curl_easy_setopt(curl, name, value) != CURLE_OK)
        throw std::runtime_error("native HTTP option setup failed");
}

struct Headers {
    curl_slist* value = nullptr;
    ~Headers() { curl_slist_free_all(value); }
    void add(const std::string& line) {
        auto* next = curl_slist_append(value, line.c_str());
        if (!next) throw std::runtime_error("native HTTP header allocation failed");
        value = next;
    }
};

void check_url(const HttpOptions& options, bool websocket) {
    if (options.url.empty() || options.url.size() > 64 * 1024 ||
        options.url.find_first_of("\r\n") != std::string::npos ||
        options.url.find('\0') != std::string::npos)
        throw std::runtime_error("native HTTP URL is invalid");
    UrlHandle parsed(curl_url(), &curl_url_cleanup);
    if (!parsed || curl_url_set(parsed.get(), CURLUPART_URL, options.url.c_str(), CURLU_NON_SUPPORT_SCHEME) != CURLUE_OK)
        throw std::runtime_error("native HTTP URL is invalid");
    char* part = nullptr;
    if (curl_url_get(parsed.get(), CURLUPART_SCHEME, &part, 0) != CURLUE_OK)
        throw std::runtime_error("native HTTP URL has no scheme");
    const std::string scheme(part);
    curl_free(part);
    const std::string secure = websocket ? "wss" : "https";
    const std::string insecure = websocket ? "ws" : "http";
    if (scheme != secure && !(scheme == insecure && options.allow_insecure_http))
        throw std::runtime_error("native transport requires HTTPS/WSS unless insecure transport is explicitly enabled");
    for (const auto field : {CURLUPART_USER, CURLUPART_PASSWORD, CURLUPART_FRAGMENT}) {
        part = nullptr;
        if (curl_url_get(parsed.get(), field, &part, 0) == CURLUE_OK) {
            curl_free(part);
            throw std::runtime_error("native HTTP URL must not contain user information or a fragment");
        }
    }
    if (options.connect_timeout_ms <= 0 || options.total_timeout_ms <= 0 ||
        options.connect_timeout_ms > options.total_timeout_ms ||
        options.total_timeout_ms > 300000)
        throw std::runtime_error("native HTTP timeouts must be positive, ordered and at most 300 seconds");
    if (options.idle_timeout_ms < 0 || options.idle_timeout_ms > 300000 ||
        options.message_timeout_ms < 0 || options.message_timeout_ms > 300000)
        throw std::runtime_error("feed timeouts must be at most 300 seconds");
}

CurlHandle make_handle(const HttpOptions& options, bool websocket = false) {
    initialize_curl();
    if (websocket) validate_websocket(options);
    else check_url(options, false);
    CurlHandle curl(curl_easy_init(), &curl_easy_cleanup);
    if (!curl) throw std::runtime_error("native HTTP handle allocation failed");
    option(curl.get(), CURLOPT_URL, options.url.c_str());
    const auto& proxy = (options.url.rfind("https:", 0) == 0 || options.url.rfind("wss:", 0) == 0)
        ? https_proxy : http_proxy;
    option(curl.get(), CURLOPT_PROXY, (proxy.empty() ? all_proxy : proxy).c_str());
    option(curl.get(), CURLOPT_NOPROXY, no_proxy.c_str());
    option(curl.get(), CURLOPT_CONNECTTIMEOUT_MS, options.connect_timeout_ms);
    option(curl.get(), CURLOPT_TIMEOUT_MS, options.total_timeout_ms);
    option(curl.get(), CURLOPT_NOSIGNAL, 1L);
    option(curl.get(), CURLOPT_FOLLOWLOCATION, 0L);
    option(curl.get(), CURLOPT_MAXREDIRS, 0L);
    option(curl.get(), CURLOPT_SSL_VERIFYPEER, 1L);
    option(curl.get(), CURLOPT_SSL_VERIFYHOST, 2L);
    option(curl.get(), CURLOPT_NETRC, static_cast<long>(CURL_NETRC_IGNORED));
    option(curl.get(), CURLOPT_USERAGENT, "pineforge-native-live/1");
#if LIBCURL_VERSION_NUM >= 0x075500
    option(curl.get(), CURLOPT_PROTOCOLS_STR, websocket ? (options.allow_insecure_http ? "wss,ws" : "wss") :
                                                        (options.allow_insecure_http ? "https,http" : "https"));
    option(curl.get(), CURLOPT_REDIR_PROTOCOLS_STR, websocket ? "wss" : "https");
#else
    option(curl.get(), CURLOPT_PROTOCOLS,
           static_cast<long>(options.allow_insecure_http ? CURLPROTO_HTTPS | CURLPROTO_HTTP : CURLPROTO_HTTPS));
    option(curl.get(), CURLOPT_REDIR_PROTOCOLS, static_cast<long>(CURLPROTO_HTTPS));
#endif
    return curl;
}

struct Response {
    std::string body;
    std::size_t received = 0;
    bool retain = false;
    bool too_large = false;
    bool allocation_failed = false;
};

struct FeedProgress {
    const std::function<bool()>& stopped;
    long idle_timeout_ms;
    curl_off_t received = 0;
    std::chrono::steady_clock::time_point last_data = std::chrono::steady_clock::now();
    bool cancelled = false;
    bool timed_out = false;
    std::exception_ptr error;
    FeedProgress(const std::function<bool()>& stop, long idle) : stopped(stop), idle_timeout_ms(idle) {}
};

int feed_progress(void* userdata, curl_off_t, curl_off_t received, curl_off_t, curl_off_t) noexcept {
    auto& progress = *static_cast<FeedProgress*>(userdata);
    try {
        if (progress.stopped && progress.stopped()) { progress.cancelled = true; return 1; }
        const auto now = std::chrono::steady_clock::now();
        if (received != progress.received) { progress.received = received; progress.last_data = now; }
        if (now - progress.last_data >= std::chrono::milliseconds(progress.idle_timeout_ms)) {
            progress.timed_out = true;
            return 1;
        }
    } catch (...) { progress.error = std::current_exception(); return 1; }
    return 0;
}

void attach_progress(CURL* curl, FeedProgress& progress) {
    option(curl, CURLOPT_NOPROGRESS, 0L);
    option(curl, CURLOPT_XFERINFOFUNCTION, &feed_progress);
    option(curl, CURLOPT_XFERINFODATA, &progress);
}

void check_progress(const FeedProgress& progress) {
    if (progress.error) std::rethrow_exception(progress.error);
    if (progress.timed_out) throw std::runtime_error("native feed idle timeout");
}

std::size_t receive(char* data, std::size_t size, std::size_t nmemb, void* userdata) noexcept {
    auto& response = *static_cast<Response*>(userdata);
    if (size != 0 && nmemb > std::numeric_limits<std::size_t>::max() / size) return 0;
    const auto count = size * nmemb;
    if (count > max_feed_bytes - response.received) {
        response.too_large = true;
        return 0;
    }
    response.received += count;
    if (response.retain) {
        try { response.body.append(data, count); }
        catch (...) { response.allocation_failed = true; return 0; }
    }
    return count;
}

DeliveryResult response_result(CURL* curl, const Response& response, CURLcode code) {
    DeliveryResult result;
    if (curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &result.status) != CURLE_OK)
        throw std::runtime_error("native HTTP response status unavailable");
    if (result.status >= 300) {
        result.error = "http_status";
        return result;
    }
    if (code != CURLE_OK) {
        if (response.too_large) result.error = "response_too_large";
        else if (response.allocation_failed) result.error = "response_allocation_failed";
        else if (code == CURLE_OPERATION_TIMEDOUT) result.error = "timeout";
        else if (code == CURLE_PEER_FAILED_VERIFICATION || code == CURLE_SSL_CONNECT_ERROR ||
                 code == CURLE_SSL_CERTPROBLEM || code == CURLE_SSL_CACERT_BADFILE)
            result.error = "tls_failure";
        else result.error = "network_error";
        result.retryable = result.status == 0 && !response.too_large && !response.allocation_failed &&
            (code == CURLE_COULDNT_CONNECT || code == CURLE_COULDNT_RESOLVE_HOST ||
             code == CURLE_COULDNT_RESOLVE_PROXY || code == CURLE_OPERATION_TIMEDOUT ||
             code == CURLE_SEND_ERROR || code == CURLE_RECV_ERROR || code == CURLE_GOT_NOTHING);
        return result;
    }
    result.success = result.status >= 200 && result.status <= 299;
    if (!result.success) result.error = "http_status";
    return result;
}

DeliveryResult perform(CURL* curl, Response& response) {
    option(curl, CURLOPT_WRITEFUNCTION, &receive);
    option(curl, CURLOPT_WRITEDATA, &response);
    return response_result(curl, response, curl_easy_perform(curl));
}

bool valid_utf8(std::string_view bytes) {
    std::uint32_t value = 0, minimum = 0;
    unsigned remaining = 0;
    for (const unsigned char byte : bytes) {
        if (remaining) {
            if ((byte & 0xc0) != 0x80) return false;
            value = (value << 6) | (byte & 0x3f);
            if (--remaining == 0 && (value < minimum || value > 0x10ffff ||
                                    (value >= 0xd800 && value <= 0xdfff))) return false;
        } else if (byte <= 0x7f) continue;
        else if (byte >= 0xc2 && byte <= 0xdf) { value = byte & 0x1f; minimum = 0x80; remaining = 1; }
        else if (byte >= 0xe0 && byte <= 0xef) { value = byte & 0x0f; minimum = 0x800; remaining = 2; }
        else if (byte >= 0xf0 && byte <= 0xf4) { value = byte & 0x07; minimum = 0x10000; remaining = 3; }
        else return false;
    }
    return remaining == 0;
}

#if LIBCURL_VERSION_NUM >= 0x075600
using Clock = std::chrono::steady_clock;

bool wait_socket(CURL* curl, short events, Clock::time_point deadline,
                 const std::function<bool()>& stopped) {
    curl_socket_t socket = CURL_SOCKET_BAD;
    if (curl_easy_getinfo(curl, CURLINFO_ACTIVESOCKET, &socket) != CURLE_OK || socket == CURL_SOCKET_BAD)
        throw std::runtime_error("native WebSocket socket unavailable");
    while (!stopped()) {
        const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now()).count();
        if (left <= 0) throw std::runtime_error("native WebSocket idle or message timeout");
        pollfd fd {socket, events, 0};
        const int rc = poll(&fd, 1, static_cast<int>(std::min<std::int64_t>(left, 100)));
        if (rc < 0 && errno == EINTR) continue;
        if (rc < 0) throw std::runtime_error("native WebSocket socket wait failed");
        if (rc > 0) {
            if (fd.revents & events) return true;
            if (fd.revents & (POLLHUP | POLLERR | POLLNVAL))
                throw std::runtime_error("native WebSocket connection closed");
        }
    }
    return false;
}

bool send_subscription(CURL* curl, std::string_view bytes, long timeout_ms,
                       const std::function<bool()>& stopped) {
    std::size_t offset = 0;
    bool begun = false;
    const auto deadline = Clock::now() + std::chrono::milliseconds(timeout_ms);
    while (offset < bytes.size() && !stopped()) {
        std::size_t sent = 0;
        const auto code = curl_ws_send(curl, bytes.data() + offset, bytes.size() - offset,
            &sent, begun ? 0 : static_cast<curl_off_t>(bytes.size()), CURLWS_TEXT | CURLWS_OFFSET);
        begun = true;
        if (code != CURLE_OK && code != CURLE_AGAIN)
            throw std::runtime_error("native WebSocket subscription send failed");
        if (sent > bytes.size() - offset)
            throw std::runtime_error("native WebSocket invalid subscription send count");
        offset += sent;
        if (Clock::now() >= deadline) throw std::runtime_error("native WebSocket subscription timeout");
        if (offset < bytes.size() && !wait_socket(curl, POLLOUT, deadline, stopped)) return false;
    }
    return !stopped();
}
#endif

} // namespace

std::string sha256_hex(std::string_view bytes) {
    std::array<unsigned char, EVP_MAX_MD_SIZE> digest {};
    unsigned int length = 0;
    if (EVP_Digest(bytes.data(), bytes.size(), digest.data(), &length, EVP_sha256(), nullptr) != 1 ||
        length != 32)
        throw std::runtime_error("native SHA256 computation failed");
    return hex(digest.data(), length);
}

std::string hmac_sha256_hex(std::string_view secret, std::string_view bytes) {
    if (secret.size() > static_cast<std::size_t>(std::numeric_limits<int>::max()))
        throw std::runtime_error("native HMAC secret exceeds supported size");
    std::array<unsigned char, EVP_MAX_MD_SIZE> digest {};
    unsigned int length = 0;
    if (!HMAC(EVP_sha256(), secret.data(), static_cast<int>(secret.size()),
              reinterpret_cast<const unsigned char*>(bytes.data()), bytes.size(), digest.data(), &length) ||
        length != 32)
        throw std::runtime_error("native HMAC computation failed");
    return hex(digest.data(), length);
}

#ifdef PINEFORGE_LIVE_LEGACY_TEST_API
DeliveryResult post_webhook(const HttpOptions& options, const StoredEvent& event) {
    if (event.id.empty() || event.id.size() > 256 ||
        event.id.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-_.:")
            != std::string::npos ||
        event.payload.empty() || event.payload.size() > max_event_bytes)
        throw std::runtime_error("native HTTP event id or payload is invalid");
    auto curl = make_handle(options);
    Headers headers;
    headers.add("Content-Type: application/json");
    headers.add("Idempotency-Key: " + (event.delivery_id.empty() ? event.id : event.delivery_id));
    headers.add("X-PineForge-Event-Id: " + event.id);
    headers.add("Expect:");
    if (!options.hmac_secret.empty())
        headers.add("X-PineForge-Signature: sha256=" + hmac_sha256_hex(options.hmac_secret, event.payload));
    option(curl.get(), CURLOPT_HTTPHEADER, headers.value);
    option(curl.get(), CURLOPT_POST, 1L);
    option(curl.get(), CURLOPT_POSTFIELDS, event.payload.data());
    option(curl.get(), CURLOPT_POSTFIELDSIZE_LARGE, static_cast<curl_off_t>(event.payload.size()));
    Response response;
    return perform(curl.get(), response);
}
#endif

std::string get_feed_snapshot(const HttpOptions& options, const std::function<bool()>& stopped) {
    auto curl = make_handle(options);
    FeedProgress progress{stopped, options.idle_timeout_ms ? options.idle_timeout_ms : options.total_timeout_ms};
    attach_progress(curl.get(), progress);
    Headers headers;
    headers.add("Accept: application/x-ndjson, application/jsonl, text/plain");
    option(curl.get(), CURLOPT_HTTPHEADER, headers.value);
    Response response;
    response.retain = true;
    const auto result = perform(curl.get(), response);
    check_progress(progress);
    if (progress.cancelled) return {};
    if (!result.success)
        throw std::runtime_error("native HTTP feed request failed: " + result.error);
    return std::move(response.body);
}

void validate_http(const HttpOptions& options) {
    initialize_curl();
    check_url(options, false);
}

struct WebhookMulti::Impl {
    struct Request {
        StoredEvent event;
        std::uint64_t key;
        Headers headers;
        Response response;
        CurlHandle curl;

        Request(std::uint64_t request_key, const HttpOptions& options, const StoredEvent& stored)
            : event(stored), key(request_key), curl(make_handle(options)) {
            if (event.id.empty() || event.delivery_id.empty() || event.id.size() > 256 ||
                event.delivery_id.size() > 256 ||
                event.id.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-_.:") != std::string::npos ||
                event.delivery_id.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-_.:") != std::string::npos ||
                event.payload.empty() || event.payload.size() > max_event_bytes)
                throw std::runtime_error("native HTTP event id or payload is invalid");
            headers.add("Content-Type: application/json");
            headers.add("Idempotency-Key: " + event.delivery_id);
            headers.add("X-PineForge-Event-Id: " + event.id);
            headers.add("Expect:");
            if (!options.hmac_secret.empty())
                headers.add("X-PineForge-Signature: sha256=" + hmac_sha256_hex(options.hmac_secret, event.payload));
            option(curl.get(), CURLOPT_HTTPHEADER, headers.value);
            option(curl.get(), CURLOPT_POST, 1L);
            option(curl.get(), CURLOPT_POSTFIELDS, event.payload.data());
            option(curl.get(), CURLOPT_POSTFIELDSIZE_LARGE, static_cast<curl_off_t>(event.payload.size()));
            option(curl.get(), CURLOPT_WRITEFUNCTION, &receive);
            option(curl.get(), CURLOPT_WRITEDATA, &response);
        }
    };
    CURLM* multi = nullptr;
    std::map<CURL*, std::unique_ptr<Request>> requests;
    ~Impl() {
        for (const auto& [handle, request] : requests) {
            (void)request;
            curl_multi_remove_handle(multi, handle);
        }
        requests.clear();
        if (multi) curl_multi_cleanup(multi);
    }
};

WebhookMulti::WebhookMulti() : impl_(std::make_unique<Impl>()) {
    initialize_curl();
    impl_->multi = curl_multi_init();
    if (!impl_->multi) throw std::runtime_error("native HTTP multi allocation failed");
}

WebhookMulti::~WebhookMulti() = default;

void WebhookMulti::add(std::uint64_t key, const HttpOptions& options, const StoredEvent& event) {
    auto request = std::make_unique<Impl::Request>(key, options, event);
    auto* handle = request->curl.get();
    impl_->requests.emplace(handle, std::move(request));
    if (curl_multi_add_handle(impl_->multi, handle) != CURLM_OK) {
        impl_->requests.erase(handle);
        throw std::runtime_error("native HTTP multi add failed");
    }
    int running = 0;
    if (curl_multi_perform(impl_->multi, &running) != CURLM_OK)
        throw std::runtime_error("native HTTP multi perform failed");
}

std::vector<CompletedWebhook> WebhookMulti::poll(int timeout_ms) {
    int running = 0;
    if (curl_multi_perform(impl_->multi, &running) != CURLM_OK ||
        curl_multi_poll(impl_->multi, nullptr, 0, timeout_ms, nullptr) != CURLM_OK ||
        curl_multi_perform(impl_->multi, &running) != CURLM_OK)
        throw std::runtime_error("native HTTP multi polling failed");
    std::vector<CompletedWebhook> completed;
    int remaining = 0;
    while (auto* message = curl_multi_info_read(impl_->multi, &remaining)) {
        if (message->msg != CURLMSG_DONE) continue;
        const auto found = impl_->requests.find(message->easy_handle);
        if (found == impl_->requests.end()) throw std::runtime_error("native HTTP unknown completed request");
        const auto& request = *found->second;
        completed.push_back({request.key, response_result(message->easy_handle, request.response, message->data.result)});
        if (curl_multi_remove_handle(impl_->multi, message->easy_handle) != CURLM_OK)
            throw std::runtime_error("native HTTP multi remove failed");
        impl_->requests.erase(found);
    }
    return completed;
}

void capture_proxy_environment() {
    const auto read = [](const char* lower, const char* upper) {
        const char* value = std::getenv(lower);
        if (!value && upper) value = std::getenv(upper);
        return std::string(value ? value : "");
    };
    http_proxy = read("http_proxy", nullptr);
    https_proxy = read("https_proxy", "HTTPS_PROXY");
    all_proxy = read("all_proxy", "ALL_PROXY");
    no_proxy = read("no_proxy", "NO_PROXY");
}

void validate_websocket(const HttpOptions& options) {
    initialize_curl();
    check_url(options, true);
    const auto* info = curl_version_info(CURLVERSION_NOW);
    if (!info || info->version_num < 0x080e01)
        throw std::runtime_error("native WebSocket requires libcurl 8.14.1 or newer for complete-message finality (loaded " +
                                 std::string(info && info->version ? info->version : "unknown") + ")");
    const std::string_view protocol = options.url.rfind("wss:", 0) == 0 ? "wss" : "ws";
    for (const char* const* entry = info->protocols; entry && *entry; ++entry)
        if (std::string_view(*entry) == protocol) return;
    throw std::runtime_error("native WebSocket requires a libcurl build with WS/WSS support enabled");
}

void receive_websocket(const HttpOptions& options, std::string_view subscription,
                       const std::function<bool(std::string_view)>& on_message,
                       const std::function<bool()>& stopped,
                       const std::function<void(std::size_t)>& buffered) {
    if (!on_message || !stopped || subscription.size() > max_event_bytes || !valid_utf8(subscription))
        throw std::runtime_error("native WebSocket invalid callbacks or subscription");
    if (stopped()) return;
#if LIBCURL_VERSION_NUM < 0x080e01
    (void)options;
    throw std::runtime_error("native WebSocket requires libcurl 8.14.1 or newer with WS/WSS support enabled");
#else
    auto curl = make_handle(options, true);
    option(curl.get(), CURLOPT_CONNECT_ONLY, 2L);
    option(curl.get(), CURLOPT_HTTP_VERSION, static_cast<long>(CURL_HTTP_VERSION_1_1));
    Response handshake_response;
    FeedProgress progress{stopped, options.idle_timeout_ms ? options.idle_timeout_ms : options.total_timeout_ms};
    attach_progress(curl.get(), progress);
    const auto handshake = perform(curl.get(), handshake_response);
    check_progress(progress);
    if (progress.cancelled) return;
    if (handshake.status != 101 || (!handshake.error.empty() && handshake.error != "http_status"))
        throw std::runtime_error("native WebSocket handshake failed");
    if (!send_subscription(curl.get(), subscription, options.total_timeout_ms, stopped)) return;
    const auto idle_timeout = std::chrono::milliseconds(options.idle_timeout_ms ? options.idle_timeout_ms : options.total_timeout_ms);
    const auto message_timeout = std::chrono::milliseconds(options.message_timeout_ms ? options.message_timeout_ms : options.total_timeout_ms);
    auto idle_deadline = Clock::now() + idle_timeout;
    auto message_deadline = idle_deadline;
    bool assembling = false;
    std::string message;
    std::uint64_t frame_offset = 0;
    int frame_flags = 0;
    for (;;) {
        if (stopped()) return;
        const auto deadline = assembling ? std::min(idle_deadline, message_deadline) : idle_deadline;
        if (Clock::now() >= deadline) throw std::runtime_error("native WebSocket idle or message timeout");
        std::array<char, 16384> buffer {};
        std::size_t received = 0;
        const curl_ws_frame* meta = nullptr;
        const auto code = curl_ws_recv(curl.get(), buffer.data(), buffer.size(), &received, &meta);
        if (code == CURLE_AGAIN) {
            if (!wait_socket(curl.get(), POLLIN, deadline, stopped)) return;
            continue;
        }
        if (code != CURLE_OK || !meta)
            throw std::runtime_error(assembling ? "native WebSocket truncated text message" :
                                                 "native WebSocket receive failed or connection closed");
        if (meta->flags & CURLWS_CLOSE)
            throw std::runtime_error(assembling ? "native WebSocket closed during text message" :
                                                 "native WebSocket peer closed connection");
        if (meta->flags & (CURLWS_PING | CURLWS_PONG)) {
            // libcurl handles automatic PONG responses. No control frame may
            // become a strategy input or reset an incomplete-message deadline.
            idle_deadline = Clock::now() + idle_timeout;
            continue;
        }
        if ((meta->flags & CURLWS_BINARY) || !(meta->flags & CURLWS_TEXT) ||
            meta->offset < 0 || meta->bytesleft < 0 ||
            static_cast<std::uint64_t>(meta->offset) != frame_offset || received > buffer.size() ||
            meta->len != received || (frame_offset && meta->flags != frame_flags))
            throw std::runtime_error("native WebSocket requires ordered text frames");
        frame_flags = meta->flags;
        if (!assembling) {
            message_deadline = Clock::now() + message_timeout;
            assembling = true;
        }
        if (received > max_event_bytes - message.size() ||
            static_cast<std::uint64_t>(meta->bytesleft) > max_event_bytes - message.size() - received)
            throw std::runtime_error("native WebSocket message exceeds 1 MiB");
        message.append(buffer.data(), received);
        if (buffered) buffered(message.size());
        frame_offset += received;
        idle_deadline = Clock::now() + idle_timeout;
        if (meta->bytesleft != 0) continue;
        frame_offset = 0;
        if (meta->flags & CURLWS_CONT) continue;
        if (!valid_utf8(message)) throw std::runtime_error("native WebSocket text is not valid UTF-8");
        if (!on_message(message)) return;
        message.clear();
        if (buffered) buffered(0);
        assembling = false;
        idle_deadline = Clock::now() + idle_timeout;
    }
#endif
}

} // namespace pineforge::live
