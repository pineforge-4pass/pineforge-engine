#pragma once

#include "default_scratch_tmpfs_fixture.hpp"
#include <pineforge/detail/scratch_file.hpp>

namespace s4e_tmp {

// Own every mutable fixture; never change permissions on a supplied directory.
struct InvalidDirectories {
    std::string disk, memory;

    InvalidDirectories() {
        require_filesystems();
        char disk_name[] = "/var/tmp/s4e-invalid-directory-XXXXXX";
        char memory_name[] = "/dev/shm/s4e-invalid-directory-XXXXXX";
        const char* created = ::mkdtemp(disk_name);
        require(created != nullptr, "cannot create disk-backed directory fixture");
        disk = created;
        created = ::mkdtemp(memory_name);
        require(created != nullptr, "cannot create tmpfs directory fixture");
        memory = created;
        std::printf("S4E owned inspection paths: /dev/null, %s, %s\n",
                    disk.c_str(), memory.c_str());
        for (const auto& root : {disk, memory}) {
            Fd regular(::open((root + "/regular").c_str(), O_CREAT | O_EXCL | O_RDWR, 0600));
            require(regular.value >= 0, "cannot create regular-file candidate");
            require(::mkdir((root + "/mode0000").c_str(), 0) == 0,
                    "cannot create non-searchable candidate");
        }
    }
    ~InvalidDirectories() {
        for (const auto& root : {disk, memory}) {
            if (root.empty()) continue;
            ::chmod((root + "/mode0000").c_str(), 0700);
            ::rmdir((root + "/mode0000").c_str());
            ::unlink((root + "/regular").c_str());
            ::rmdir(root.c_str());
        }
    }
    template<class Check>
    void each(Check check) const {
        check("/dev/null", ENOTDIR);
        for (const auto& root : {disk, memory}) check(root + "/regular", ENOTDIR);
        if (::geteuid() == 0) {
            std::puts("SKIP EACCES mode-0000 cases only: running as root");
        } else {
            for (const auto& root : {disk, memory}) check(root + "/mode0000", EACCES);
        }
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
