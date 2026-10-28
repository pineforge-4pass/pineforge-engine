#pragma once

#include <pineforge/pineforge.h>

namespace pineforge {

// Process protection/diagnostics, not broker state: never enters a trades hash.
enum class RunSandboxClass : unsigned {
    network = 1, process, exec, ptrace, privileged, filesystem
};
struct RunSandboxViolation {
    int syscall_number = -1;
    RunSandboxClass kind = RunSandboxClass::privileged;
};

int run_sandbox_enter(const pf_run_sandbox_config_v1* config) noexcept;
// Native fallback uses the same TMPDIR/disk-fallback choice as scratch storage.
int run_sandbox_enter_defaults(const char* scratch_dir, const char* sink_path,
                               int sink_fd, int arena_fd) noexcept;
const char* run_sandbox_mode() noexcept;
RunSandboxViolation run_sandbox_violation() noexcept;
const char* run_sandbox_class_name(RunSandboxClass kind) noexcept;

} // namespace pineforge
