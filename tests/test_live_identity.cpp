#include "deployment_identity.hpp"
#include <cassert>
#include <iostream>
#include <string>

using namespace pineforge::live;

int main() {
    const std::string base = "unchanged-library-config-and-warmup";
    const std::string settings = "unchanged-effective-settings";
    const std::string receipt = "{\"unresolved\":[],\"version\":1}";
    const std::string changed_receipt = "{\"unresolved\":[\"barstate.islast\"],\"version\":1}";
    const std::string routes = "unchanged-webhook-routing";
    assert(bind_deployment_identity(base, "", "", false, routes) == base);
    const auto settings_identity = sha256_hex(base + ":settings-v1:" + settings);
    assert(bind_deployment_identity(base, settings, "", false, routes) == settings_identity);
    const auto capabilities_identity = sha256_hex(settings_identity + ":capabilities-v1:" + receipt);
    assert(bind_deployment_identity(base, settings, receipt, false, routes) == capabilities_identity);
    assert(bind_deployment_identity(base, settings, changed_receipt, false, routes) != capabilities_identity);
    const auto routed_identity = bind_deployment_identity(base, settings, receipt, true, routes);
    assert(routed_identity == sha256_hex(Json::object({
        {"deployment", Json::string(capabilities_identity)},
        {"webhook_routes", Json::string(routes)}}).dump()));
    assert(bind_deployment_identity(base, settings, changed_receipt, true, routes) != routed_identity);
    const auto premature_routing = bind_deployment_identity(base, settings, "", true, routes);
    assert(routed_identity != sha256_hex(premature_routing + ":capabilities-v1:" + receipt));
    std::cout << "deployment identity: receipt-only changes and routing-last order PASS\n";
}
