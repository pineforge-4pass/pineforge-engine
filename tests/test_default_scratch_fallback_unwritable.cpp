// Unit witness: inject filesystem facts and an EROFS create diagnostic.
// No namespace/mount privileges or writable /var/tmp are needed.
#include "default_scratch_tmpfs_fixture.hpp"
#include <pineforge/detail/scratch_directory.hpp>

namespace {
void row() {
    using namespace s4e_tmp;
    Environment temporary("TMPDIR", "/s4e-rejected-memory");
    std::string note;
    int error = 0;
    bool proven_memory = false;
    std::vector<std::string> visited;
    const auto probe = [&](const char* path, std::uint64_t& type) {
        visited.emplace_back(path);
        type = std::string(path) == "/s4e-rejected-memory"
            ? UINT64_C(0x01021994) : UINT64_C(0xef53);
        return 0;
    };
    const auto directory = pineforge::detail::resolve_scratch_directory(
        "", note, error, proven_memory, probe);
    require(directory == "/var/tmp" && error == 0 && !proven_memory
                && visited == std::vector<std::string>{"/s4e-rejected-memory", "/var/tmp"},
            "unit fixture must select disk-backed /var/tmp after proven tmpfs");
    // These are the two leaf create wrappers' shared message builder; the
    // facade translators must retain this text (covered by their own rows).
    for (const char* operation : {"scratch create", "command scratch create"}) {
        const auto message = pineforge::detail::scratch_create_message(operation, directory, note);
        const std::system_error failure(EROFS, std::generic_category(), message);
        const std::string text = failure.what();
        require(failure.code() == std::error_code(EROFS, std::generic_category())
                    && text.find(operation) == 0
                    && text.find("/var/tmp") != std::string::npos
                    && text.find("TMPDIR '/s4e-rejected-memory': tmpfs") != std::string::npos
                    && text.find("set TMPDIR to a disk-backed directory") != std::string::npos,
                "EROFS create must keep its prefix, chosen/rejected directories and remedy");
    }
}
} // namespace

int main() {
    return s4e_tmp::test_main("test_default_scratch_fallback_unwritable", row);
}
