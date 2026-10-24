// stat -> lstat must fail the disk-symlink cell (m9); R_OK must fail 0300.
#include "scratch_process_fixture.hpp"
#include <pineforge/detail/scratch_file.hpp>

namespace {
using namespace s4e_tmp;
namespace pd = pineforge::detail;

struct ProbeDirectory {
    std::string path, disk_link, memory_link, search_only;
    ProbeDirectory() {
        require_filesystems();
        char name[] = "/var/tmp/s4e-probe-links-XXXXXX";
        require(::mkdtemp(name) != nullptr, "cannot create symlink-probe fixture");
        try {
            path = name;
            disk_link = path + "/disk-link";
            memory_link = path + "/memory-link";
            search_only = path + "/mode0300";
            std::printf("S4E owned inspection paths: %s -> %s, %s -> /dev/shm, %s (0300)\n",
                        disk_link.c_str(), path.c_str(), memory_link.c_str(), search_only.c_str());
            require(::symlink(path.c_str(), disk_link.c_str()) == 0,
                    "cannot create disk-directory symlink");
            require(::symlink("/dev/shm", memory_link.c_str()) == 0,
                    "cannot create tmpfs-directory symlink");
            require(::mkdir(search_only.c_str(), 0700) == 0
                        && ::chmod(search_only.c_str(), 0300) == 0,
                    "cannot create write-and-search-only directory");
        } catch (...) {
            cleanup();
            ::rmdir(name);
            throw;
        }
    }
    ~ProbeDirectory() { cleanup(); }
    void cleanup() noexcept {
        if (!search_only.empty()) {
            ::chmod(search_only.c_str(), 0700);
            ::rmdir(search_only.c_str());
        }
        if (!disk_link.empty()) ::unlink(disk_link.c_str());
        if (!memory_link.empty()) ::unlink(memory_link.c_str());
        if (!path.empty()) ::rmdir(path.c_str());
    }
};

void selected_default(const std::string& path) {
    Environment temporary("TMPDIR", path.c_str());
    std::string refusal;
    int error = 0;
    bool proven_memory = false;
    require(pd::resolve_scratch_directory("", refusal, error, proven_memory) == path
                && error == 0 && refusal.empty() && !proven_memory,
            "usable TMPDIR must be selected byte-for-byte without fallback");
    pd::ScratchFile file;
    file.bound_cache();
    file.open("pineforge-symlink-search");
    require(file.opened(), "selected TMPDIR must permit a real scratch create");
    exercise_command_scratch(pd::ScratchBackingKind::disk);
}

void cells() {
    PlainEnvironment environment;
    ProbeDirectory directory;
    selected_default(directory.disk_link);
    errno = 0;
    require(::access(directory.search_only.c_str(), R_OK) != 0 && errno == EACCES,
            "0300 fixture must reject read access under the cell's credentials");
    selected_default(directory.search_only);
    std::string refusal;
    int error = 0;
    bool proven_memory = false;
    require(pd::resolve_scratch_directory(directory.memory_link, refusal, error, proven_memory)
                .empty()
                && error == ENODEV && proven_memory
                && refusal.find("explicit directory '" + directory.memory_link + "': tmpfs")
                    != std::string::npos,
            "an explicit symlink to tmpfs must be refused by its target's filesystem");
    pd::ScratchFile file;
    file.set_directory(directory.memory_link);
    bool refused = false;
    try {
        file.open("pineforge-explicit-memory-link");
    } catch (const pd::ReportScratchIoError& failure) {
        require(failure.code() == std::error_code(ENODEV, std::generic_category())
                    && std::string(failure.what()).find(directory.memory_link) != std::string::npos,
                "real explicit symlink refusal must preserve ENODEV and the supplied link");
        refused = true;
    }
    require(refused, "explicit memory symlink unexpectedly allowed a scratch create");
}

void row() {
    scratch_unprivileged("symlink and search-only scratch", cells);
}
} // namespace

int main() {
    return s4e_tmp::test_main("test_scratch_probe_symlink_and_search_only", row);
}
