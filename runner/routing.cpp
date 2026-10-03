#include "routing.hpp"

#include <cstdlib>
#include <stdexcept>

namespace pineforge::live {
namespace {

std::optional<std::string> target_name(const Json& value) {
    if (value.kind == Json::Kind::Null) return std::nullopt;
    auto name = value.text();
    if (name.empty() || name.size() > 128 ||
        name.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_.:-") != std::string::npos)
        throw std::runtime_error("webhook target id must be 1..128 identifier characters");
    return name;
}

void secret_name(const std::string& name) {
    if (name.empty() || name.size() > 128 ||
        name.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_") != std::string::npos ||
        (name.front() >= '0' && name.front() <= '9'))
        throw std::runtime_error("webhook secret_env must name an environment variable, not contain a secret");
}

long bounded(const Json& value, long minimum, long maximum, const char* name) {
    const auto number = value.integer<std::uint64_t>();
    if (number < static_cast<std::uint64_t>(minimum) || number > static_cast<std::uint64_t>(maximum))
        throw std::runtime_error(std::string("webhook delivery ") + name + " out of range");
    return static_cast<long>(number);
}

void validate_targets(const RoutingConfig& config) {
    for (const auto& [name, target] : config.targets) {
        (void)name;
        HttpOptions options;
        options.url = target.url;
        options.allow_insecure_http = config.allow_insecure_http;
        options.connect_timeout_ms = config.delivery.connect_timeout_ms;
        options.total_timeout_ms = config.delivery.total_timeout_ms;
        validate_http(options);
    }
}

}

RoutingConfig parse_routes(const std::string& text, bool allow_http,
                           const std::string& default_url, const std::string& default_secret_env) {
    RoutingConfig config;
    config.routed = true;
    config.allow_insecure_http = allow_http;
    config.document = parse_json(text);
    config.file_identity = sha256_hex(text);
    const auto& root = config.document;
    only_fields(root, {"schema_version", "default_target", "targets", "rules", "delivery"});
    if (root.at("schema_version").integer<unsigned>() != 1)
        throw std::runtime_error("unsupported webhook routes schema_version; expected 1");
    config.default_target = target_name(root.at("default_target"));
    const auto& targets = root.at("targets");
    if (targets.kind != Json::Kind::Object || targets.members.size() > 128)
        throw std::runtime_error("webhook targets must be an object with at most 128 targets");
    for (const auto& [name, value] : targets.members) {
        target_name(Json::string(name));
        only_fields(value, {"url", "secret_env"});
        WebhookTarget target{value.at("url").text(), value.at("secret_env").text()};
        secret_name(target.secret_env);
        config.targets.emplace(name, std::move(target));
    }
    const auto require_target = [&](const std::optional<std::string>& target) {
        if (target && !config.targets.count(*target))
            throw std::runtime_error("undefined webhook target: " + *target);
    };
    require_target(config.default_target);
    const auto& rules = root.at("rules");
    if (rules.kind != Json::Kind::Array || rules.items.size() > 4096)
        throw std::runtime_error("webhook rules must be an array with at most 4096 rules");
    for (const auto& value : rules.items) {
        only_fields(value, {"match", "target"});
        const auto& match = value.at("match");
        if (match.find("alert_message") ||
            (match.find("kind") && match.at("kind").kind == Json::Kind::String &&
             match.at("kind").text() == "close"))
            throw std::runtime_error("B2 webhook selectors require the missing strategy-metadata extension: Pine alert_message and distinct close provenance");
        only_fields(match, {"order_id", "kind", "side"});
        RoutingRule rule;
        if (const auto* order = match.find("order_id")) rule.order_id = order->text();
        if (const auto* kind = match.find("kind")) {
            rule.kind = kind->text();
            if (*rule.kind != "entry" && *rule.kind != "exit")
                throw std::runtime_error("webhook kind must be entry or exit");
        }
        if (const auto* side = match.find("side")) {
            rule.side = side->text();
            if (*rule.side != "long" && *rule.side != "short")
                throw std::runtime_error("webhook side must be long or short");
        }
        rule.target = target_name(value.at("target"));
        require_target(rule.target);
        config.rules.push_back(std::move(rule));
    }
    if (const auto* delivery = root.find("delivery")) {
        only_fields(*delivery, {"max_in_flight", "connect_timeout_ms", "total_timeout_ms", "transport_retries", "retry_backoff_ms"});
        if (const auto* value = delivery->find("max_in_flight"))
            config.delivery.max_in_flight = static_cast<std::size_t>(bounded(*value, 1, 1024, "max_in_flight"));
        if (const auto* value = delivery->find("connect_timeout_ms"))
            config.delivery.connect_timeout_ms = bounded(*value, 1, 300000, "connect_timeout_ms");
        if (const auto* value = delivery->find("total_timeout_ms"))
            config.delivery.total_timeout_ms = bounded(*value, 1, 300000, "total_timeout_ms");
        if (const auto* value = delivery->find("transport_retries"))
            config.delivery.transport_retries = static_cast<unsigned>(bounded(*value, 0, 2, "transport_retries"));
        if (const auto* value = delivery->find("retry_backoff_ms")) {
            if (value->kind != Json::Kind::Array || value->items.size() > 2 ||
                value->items.size() < config.delivery.transport_retries)
                throw std::runtime_error("retry_backoff_ms must provide a delay for each transport retry (at most 2)");
            config.delivery.retry_backoff_ms.clear();
            for (const auto& delay : value->items)
                config.delivery.retry_backoff_ms.push_back(bounded(delay, 1, 300000, "retry_backoff_ms"));
        }
    }
    if (config.delivery.connect_timeout_ms > config.delivery.total_timeout_ms)
        throw std::runtime_error("connect_timeout_ms must not exceed total_timeout_ms");
    if (!default_url.empty() || !default_secret_env.empty()) {
        if (!config.default_target)
            throw std::runtime_error("webhook CLI flags conflict with journal-only default_target");
        const auto& target = config.targets.at(*config.default_target);
        if ((!default_url.empty() && target.url != default_url) ||
            (!default_secret_env.empty() && target.secret_env != default_secret_env))
            throw std::runtime_error("webhook CLI flags must agree with the routes file's default target");
    }
    validate_targets(config);
    return config;
}

RoutingConfig single_target(const std::string& url, const std::string& secret_env, bool allow_http) {
    RoutingConfig config;
    config.allow_insecure_http = allow_http;
    if (!secret_env.empty()) secret_name(secret_env);
    if (url.empty() && !secret_env.empty())
        throw std::runtime_error("webhook-secret-env requires a webhook target");
    if (!url.empty()) {
        config.default_target = "default";
        config.targets.emplace("default", WebhookTarget{url, secret_env});
    }
    validate_targets(config);
    config.document = Json::object({{"url", Json::string(url)}, {"secret_env", Json::string(secret_env)}});
    return config;
}

std::optional<std::string> RoutingConfig::select(const std::string& order_id,
                                               const std::string& kind, const std::string& side) const {
    for (const auto& rule : rules)
        if ((!rule.order_id || *rule.order_id == order_id) &&
            (!rule.kind || *rule.kind == kind) && (!rule.side || *rule.side == side))
            return rule.target;
    return default_target;
}

std::map<std::string, HttpOptions> RoutingConfig::load_secrets() const {
    std::map<std::string, HttpOptions> result;
    for (const auto& [name, target] : targets) {
        HttpOptions options;
        options.url = target.url;
        options.allow_insecure_http = allow_insecure_http;
        options.connect_timeout_ms = delivery.connect_timeout_ms;
        options.total_timeout_ms = delivery.total_timeout_ms;
        if (!target.secret_env.empty()) {
            const char* secret = std::getenv(target.secret_env.c_str());
            if (!secret || !*secret)
                throw std::runtime_error("webhook secret environment variable is missing or empty for target " + name);
            options.hmac_secret = secret;
        }
        result.emplace(name, std::move(options));
    }
    return result;
}

std::string RoutingConfig::stored_document() const {
    return Json::object({{"routed", Json::boolean(routed)},
                         {"allow_insecure_http", Json::boolean(allow_insecure_http)},
                         {"file_identity", Json::string(file_identity)}, {"configuration", document}}).dump();
}

RoutingConfig restore_routes(const std::string& document) {
    const auto value = parse_json(document);
    only_fields(value, {"routed", "allow_insecure_http", "file_identity", "configuration"});
    for (const auto* name : {"routed", "allow_insecure_http"})
        if (value.at(name).kind != Json::Kind::Bool)
            throw std::runtime_error("invalid stored routing configuration");
    const bool insecure = value.at("allow_insecure_http").value == "true";
    const auto& configuration = value.at("configuration");
    auto config = value.at("routed").value == "true"
        ? parse_routes(configuration.dump(), insecure)
        : single_target(configuration.at("url").text(), configuration.at("secret_env").text(), insecure);
    config.file_identity = value.at("file_identity").text();
    return config;
}

std::string delivery_identity(const std::string& event_id, const std::optional<std::string>& target) {
    return sha256_hex(Json::object({{"event_id", Json::string(event_id)},
                                   {"target_id", target ? Json::string(*target) : Json{}}}).dump());
}

}
