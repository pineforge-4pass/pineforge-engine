#pragma once

#include "json.hpp"
#include "transport.hpp"

#include <map>
#include <optional>
#include <string>
#include <vector>

namespace pineforge::live {

struct DeliveryOptions {
    std::size_t max_in_flight = 8;
    long connect_timeout_ms = 2000;
    long total_timeout_ms = 5000;
    unsigned transport_retries = 2;
    std::vector<long> retry_backoff_ms{1000, 2000};
};

struct WebhookTarget {
    std::string url;
    std::string secret_env;
};

struct RoutingRule {
    std::optional<std::string> order_id, kind, side, target;
};

struct RoutingConfig {
    bool routed = false;
    bool allow_insecure_http = false;
    std::optional<std::string> default_target;
    std::map<std::string, WebhookTarget> targets;
    std::vector<RoutingRule> rules;
    DeliveryOptions delivery;
    Json document;
    std::string file_identity;

    std::optional<std::string> select(const std::string& order_id,
                                    const std::string& kind, const std::string& side) const;
    std::map<std::string, HttpOptions> load_secrets() const;
    std::string stored_document() const;
};

RoutingConfig parse_routes(const std::string& text, bool allow_http,
                           const std::string& default_url = "",
                           const std::string& default_secret_env = "");
RoutingConfig single_target(const std::string& url, const std::string& secret_env, bool allow_http);
RoutingConfig restore_routes(const std::string& document);
std::string delivery_identity(const std::string& event_id,
                              const std::optional<std::string>& target);

} 
