// SPDX-License-Identifier: Apache-2.0
#include "../runner/service.hpp"

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <sstream>
#include <stdexcept>
#include <string>
#include <system_error>
#include <vector>

namespace {

namespace fs = std::filesystem;
using pineforge::live::Json;
using pineforge::live::atomic_json_file;

int refusals_remaining = 0;
unsigned temporary_unlinks = 0;

} // namespace

extern "C" int __real_unlink(const char* name);

extern "C" int __wrap_unlink(const char* name) {
    if (std::strstr(name, ".new.")) {
        ++temporary_unlinks;
        if (refusals_remaining != 0) {
            if (refusals_remaining > 0) --refusals_remaining;
            errno = EACCES;
            return -1;
        }
    }
    return __real_unlink(name);
}

namespace {

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

class TemporaryDirectory {
public:
    TemporaryDirectory() {
        std::string name = (fs::temp_directory_path() / "pineforge-service-unlink-XXXXXX").string();
        require(::mkdtemp(name.data()) != nullptr, "cannot create test directory");
        path = name;
    }

    ~TemporaryDirectory() {
        refusals_remaining = 0;
        std::error_code ignored;
        fs::remove_all(path, ignored);
    }

    fs::path path;
};

class WarningCapture {
public:
    WarningCapture() : previous_(std::cerr.rdbuf(output_.rdbuf())) {}
    ~WarningCapture() { std::cerr.rdbuf(previous_); }

    std::string str() const { return output_.str(); }

private:
    std::ostringstream output_;
    std::streambuf* previous_;
};

std::vector<fs::path> directory_entries(const fs::path& directory) {
    std::vector<fs::path> entries;
    for (const auto& entry : fs::directory_iterator(directory)) entries.push_back(entry.path());
    return entries;
}

fs::path sole_temporary_file(const fs::path& target) {
    const auto entries = directory_entries(target.parent_path());
    require(entries.size() == 2, "expected the target and one temporary file");
    require(entries[0] == target || entries[1] == target, "target missing");
    const auto& temporary = entries[0] == target ? entries[1] : entries[0];
    const auto prefix = target.filename().string() + ".new.";
    require(temporary.filename().string().compare(0, prefix.size(), prefix) == 0,
            "unexpected temporary name");
    return temporary;
}

std::string contents(const fs::path& file) {
    std::ifstream input(file, std::ios::binary);
    require(static_cast<bool>(input), "cannot open installed file");
    return std::string(std::istreambuf_iterator<char>(input), {});
}

std::string expected_warning(const fs::path& temporary) {
    return "pineforge-live: warning: cannot remove temporary service file " + temporary.string() +
           ": " + std::strerror(EACCES) + "\n";
}

void link_with_persistent_refusal(const Json& document) {
    TemporaryDirectory directory;
    const auto target = directory.path / "request.json";
    WarningCapture warning;
    refusals_remaining = -1;
    temporary_unlinks = 0;
    atomic_json_file(target.string(), document, true, false);

    require(contents(target) == document.dump() + "\n", "installed document differs");
    const auto temporary = sole_temporary_file(target);
    require(fs::equivalent(temporary, target), "temporary file is not a hard link to the target");
    require(warning.str() == expected_warning(temporary), "expected exactly one cleanup warning");
    require(temporary_unlinks == 2, "expected one cleanup retry");
}

void link_with_one_refusal(const Json& document) {
    TemporaryDirectory directory;
    const auto target = directory.path / "request.json";
    WarningCapture warning;
    refusals_remaining = 1;
    temporary_unlinks = 0;
    atomic_json_file(target.string(), document, true, false);

    require(contents(target) == document.dump() + "\n", "installed document differs");
    require(directory_entries(directory.path) == std::vector<fs::path>{target}, "retry left a temporary file");
    require(warning.str().empty(), "successful retry emitted a warning");
    require(temporary_unlinks == 2, "expected one cleanup retry");
}

void check_existing_target(const Json& document, int refusals) {
    TemporaryDirectory directory;
    const auto target = directory.path / "request.json";
    const std::string original = "{}\n";
    {
        std::ofstream existing(target);
        existing << original;
        existing.close();
        require(static_cast<bool>(existing), "cannot write test target");
    }
    WarningCapture warning;
    refusals_remaining = refusals;
    temporary_unlinks = 0;
    bool install_failed = false;
    try {
        atomic_json_file(target.string(), document, true, false);
    } catch (const std::runtime_error& error) {
        require(std::string(error.what()) == "cannot install atomic service file", "installation error changed");
        install_failed = true;
    }
    require(install_failed, "existing target did not prevent installation");
    require(contents(target) == original, "failed installation changed the target");

    if (refusals) {
        const auto temporary = sole_temporary_file(target);
        require(warning.str() == expected_warning(temporary), "expected exactly one cleanup warning");
        require(temporary_unlinks == 2, "expected one cleanup retry");
    } else {
        require(directory_entries(directory.path) == std::vector<fs::path>{target}, "cleanup left a temporary file");
        require(warning.str().empty(), "successful cleanup emitted a warning");
        require(temporary_unlinks == 1, "expected one cleanup attempt");
    }
}

void link_onto_existing_target(const Json& document) {
    check_existing_target(document, 0);
    check_existing_target(document, -1);
}

void rename_does_not_unlink(const Json& document) {
    TemporaryDirectory directory;
    const auto target = directory.path / "status.json";
    WarningCapture warning;
    refusals_remaining = -1;
    temporary_unlinks = 0;
    atomic_json_file(target.string(), document, false, false);

    require(contents(target) == document.dump() + "\n", "installed document differs");
    require(directory_entries(directory.path) == std::vector<fs::path>{target}, "rename left a temporary file");
    require(warning.str().empty(), "rename emitted a cleanup warning");
    require(temporary_unlinks == 0, "rename attempted to unlink the temporary file");
}

} // namespace

int main() {
    const auto document = Json::object({{"ready", Json::boolean(true)}});
    const struct {
        const char* name;
        void (*run)(const Json&);
    } cases[] = {
        {"link_with_persistent_refusal", link_with_persistent_refusal},
        {"link_with_one_refusal", link_with_one_refusal},
        {"link_onto_existing_target", link_onto_existing_target},
        {"rename_does_not_unlink", rename_does_not_unlink},
    };
    int failures = 0;
    for (const auto& test : cases) {
        try {
            test.run(document);
        } catch (const std::exception& error) {
            std::cerr << test.name << ": " << error.what() << '\n';
            ++failures;
        }
    }
    return failures == 0 ? 0 : 1;
}
