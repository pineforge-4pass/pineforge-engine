#include <fstream>
#include <iostream>
#include <string>
#include <string_view>

namespace {

constexpr std::string_view expected = "release-toolchain";

int not_compared(std::string_view reason, std::string_view want, std::string_view actual) {
    std::cerr << "NOT COMPARED #957 " << reason << ": expected " << want
              << ", actual " << actual << '\n';
    return 2;
}

}  // namespace

int main() {
    // Read the workflow in the checkout that supplied this test's source.
    const std::string source = __FILE__;
    const auto slash = source.find_last_of("/\\");
    if (slash == std::string::npos)
        return not_compared("test source path", "a path to the test source", source);
    const auto path = source.substr(0, slash + 1) + "../.github/workflows/release.yml";
    std::ifstream workflow(path);
    if (!workflow)
        return not_compared("release workflow input", "readable .github/workflows/release.yml",
                            "<missing or unreadable>");

    // Match the checked-in block layout; declarations in other jobs or
    // nested under steps are not job settings for prebuilt-x86.
    constexpr std::string_view environment_key = "    environment:";
    constexpr std::string_view auth_action = "        uses: google-github-actions/auth@";
    enum class Section { other, permissions, steps };
    auto section = Section::other;
    bool in_jobs = false;
    bool in_x86 = false;
    bool in_auth = false;
    unsigned jobs = 0, oidc_writes = 0, auth_actions = 0, environments = 0;
    std::string actual;
    for (std::string line; std::getline(workflow, line);) {
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        const auto indent = line.find_first_not_of(" \t\r");
        if (indent == std::string::npos || line[indent] == '#')
            continue;
        if (indent == 0) {
            in_jobs = line == "jobs:";
            in_x86 = false;
        }
        if (!in_jobs)
            continue;
        if (indent == 2) {
            in_x86 = line == "  prebuilt-x86:";
            section = Section::other;
            if (in_x86)
                ++jobs;
            continue;
        }
        if (!in_x86)
            continue;
        const std::string_view row = line;
        if (indent == 4) {
            section = row == "    permissions:" ? Section::permissions :
                      row == "    steps:" ? Section::steps : Section::other;
            in_auth = false;
        }
        if (section == Section::permissions && row == "      id-token: write")
            ++oidc_writes;
        if (section == Section::steps && indent == 6)
            in_auth = row == "      - name: Authenticate the dedicated toolchain reader";
        if (in_auth && row.substr(0, auth_action.size()) == auth_action)
            ++auth_actions;
        if (row.substr(0, environment_key.size()) == environment_key) {
            if (environments++ != 0)
                actual += ", ";
            const auto start = row.find_first_not_of(" \t", environment_key.size());
            actual += start == row.npos ? "<empty>" : std::string(row.substr(start));
        }
    }
    if (workflow.bad() || !workflow.eof())
        return not_compared("release workflow read", "complete input", "<read error>");
    if (jobs != 1)
        return not_compared("release.yml prebuilt-x86 job count", "1", std::to_string(jobs));
    if (oidc_writes != 1)
        return not_compared("prebuilt-x86 id-token: write count", "1", std::to_string(oidc_writes));
    if (auth_actions != 1)
        return not_compared("prebuilt-x86 Google auth action count", "1", std::to_string(auth_actions));

    if (environments == 0)
        actual = "<missing>";
    if (environments != 1 || std::string_view(actual) != expected) {
        std::cerr << "FAIL #957 release.yml prebuilt-x86 environment: expected " << expected
                  << ", actual " << actual << '\n';
        return 1;
    }
    std::cout << "PASS #957 release.yml prebuilt-x86 environment: expected " << expected
              << ", actual " << actual << '\n';
    return 0;
}
