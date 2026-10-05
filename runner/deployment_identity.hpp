#pragma once

#include "json.hpp"
#include "transport.hpp"
#include <string>

namespace pineforge::live {

inline std::string bind_deployment_identity(std::string deployment,
                                           const std::string& settings_receipt,
                                           const std::string& capabilities_receipt,
                                           bool routed,
                                           const std::string& routing_identity,
                                           const std::string& confirmed_bar_receipt = {}) {
    if (!settings_receipt.empty())
        deployment = sha256_hex(deployment + ":settings-v1:" + settings_receipt);
    if (!capabilities_receipt.empty())
        deployment = sha256_hex(deployment + ":capabilities-v1:" + capabilities_receipt);
    if (!confirmed_bar_receipt.empty())
        deployment = sha256_hex(deployment + ":confirmed-bars-v1:" + confirmed_bar_receipt);
    if (routed)
        deployment = sha256_hex(Json::object({{"deployment", Json::string(deployment)},
            {"webhook_routes", Json::string(routing_identity)}}).dump());
    return deployment;
}

}
