#pragma once

#include "scratch_process_fixture.hpp"
#include <pineforge/detail/scratch_file.hpp>

namespace s4e_tmp {

// Own every mutable fixture; never change permissions on a supplied directory.
struct InvalidDirectories {
    std::string disk, memory;

private:
    struct Entries {
        std::string regular, mode0000;
        bool regular_created = false, mode_created = false;
    };
    std::array<Entries, 2> entries_{};

    static void cleanup(const char* root, bool root_created, const Entries& entries) noexcept {
        if (entries.mode_created) {
            ::chmod(entries.mode0000.c_str(), 0700);
            ::rmdir(entries.mode0000.c_str());
        }
        if (entries.regular_created) ::unlink(entries.regular.c_str());
        if (root_created) ::rmdir(root);
    }

public:
    InvalidDirectories() {
        require_filesystems();
        char disk_name[] = "/var/tmp/s4e-invalid-directory-XXXXXX";
        char memory_name[] = "/dev/shm/s4e-invalid-directory-XXXXXX";
        bool disk_created = false, memory_created = false;
        try {
            disk_created = ::mkdtemp(disk_name) != nullptr;
            require(disk_created, "cannot create disk-backed directory fixture");
            disk = disk_name;
            memory_created = ::mkdtemp(memory_name) != nullptr;
            require(memory_created, "cannot create tmpfs directory fixture");
            memory = memory_name;
            std::printf("Owned inspection paths: /dev/null, %s, %s\n",
                        disk.c_str(), memory.c_str());
            const std::array<const std::string*, 2> roots{{&disk, &memory}};
            for (std::size_t i = 0; i < roots.size(); ++i) {
                const auto& root = *roots[i];
                auto& entries = entries_[i];
                entries.regular = root + "/regular";
                entries.mode0000 = root + "/mode0000";
                Fd regular(::open(entries.regular.c_str(), O_CREAT | O_EXCL | O_RDWR, 0600));
                entries.regular_created = regular.value >= 0;
                require(entries.regular_created, "cannot create regular-file candidate");
                entries.mode_created = ::mkdir(entries.mode0000.c_str(), 0) == 0;
                require(entries.mode_created, "cannot create non-searchable candidate");
                // A dropped child must reach the target: only mode0000,
                // rather than its mkdtemp parent, should deny search.
                if (::geteuid() == 0)
                    require(::chmod(root.c_str(), 0711) == 0,
                            "cannot make owned fixture parent searchable by the dropped child");
            }
        } catch (...) {
            // The destructor is not called if construction fails. The raw
            // mkdtemp names also cover a failed std::string assignment.
            cleanup(disk_name, disk_created, entries_[0]);
            cleanup(memory_name, memory_created, entries_[1]);
            throw;
        }
    }
    ~InvalidDirectories() {
        cleanup(disk.c_str(), true, entries_[0]);
        cleanup(memory.c_str(), true, entries_[1]);
    }
    template<class Check>
    void each(Check check) const {
        check("/dev/null", ENOTDIR);
        for (const auto& root : {disk, memory}) check(root + "/regular", ENOTDIR);
        scratch_unprivileged("mode-0000 scratch cells", [&] {
            std::printf("Mode-0000 cells: effective uid %lu\n",
                        static_cast<unsigned long>(::geteuid()));
            for (const auto& root : {disk, memory}) check(root + "/mode0000", EACCES);
        });
    }
};

template<class Operation>
void expect_directory_error(Operation operation, int number,
                            pineforge::RunFailureCode code, bool report_type) {
    try {
        operation();
    } catch (const std::system_error& error) {
        require(error.code() == std::error_code(number, std::generic_category()),
                "invalid directory must retain its exact create errno");
        require(pineforge::classify_run_failure(error).code == code,
                "invalid directory must retain its failure code");
        if (report_type) {
            require(dynamic_cast<const pineforge::detail::ReportScratchIoError*>(&error) != nullptr,
                    "report scratch must retain its distinct exception type");
            const auto failure = pineforge::classify_run_failure(error);
            require(failure.args && *failure.args == "{\"reason\":\"scratch_io\"}",
                    "report scratch must retain the scratch_io reason");
        }
        return;
    }
    throw std::runtime_error("invalid directory unexpectedly succeeded or fell back");
}

} // namespace s4e_tmp
