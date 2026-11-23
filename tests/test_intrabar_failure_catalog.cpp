#include "../runner/json.hpp"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <string_view>

namespace {

using pineforge::live::Json;

constexpr char kSchema[] = "pineforge-run-failure-catalog/v1";
constexpr char kCode[] = "run_options_rejected";
constexpr const char* kMessages[] = {
    "native lower source segment failed",
    "magnifier calendar lookup changed after pre-scan",
    "magnifier timestamp exhausted",
    "magnifier path count exhausted",
};

int not_compared(std::string_view reason, std::string_view actual,
                 std::string_view expected) {
    std::cerr << "NOT COMPARED #960 " << reason << ": actual=" << actual
              << "; expected=" << expected << '\n';
    return 1;
}

bool contains(const Json& list, std::string_view text) {
    return std::any_of(list.items.begin(), list.items.end(), [text](const Json& item) {
        return item.kind == Json::Kind::String && std::string_view(item.value) == text;
    });
}

int check_catalog(const Json& catalog) {
    const auto& schema = catalog.at("schema");
    if (schema.kind != Json::Kind::String || schema.value != kSchema)
        return not_compared("catalog schema", schema.dump(), Json::quote(kSchema));

    const auto& code = catalog.at("codes").at(kCode);
    const auto& options = code.at("args").at("option").at("values");
    if (options.kind != Json::Kind::Array || !contains(options, "intrabar_path"))
        return not_compared("codes.run_options_rejected.args.option.values", options.dump(),
                            "an array containing \"intrabar_path\"");

    const auto& english = code.at("english");
    if (english.kind != Json::Kind::Array || english.items.empty()
        || !std::all_of(english.items.begin(), english.items.end(), [](const Json& item) {
               return item.kind == Json::Kind::String;
           }))
        return not_compared("codes.run_options_rejected.english", english.dump(),
                            "a nonempty array of strings");

    bool missing = false;
    for (const char* message : kMessages) {
        if (contains(english, message)) continue;
        std::cerr << "FAIL #960 codes.run_options_rejected.english: actual=missing; expected="
                  << Json::quote(message) << '\n';
        missing = true;
    }
    if (missing) return 1;
    std::cout << "PASS #960: found all " << std::size(kMessages)
              << " intrabar path templates under " << kCode << '\n';
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc > 2)
        return not_compared("catalog argument count", std::to_string(argc - 1), "zero or one");
    // CTest reads the checkout's catalog at runtime. A path override lets the
    // lab exercise input failures with a separate file.
    const auto path = argc == 2 ? std::filesystem::path(argv[1])
                               : std::filesystem::path(__FILE__).parent_path().parent_path()
                                 / "docker" / "run_failure_codes.json";
    std::ifstream input(path, std::ios::binary);
    if (!input)
        return not_compared("catalog could not be opened", path.string(), "a readable catalog");
    try {
        const std::string bytes((std::istreambuf_iterator<char>(input)), {});
        if (bytes.empty())
            return not_compared("catalog byte count", "0", "greater than zero");
        return check_catalog(pineforge::live::parse_json(bytes));
    } catch (const std::runtime_error& error) {
        return not_compared("catalog could not be compared", error.what(),
                            "valid catalog JSON with a run_options_rejected entry");
    }
}
