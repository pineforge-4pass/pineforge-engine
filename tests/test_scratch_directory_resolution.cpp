// The resolver's filesystem probe is injected: no mounts or privileged
// rearrangement of /tmp are needed, and no test case can skip.
#include <pineforge/detail/scratch_directory.hpp>

#include "default_scratch_tmpfs_fixture.hpp"

namespace {
using namespace s4e_tmp;
constexpr std::uint64_t disk = UINT64_C(0xef53);
constexpr std::uint64_t tmpfs = UINT64_C(0x01021994);
constexpr std::uint64_t ramfs = UINT64_C(0x858458f6);

struct Probe {
    const std::vector<std::pair<std::string, std::uint64_t>>& filesystems;
    std::vector<std::string>& visited;
    int absent_error = ENOENT;
    int operator()(const char* path, std::uint64_t& type) const {
        visited.emplace_back(path);
        for (const auto& entry : filesystems) {
            if (entry.first == path) { type = entry.second; return 0; }
        }
        return absent_error;
    }
};

void row() {
    Environment temporary("TMPDIR", nullptr);
    std::string refusal;
    int error = 0;
    std::vector<std::string> visited;
    const auto resolve = [&](const std::string& configured,
                             const std::vector<std::pair<std::string, std::uint64_t>>& facts,
                             int absent_error = ENOENT) {
        visited.clear();
        return pineforge::detail::resolve_scratch_directory(
            configured, refusal, error, Probe{facts, visited, absent_error});
    };
    require(resolve("", {{"/tmp", tmpfs}, {"/var/tmp", disk}}) == "/var/tmp"
                && error == 0 && refusal.empty(),
            "unset TMPDIR and tmpfs /tmp must select disk-backed /var/tmp");
    require(visited == std::vector<std::string>{"/tmp", "/var/tmp"},
            "unset TMPDIR candidate order changed");
    require(resolve("", {{"/tmp", disk}, {"/var/tmp", ramfs}}) == "/tmp"
                && visited == std::vector<std::string>{"/tmp"},
            "disk-backed legacy /tmp must precede /var/tmp");
    require(resolve("", {{"/tmp", tmpfs}, {"/var/tmp", ramfs}}).empty() && error == ENODEV,
            "all-memory default candidates must be refused with ENODEV");
    require(refusal.find("/tmp") != std::string::npos
                && refusal.find("tmpfs") != std::string::npos
                && refusal.find("/var/tmp") != std::string::npos
                && refusal.find("ramfs") != std::string::npos
                && refusal.find("set TMPDIR to a disk-backed directory") != std::string::npos,
            "default refusal must name every candidate, its rejection and the remedy");
    require(resolve("", {{"/var/tmp", tmpfs}}).empty()
                && error == ENOENT && visited == std::vector<std::string>{"/tmp"}
                && refusal.find("statfs") != std::string::npos
                && refusal.find("errno 2") != std::string::npos,
            "failed filesystem probes must remain visible in the refusal");
    require(refusal.find("/var/tmp") == std::string::npos
                && refusal.find("set TMPDIR") == std::string::npos,
            "a legacy /tmp query error must stop before fallback or memory-backing advice");
    for (const int query_error : {ENOENT, ENOTDIR, EACCES, EIO, ENODEV}) {
        require(resolve("", {{"/var/tmp", disk}}, query_error).empty()
                    && error == query_error && visited.size() == 1 && visited.front() == "/tmp",
                "an unset TMPDIR must preserve every legacy /tmp query error without fallback");
    }
    {
        Environment selected("TMPDIR", "/named-like-disk");
        for (const int query_error : {ENOENT, ENOTDIR, EACCES, EIO, ENODEV}) {
            require(resolve("", {{"/var/tmp", disk}}, query_error).empty()
                        && error == query_error,
                    "a TMPDIR query error must fail with its exact errno despite disk /var/tmp");
            require(visited.size() == 1 && visited.front() == "/named-like-disk",
                    "a TMPDIR query error must make exactly one probe and never probe /var/tmp");
            require(refusal.find("TMPDIR '/named-like-disk'") != std::string::npos
                        && refusal.find("statfs failed (errno " + std::to_string(query_error) + ": "
                            + std::error_code(query_error, std::generic_category()).message() + ")")
                            != std::string::npos
                        && refusal.find("/var/tmp") == std::string::npos
                        && refusal.find("set TMPDIR") == std::string::npos,
                    "a TMPDIR query error must name the candidate and errno without fallback advice");
        }
        require(resolve("", {{"/named-like-disk", tmpfs}, {"/var/tmp", disk}}) == "/var/tmp"
                    && error == 0 && refusal.empty()
                    && visited == std::vector<std::string>{"/named-like-disk", "/var/tmp"},
                "proven tmpfs TMPDIR must fall back to disk-backed /var/tmp");
        require(resolve("", {{"/named-like-disk", ramfs}, {"/tmp", disk}, {"/var/tmp", disk}})
                    == "/var/tmp"
                    && visited == std::vector<std::string>{"/named-like-disk", "/var/tmp"},
                "set TMPDIR on ramfs must fall back to /var/tmp by type");
        require(resolve("", {{"/named-like-disk", ramfs}, {"/var/tmp", tmpfs}}).empty()
                    && error == ENODEV
                    && visited == std::vector<std::string>{"/named-like-disk", "/var/tmp"},
                "proven ramfs TMPDIR and memory-backed /var/tmp must fail with ENODEV");
        require(refusal.find("TMPDIR '/named-like-disk': ramfs") != std::string::npos
                    && refusal.find("fallback '/var/tmp': tmpfs") != std::string::npos
                    && refusal.find("set TMPDIR to a disk-backed directory") != std::string::npos,
                "all-memory TMPDIR refusal must name both candidates, their types and the remedy");
        require(resolve("", {{"/named-like-disk", tmpfs}}).empty()
                    && error == ENOENT
                    && visited == std::vector<std::string>{"/named-like-disk", "/var/tmp"}
                    && refusal.find("TMPDIR '/named-like-disk': tmpfs") != std::string::npos
                    && refusal.find("fallback '/var/tmp': statfs failed (errno 2")
                        != std::string::npos
                    && refusal.find("set TMPDIR") == std::string::npos,
                "a /var/tmp query error must retain its errno and both candidate diagnostics");
        require(resolve("", {{"/named-like-disk", disk}, {"/var/tmp", tmpfs}})
                    == "/named-like-disk"
                    && error == 0 && refusal.empty()
                    && visited == std::vector<std::string>{"/named-like-disk"},
                "disk-backed TMPDIR must win");
        require(resolve("/dev/shm", {{"/dev/shm", disk}}) == "/dev/shm"
                    && visited == std::vector<std::string>{"/dev/shm"},
                "a directory name must not determine its filesystem type");
        require(resolve("/explicit/", {{"/explicit/", disk}}) == "/explicit/",
                "an accepted explicit path must pass through byte-for-byte");
        require(resolve("/explicit", {{"/explicit", tmpfs}, {"/var/tmp", disk}}).empty()
                    && error == ENODEV && visited == std::vector<std::string>{"/explicit"},
                "explicit tmpfs must be refused without trying a default");
        require(refusal.find("/explicit") != std::string::npos
                    && refusal.find("tmpfs") != std::string::npos,
                "explicit refusal must name the rejected candidate and filesystem type");
    }
    {
        Environment empty("TMPDIR", "");
        require(resolve("", {{"/tmp", ramfs}, {"/var/tmp", disk}}) == "/var/tmp",
                "empty TMPDIR must preserve the legacy /tmp candidate before /var/tmp");
    }
}
} // namespace

int main() {
    return s4e_tmp::test_main("test_scratch_directory_resolution", row);
}
