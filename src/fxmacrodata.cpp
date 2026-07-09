#include <pineforge/fxmacrodata.hpp>

#include <cctype>
#include <iomanip>
#include <sstream>
#include <stdexcept>

namespace pineforge {
namespace {

std::string trim_trailing_slash(std::string value) {
    while (!value.empty() && value.back() == '/') {
        value.pop_back();
    }
    return value;
}

std::string ensure_leading_slash(const std::string& path) {
    if (!path.empty() && path.front() == '/') {
        return path;
    }
    return "/" + path;
}

std::string lower(std::string value) {
    for (char& ch : value) {
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    }
    return value;
}

std::string encode(std::string value) {
    std::ostringstream out;
    out << std::uppercase << std::hex;
    for (unsigned char ch : value) {
        if (std::isalnum(ch) || ch == '-' || ch == '_' || ch == '.' || ch == '~') {
            out << static_cast<char>(ch);
        } else {
            out << '%' << std::setw(2) << std::setfill('0') << static_cast<int>(ch);
        }
    }
    return out.str();
}

}  // namespace

FxMacroDataClient::FxMacroDataClient(std::string api_key,
                                     std::string base_url,
                                     Transport transport)
    : api_key_(std::move(api_key)),
      base_url_(trim_trailing_slash(std::move(base_url))),
      transport_(std::move(transport)) {}

std::string FxMacroDataClient::build_url(const std::string& path,
                                         QueryParams params) const {
    if (!api_key_.empty()) {
        params.emplace_back("api_key", api_key_);
    }

    std::ostringstream url;
    url << base_url_ << ensure_leading_slash(path);
    char separator = '?';
    for (const auto& [key, value] : params) {
        url << separator << encode(key) << '=' << encode(value);
        separator = '&';
    }
    return url.str();
}

std::string FxMacroDataClient::send(const std::string& method,
                                    const std::string& path,
                                    QueryParams params,
                                    const std::string& body) const {
    if (!transport_) {
        throw std::runtime_error("FxMacroDataClient requires a transport callback");
    }
    return transport_(method, build_url(path, std::move(params)), body);
}

std::string FxMacroDataClient::data_catalogue(const std::string& currency,
                                              QueryParams params) const {
    return send("GET", "/data_catalogue/" + encode(lower(currency)), std::move(params));
}

std::string FxMacroDataClient::announcements(const std::string& currency,
                                             const std::string& indicator,
                                             QueryParams params) const {
    return send("GET", "/announcements/" + encode(lower(currency)) + "/" + encode(indicator),
                std::move(params));
}

std::string FxMacroDataClient::latest_announcements(const std::string& currency,
                                                    QueryParams params) const {
    return send("GET", "/announcements/" + encode(lower(currency)) + "/latest",
                std::move(params));
}

std::string FxMacroDataClient::announcement_changes(QueryParams params) const {
    return send("GET", "/announcements/changes", std::move(params));
}

std::string FxMacroDataClient::calendar(const std::string& currency,
                                        QueryParams params) const {
    return send("GET", "/calendar/" + encode(lower(currency)), std::move(params));
}

std::string FxMacroDataClient::predictions(const std::string& currency,
                                           const std::string& indicator,
                                           QueryParams params) const {
    return send("GET", "/predictions/" + encode(lower(currency)) + "/" + encode(indicator),
                std::move(params));
}

std::string FxMacroDataClient::forex(const std::string& base,
                                     const std::string& quote,
                                     QueryParams params) const {
    return send("GET", "/forex/" + encode(lower(base)) + "/" + encode(lower(quote)),
                std::move(params));
}

std::string FxMacroDataClient::cot(const std::string& currency,
                                   QueryParams params) const {
    return send("GET", "/cot/" + encode(lower(currency)), std::move(params));
}

std::string FxMacroDataClient::commodity(const std::string& indicator,
                                         QueryParams params) const {
    return send("GET", "/commodities/" + encode(indicator), std::move(params));
}

std::string FxMacroDataClient::commodities_latest(QueryParams params) const {
    return send("GET", "/commodities/latest", std::move(params));
}

std::string FxMacroDataClient::curves(const std::string& currency,
                                      QueryParams params) const {
    return send("GET", "/curves/" + encode(lower(currency)), std::move(params));
}

std::string FxMacroDataClient::curve_proxies(const std::string& currency,
                                             QueryParams params) const {
    return send("GET", "/curve_proxies/" + encode(lower(currency)), std::move(params));
}

std::string FxMacroDataClient::forward_curves(const std::string& currency,
                                              QueryParams params) const {
    return send("GET", "/forward_curves/" + encode(lower(currency)), std::move(params));
}

std::string FxMacroDataClient::rate_differentials(const std::string& base,
                                                  const std::string& quote,
                                                  QueryParams params) const {
    return send("GET", "/rate_differentials/" + encode(lower(base)) + "/" + encode(lower(quote)),
                std::move(params));
}

std::string FxMacroDataClient::forward_differentials(const std::string& base,
                                                     const std::string& quote,
                                                     QueryParams params) const {
    return send("GET", "/forward_differentials/" + encode(lower(base)) + "/" + encode(lower(quote)),
                std::move(params));
}

std::string FxMacroDataClient::market_sessions(QueryParams params) const {
    return send("GET", "/market_sessions", std::move(params));
}

std::string FxMacroDataClient::risk_sentiment(QueryParams params) const {
    return send("GET", "/risk_sentiment", std::move(params));
}

std::string FxMacroDataClient::news(const std::string& currency,
                                    QueryParams params) const {
    return send("GET", "/news/" + encode(lower(currency)), std::move(params));
}

std::string FxMacroDataClient::press_releases(const std::string& currency,
                                              QueryParams params) const {
    return send("GET", "/press-releases/" + encode(lower(currency)), std::move(params));
}

std::string FxMacroDataClient::graphql(const std::string& query,
                                       const std::string& variables_json) const {
    return send("POST", "/graphql", {}, "{\"query\":\"" + query + "\",\"variables\":" + variables_json + "}");
}

std::string FxMacroDataClient::request(const std::string& path,
                                       QueryParams params,
                                       std::string method,
                                       std::string body) const {
    return send(std::move(method), path, std::move(params), body);
}

}  // namespace pineforge
