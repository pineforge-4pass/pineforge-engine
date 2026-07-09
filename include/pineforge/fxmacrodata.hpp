#pragma once

#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace pineforge {

class FxMacroDataClient {
public:
    using QueryParams = std::vector<std::pair<std::string, std::string>>;
    using Transport = std::function<std::string(const std::string& method,
                                                const std::string& url,
                                                const std::string& body)>;

    explicit FxMacroDataClient(std::string api_key = {},
                               std::string base_url = "https://api.fxmacrodata.com/v1",
                               Transport transport = {});

    std::string data_catalogue(const std::string& currency,
                               QueryParams params = {}) const;
    std::string announcements(const std::string& currency,
                              const std::string& indicator,
                              QueryParams params = {}) const;
    std::string latest_announcements(const std::string& currency,
                                     QueryParams params = {}) const;
    std::string announcement_changes(QueryParams params = {}) const;
    std::string calendar(const std::string& currency,
                         QueryParams params = {}) const;
    std::string predictions(const std::string& currency,
                            const std::string& indicator,
                            QueryParams params = {}) const;
    std::string forex(const std::string& base,
                      const std::string& quote,
                      QueryParams params = {}) const;
    std::string cot(const std::string& currency, QueryParams params = {}) const;
    std::string commodity(const std::string& indicator,
                          QueryParams params = {}) const;
    std::string commodities_latest(QueryParams params = {}) const;
    std::string curves(const std::string& currency, QueryParams params = {}) const;
    std::string curve_proxies(const std::string& currency,
                              QueryParams params = {}) const;
    std::string forward_curves(const std::string& currency,
                               QueryParams params = {}) const;
    std::string rate_differentials(const std::string& base,
                                   const std::string& quote,
                                   QueryParams params = {}) const;
    std::string forward_differentials(const std::string& base,
                                      const std::string& quote,
                                      QueryParams params = {}) const;
    std::string market_sessions(QueryParams params = {}) const;
    std::string risk_sentiment(QueryParams params = {}) const;
    std::string news(const std::string& currency, QueryParams params = {}) const;
    std::string press_releases(const std::string& currency,
                               QueryParams params = {}) const;
    std::string graphql(const std::string& query,
                        const std::string& variables_json = "{}") const;
    std::string request(const std::string& path,
                        QueryParams params = {},
                        std::string method = "GET",
                        std::string body = {}) const;

    std::string build_url(const std::string& path, QueryParams params = {}) const;

private:
    std::string api_key_;
    std::string base_url_;
    Transport transport_;

    std::string send(const std::string& method,
                     const std::string& path,
                     QueryParams params,
                     const std::string& body = {}) const;
};

}  // namespace pineforge
