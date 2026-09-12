#pragma once

#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <string>
#include <vector>

namespace native_proof {

inline std::string hex64(uint64_t value) {
    std::ostringstream out;
    out << std::hex << std::setw(16) << std::setfill('0') << value;
    return out.str();
}

struct Scenario {
    std::string id;
    std::string status = "passed";
    std::string native_configuration;
    std::string run_identity;
    std::string calendar;
    std::string logical_inputs;
    std::string lifecycle_events;
    std::string physical_effects;
    std::string observations;
    std::string comparisons;
};

inline bool write_if_requested(const char* test_name,
                               const std::vector<Scenario>& scenarios,
                               const std::string& fixture_name,
                               const std::string& fixture_sha256) {
    const char* dir = std::getenv("PINEFORGE_NATIVE_PROOF_OUTPUT");
    if (!dir || !*dir) return true;
    std::ostringstream json;
    json << "{\n  \"schemaVersion\": \"pineforge-native-scenario-artifact/v1\",\n"
         << "  \"testName\": \"" << test_name << "\",\n"
         << "  \"scenarios\": [\n";
    for (std::size_t i = 0; i < scenarios.size(); ++i) {
        const auto& s = scenarios[i];
        json << "    {\n"
             << "      \"scenarioId\": \"" << s.id << "\",\n"
             << "      \"status\": \"" << s.status << "\",\n"
             << "      \"assertionsEnabled\": true,\n"
             << "      \"fixture\": { \"" << fixture_name << "\": \"" << fixture_sha256 << "\" },\n"
             << "      \"nativeConfiguration\": " << (s.native_configuration.empty() ? "{}" : s.native_configuration) << ",\n"
             << "      \"runIdentity\": " << (s.run_identity.empty() ? "null" : s.run_identity) << ",\n"
             << "      \"calendar\": " << (s.calendar.empty() ? "{}" : s.calendar) << ",\n"
             << "      \"logicalInputs\": " << (s.logical_inputs.empty() ? "[]" : s.logical_inputs) << ",\n"
             << "      \"lifecycleEvents\": " << (s.lifecycle_events.empty() ? "[]" : s.lifecycle_events) << ",\n"
             << "      \"physicalEffects\": " << (s.physical_effects.empty() ? "[]" : s.physical_effects) << ",\n"
             << "      \"observations\": " << (s.observations.empty() ? "{}" : s.observations) << ",\n"
             << "      \"comparisons\": " << (s.comparisons.empty() ? "[]" : s.comparisons) << "\n"
             << "    }" << (i + 1 == scenarios.size() ? "\n" : ",\n");
    }
    json << "  ]\n}\n";
    std::string path = std::string(dir) + "/" + test_name + ".json";
    std::ofstream out(path.c_str(), std::ios::binary);
    if (!out) return false;
    out << json.str();
    return static_cast<bool>(out);
}

}  // namespace native_proof
