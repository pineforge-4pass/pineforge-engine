#include <pineforge/run_sandbox.hpp>

#include <cstdio>
#include <mutex>

namespace pineforge {
namespace {
std::once_flag receipt;
}
int run_sandbox_enter(const pf_run_sandbox_config_v1*) noexcept {
    std::call_once(receipt, [] {
        std::fputs("sandbox: not implemented on this platform; sandbox=unsupported\n", stderr);
    });
    return 0;
}
int run_sandbox_enter_defaults(const char*, const char*, int, int) noexcept {
    return run_sandbox_enter(nullptr);
}
const char* run_sandbox_mode() noexcept { return "unsupported"; }
RunSandboxViolation run_sandbox_violation() noexcept { return {}; }
const char* run_sandbox_class_name(RunSandboxClass kind) noexcept {
    switch (kind) {
    case RunSandboxClass::network: return "network";
    case RunSandboxClass::process: return "process";
    case RunSandboxClass::exec: return "exec";
    case RunSandboxClass::ptrace: return "ptrace";
    case RunSandboxClass::privileged: return "privileged";
    case RunSandboxClass::filesystem: return "filesystem";
    }
    return "privileged";
}
} // namespace pineforge
