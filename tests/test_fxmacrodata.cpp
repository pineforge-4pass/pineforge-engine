#include <pineforge/fxmacrodata.hpp>

#include <stdexcept>
#include <string>

static void require(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

int main() {
    std::string method;
    std::string url;
    std::string body;

    pineforge::FxMacroDataClient client(
        "test-key",
        "https://api.fxmacrodata.com/v1/",
        [&](const std::string& m, const std::string& u, const std::string& b) {
            method = m;
            url = u;
            body = b;
            return std::string{"{\"ok\":true}"};
        });

    require(client.calendar("USD", {{"days_ahead", "30"}}) == "{\"ok\":true}",
            "transport result should be returned");
    require(method == "GET", "calendar should use GET");
    require(url == "https://api.fxmacrodata.com/v1/calendar/usd?days_ahead=30&api_key=test-key",
            "calendar URL should include normalized currency and auth");

    client.rate_differentials("EUR", "USD", {{"tenor", "2y"}});
    require(url == "https://api.fxmacrodata.com/v1/rate_differentials/eur/usd?tenor=2y&api_key=test-key",
            "rate differential endpoint should include both currencies");

    client.graphql("query { marketSessions { name } }");
    require(method == "POST", "GraphQL should use POST");
    require(url == "https://api.fxmacrodata.com/v1/graphql?api_key=test-key",
            "GraphQL URL should include auth");
    require(body.find("marketSessions") != std::string::npos,
            "GraphQL query should be placed in the request body");

    return 0;
}
