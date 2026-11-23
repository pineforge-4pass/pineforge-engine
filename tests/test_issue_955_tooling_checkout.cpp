#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

namespace {

int not_compared(const char* reason, const std::string& expected,
                 const std::string& actual) {
    std::cerr << "NOT COMPARED: " << reason << "; expected [" << expected
              << "], actual [" << actual << "]\n";
    return 1;
}

}  // namespace

int main() {
    const auto path = std::filesystem::path(__FILE__).parent_path().parent_path() /
                      ".github/workflows/promote-baseline.yml";
    std::ifstream workflow(path);
    if (!workflow) {
        return not_compared("cannot open promotion workflow", path.string(), "<unreadable>");
    }

    // Read only the second checkout's own inputs. The first checkout is used
    // by the subsequent git fetch, so its settings cannot satisfy this check.
    const std::string name = "      - name: Checkout campaign tooling (pineforge-workflow)";
    const std::string checkout_pin = "        uses: actions/checkout@";
    const std::string path_key = "          path: ";
    const std::string token_key = "          token: ";
    const std::string credential_key = "          persist-credentials: ";
    int jobs = 0;
    int checkouts = 0;
    int tooling_checkout = 0;
    int steps = 0;
    int credentials = 0;
    bool in_promote = false;
    bool in_step = false;
    bool in_inputs = false;
    std::string uses, checkout_path, token, credential;
    for (std::string line; std::getline(workflow, line);) {
        const auto indent = line.find_first_not_of(' ');
        if (indent == std::string::npos || line[indent] == '#') continue;
        if (indent <= 2) {
            in_promote = line == "  promote:";
            if (in_promote) ++jobs;
        }
        if (indent <= 6) {
            in_step = in_promote && line == name;
            in_inputs = false;
            if (in_step) ++steps;
        }
        if (!in_promote) continue;
        if (line.rfind("      - uses: actions/checkout@", 0) == 0 ||
            line.rfind(checkout_pin, 0) == 0) {
            ++checkouts;
            if (in_step) tooling_checkout = checkouts;
        }
        if (!in_step) continue;
        if (indent == 8) {
            in_inputs = line == "        with:";
            if (line.rfind("        uses: ", 0) == 0) uses = line;
        }
        if (!in_inputs || indent != 10) continue;
        if (line.rfind(path_key, 0) == 0) checkout_path = line.substr(path_key.size());
        if (line.rfind(token_key, 0) == 0) token = line.substr(token_key.size());
        if (line.rfind(credential_key, 0) == 0) {
            ++credentials;
            credential = line.substr(credential_key.size());
        }
    }

    if (workflow.bad() || !workflow.eof()) {
        return not_compared("workflow read failed", "EOF", "<read error>");
    }
    if (jobs != 1) {
        return not_compared("promotion job count", "1", std::to_string(jobs));
    }
    if (steps != 1) return not_compared("campaign tooling checkout count", "1", std::to_string(steps));
    if (uses.rfind(checkout_pin, 0) != 0) {
        return not_compared("campaign tooling action", "actions/checkout", uses);
    }
    if (tooling_checkout != 2 || checkouts != 2) {
        return not_compared("campaign tooling checkout position", "2 of 2",
                            std::to_string(tooling_checkout) + " of " + std::to_string(checkouts));
    }
    if (checkout_path != ".workflow") return not_compared("checkout path", ".workflow", checkout_path);
    if (token != "${{ steps.app.outputs.token }}") {
        return not_compared("checkout token input", "${{ steps.app.outputs.token }}",
                            token.empty() ? "<missing>" : "<different binding>");
    }
    if (credentials == 0) credential = "<unset; checkout defaults to true>";
    if (credentials > 1) credential = "<duplicate inputs>";
    if (credentials != 1 || credential != "false") {
        std::cerr << "FAIL: tooling checkout persist-credentials: expected [false], actual ["
                  << credential << "]\n";
        return 1;
    }
    std::cout << "PASS: tooling checkout persist-credentials = false\n";
    return 0;
}
