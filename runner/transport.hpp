// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "store.hpp"

namespace pineforge::live {

struct HttpOptions {
    std::string url;
    std::string hmac_secret; // Runtime only; never passed to Ledger.
    long connect_timeout_ms = 5000;
    long total_timeout_ms = 15000;
    long idle_timeout_ms = 0;
    long message_timeout_ms = 0;
    bool allow_insecure_http = false;
};

struct DeliveryResult {
    long status = 0;
    bool success = false;
    bool retryable = false;
    // A fixed category, never a URL, response body, header or libcurl error buffer.
    std::string error;
};

std::string sha256_hex(std::string_view bytes);
std::string hmac_sha256_hex(std::string_view secret, std::string_view bytes);
void validate_http(const HttpOptions& options);
void capture_proxy_environment();

struct CompletedWebhook {
    std::uint64_t key = 0;
    DeliveryResult result;
};

class WebhookMulti {
public:
    WebhookMulti();
    ~WebhookMulti();
    WebhookMulti(const WebhookMulti&) = delete;
    WebhookMulti& operator=(const WebhookMulti&) = delete;
    void add(std::uint64_t key, const HttpOptions& options, const StoredEvent& event);
    std::vector<CompletedWebhook> poll(int timeout_ms);
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
// Blocking, bounded native libcurl delivery. Only HTTP(S), no redirects,
// verified TLS, JSON body, stable idempotency and optional HMAC headers.
#ifdef PINEFORGE_LIVE_LEGACY_TEST_API
DeliveryResult post_webhook(const HttpOptions& options, const StoredEvent& event);
#endif
// Fetch a finite provider-neutral JSONL snapshot. Maximum response 4 MiB;
// requires HTTP 2xx and never attaches webhook HMAC/idempotency headers.
std::string get_feed_snapshot(const HttpOptions& options,
                              const std::function<bool()>& stopped = {});
void validate_websocket(const HttpOptions& options);
// Native WS/WSS intake. Complete UTF-8 text messages only, at most 1 MiB.
// Separate idle/message deadlines default to total_timeout_ms. A closure
// or transport failure throws; reconnection/continuity is never inferred.
// Returns cleanly only when stopped() or on_message() requests a stop.
void receive_websocket(const HttpOptions& options, std::string_view subscription,
                       const std::function<bool(std::string_view)>& on_message,
                       const std::function<bool()>& stopped,
                       const std::function<void(std::size_t)>& buffered = {});

} // namespace pineforge::live
