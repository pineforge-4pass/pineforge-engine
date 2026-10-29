// A real source strategy exercises the production byte budget with entries
// and a global exit. Only the I/O control faults a real pwrite.
#include "command_scratch_arena_fixture.hpp"

#include <pineforge/source/pine_strategy_host.hpp>
#include <pineforge/pineforge.h>
#include <pineforge/run_failure.hpp>

#include <charconv>
#include <cstring>
#include <limits>
#include <string>
#include <system_error>

namespace byte_cap_test {
bool fail_write = false;
std::uint64_t injected_writes = 0;
std::uint64_t command_writes = 0;
std::size_t source_page_size = 0;

bool command_file(int fd) noexcept {
    const int saved = errno;
    char path[64]{}, target[4096]{};
    const int length = std::snprintf(path, sizeof path, "/proc/self/fd/%d", fd);
    const auto bytes = length > 0 && static_cast<std::size_t>(length) < sizeof path
        ? ::readlink(path, target, sizeof target - 1) : -1;
    const bool command = bytes > 0
        && std::strstr(target, "/pineforge-command-") != nullptr;
    errno = saved;
    return command;
}
} // namespace byte_cap_test

extern "C" ssize_t __real_pwrite(int, const void*, std::size_t, off_t);
extern "C" ssize_t __wrap_pwrite(int fd, const void* data, std::size_t size, off_t offset) {
    using namespace byte_cap_test;
    if (command_file(fd)) {
        ++command_writes;
        // Native journal pages are 512 bytes; the source run owner uses the
        // platform page size. Target its real exit-scratch write.
        if (fail_write && size == source_page_size) {
            fail_write = false;
            ++injected_writes;
            errno = EIO;
            return -1;
        }
    }
    return __real_pwrite(fd, data, size, offset);
}

namespace {
using namespace pineforge;
namespace pd = pineforge::detail;
using command_arena_test::require;

class Environment {
public:
    Environment(const char* name, const char* value)
        : name_(name), present_(std::getenv(name) != nullptr),
          previous_(present_ ? std::getenv(name) : "") {
        require((value ? ::setenv(name, value, 1) : ::unsetenv(name)) == 0,
                "configure command scratch test environment");
    }
    ~Environment() {
        if (present_) ::setenv(name_, previous_.c_str(), 1);
        else ::unsetenv(name_);
    }
private:
    const char* name_;
    bool present_;
    std::string previous_;
};

class EnvironmentBudget {
public:
    EnvironmentBudget()
        : previous_(pd::set_command_scratch_byte_cap(pd::CommandScratchByteCap{})) {}
    ~EnvironmentBudget() { pd::set_command_scratch_byte_cap(std::move(previous_)); }
private:
    pd::CommandScratchByteCap previous_;
};

class Directory {
public:
    std::string path = "./command-byte-cap-XXXXXX";
    Directory() { require(::mkdtemp(path.data()) != nullptr, "create disk scratch directory"); }
    ~Directory() { ::rmdir(path.c_str()); }
};

struct Writes {
    std::uint64_t successful = 0, failed = 0;
    static void charge(void* context, const pd::ScratchIoCharge& io) noexcept {
        if (io.operation != pd::ScratchIoOperation::write_page) return;
        auto& self = *static_cast<Writes*>(context);
        if (io.status == pd::ScratchStatus::ok) ++self.successful;
        if (io.status == pd::ScratchStatus::io_error) ++self.failed;
    }
};

// A global relative exit collects open entries in production source scratch
// before it submits the child brackets. No fixture replaces that store.
class CommandStrategy final : public source::PineStrategyHost {
public:
    explicit CommandStrategy(std::size_t entries = 2, bool inject_write = false)
        : entries_(entries), inject_write_(inject_write) {
        source::PineStrategyConfig config;
        config.initial_capital = 1000000.0;
        config.default_qty_type = static_cast<int>(QtyType::FIXED);
        config.default_qty_value = 1.0;
        config.pyramiding = static_cast<int>(entries);
        config.margin_long = config.margin_short = 0.0;
        config.process_orders_on_close = false;
        config.calc_on_order_fills = false;
        config.close_entries_rule_any = false;
        configure_pine_strategy(config);
        set_syminfo_timezone("UTC");
        set_syminfo_session("24x7");
        set_syminfo_mintick(0.25);
    }
    std::size_t callbacks = 0, attempted = 0, accepted = 0;
    bool exit_started = false, exit_returned = false;
    void on_source_bar(const Bar&) override {
        ++callbacks;
        const auto na = std::numeric_limits<double>::quiet_NaN();
        if (pine_bar_index() == 0) {
            for (std::size_t index = 0; index < entries_; ++index) {
                ++attempted;
                strategy_entry("entry-" + std::to_string(index), true, na, na, 1.0);
                ++accepted;
            }
        } else if (pine_bar_index() == 1) {
            require(physical_position().signed_units == static_cast<double>(entries_),
                    "real entry orders must fill before the global exit");
            exit_started = true;
            ++attempted;
            if (inject_write_) byte_cap_test::fail_write = true;
            strategy_exit("exit", "", na, na, na, na, na, 100.0, {}, na, {}, 10000.0, na);
            exit_returned = true;
        }
    }
private:
    std::size_t entries_;
    bool inject_write_;
};

void run(CommandStrategy& host) {
    const std::array<Bar, 3> bars{{
        {100.0, 101.0, 99.0, 100.0, 1.0, INT64_C(1704067200000)},
        {100.0, 101.0, 99.0, 100.0, 1.0, INT64_C(1704067260000)},
        {100.0, 101.0, 99.0, 100.0, 1.0, INT64_C(1704067320000)},
    }};
    host.run(bars.data(), static_cast<int>(bars.size()), "1", "1", false, 4,
             MagnifierDistribution::ENDPOINTS);
}

pf_strategy_t handle(CommandStrategy& host) { return static_cast<BacktestEngine*>(&host); }

void require_failed(CommandStrategy& host, RunFailureCode code) {
    require(run_failure_value_of(host).code == code,
            "public run failure must have the expected typed code");
    require(std::strcmp(strategy_get_last_error_code(handle(host)),
                        run_failure_code_name(code)) == 0,
            "C ABI must retain the same typed code");
    require(host.last_run_status() == 1 && strategy_last_run_status(handle(host)) == 1
            && host.native_state().kind != NativeLifecycleKind::Completed
            && strategy_get_last_error(handle(host))[0] != '\0',
            "scratch refusal must use the public failed-run path");
}

std::uint64_t require_capacity_args(const char* args, std::uint64_t configured_cap) {
    const std::string text(args ? args : "");
    const auto prefix = "{\"cap\":" + std::to_string(configured_cap) + ",\"needed\":";
    const std::string suffix = ",\"resource\":\"command_bytes\"}";
    require(text.size() > prefix.size() + suffix.size()
            && text.compare(0, prefix.size(), prefix) == 0
            && text.compare(text.size() - suffix.size(), suffix.size(), suffix) == 0,
            "capacity arguments must carry the exact configured cap and command_bytes");
    std::uint64_t needed = 0;
    const auto* begin = text.data() + prefix.size();
    const auto* end = text.data() + text.size() - suffix.size();
    const auto parsed = std::from_chars(begin, end, needed);
    require(parsed.ec == std::errc{} && parsed.ptr == end && needed > configured_cap,
            "needed must be an integer byte count greater than the configured cap");
    return needed;
}

void capacity_run(std::uint64_t cap) {
    const auto selected = std::to_string(cap);
    Environment environment("PINEFORGE_COMMAND_SCRATCH_MAX_BYTES", selected.c_str());
    EnvironmentBudget budget;
    Writes writes;
    command_arena_test::ObserverRegistration observer(pd::ScratchIoObserver{&writes, &Writes::charge});
    CommandStrategy host;
    run(host);
    std::fprintf(stderr, "command byte cap=%llu status=%d code=%s args=%s accepted=%zu\n",
        static_cast<unsigned long long>(cap), host.last_run_status(),
        strategy_get_last_error_code(handle(host)), strategy_get_last_error_args(handle(host)),
        host.accepted);
    // The first behavioral assertion on the unchanged tree reports
    // scratch_io_failed here, rather than scratch_capacity_exceeded.
    require_failed(host, RunFailureCode::scratch_capacity_exceeded);
    require_capacity_args(strategy_get_last_error_args(handle(host)), cap);
    require(std::strcmp(run_failure_args_of(host),
                        strategy_get_last_error_args(handle(host))) == 0,
            "native and C public result arguments must agree");
    require(host.callbacks > 0 && host.attempted > 0
            && writes.successful > 0 && writes.failed > 0,
            "real source strategy commands must reach the production byte refusal");
    require(byte_cap_test::injected_writes == 0,
            "the capacity row must never inject a backing failure");
}

void backing_refusal(std::size_t resident_bytes) {
    constexpr std::size_t page = 512;
    const std::uint64_t cap = 2 * page;
    const auto selected = std::to_string(cap);
    Environment environment("PINEFORGE_COMMAND_SCRATCH_MAX_BYTES", selected.c_str());
    EnvironmentBudget budget;
    auto first = pd::make_command_scratch_backing(page, resident_bytes);
    auto second = pd::make_command_scratch_backing(page, resident_bytes);
    std::array<std::byte, page> image{}, actual{};
    image.fill(std::byte{0x39});
    require(first->write_page(0, {image.data(), image.size()}) == pd::ScratchStatus::ok
            && second->write_page(0, {image.data(), image.size()}) == pd::ScratchStatus::ok,
            "two live production stores fill exactly the shared cap");
    const auto writes_before = byte_cap_test::command_writes;
    const auto arena_before = pd::ScratchArena::attached()
        ? pd::ScratchArena::attached()->stats() : pd::ScratchArenaStats{};
    require(first->write_page(1, {image.data(), image.size()}) == pd::ScratchStatus::io_error,
            "the next real extent must cross the shared byte cap");
    const auto* failure = first->carried_failure();
    require(failure && failure->run_failure_code() == RunFailureCode::scratch_capacity_exceeded
            && failure->run_failure().args,
            "backing refusal must carry the capacity verdict before cleanup");
    require(require_capacity_args(failure->run_failure().args->c_str(), cap) == 3 * page,
            "needed must include both stores, not only the extending store");
    require(first->page_count() == 1 && second->page_count() == 1
            && byte_cap_test::command_writes == writes_before,
            "byte refusal must precede physical writes and extent changes");
    if (const auto* arena = pd::ScratchArena::attached()) {
        const auto after = arena->stats();
        require(after.bytes_used == arena_before.bytes_used
                && after.high_water == arena_before.high_water,
                "byte refusal must precede arena claims");
    }
    require(first->read_page(0, {actual.data(), actual.size()}) == pd::ScratchStatus::ok
            && actual == image, "the refused extension must preserve existing bytes");
    second.reset();
    require(first->write_page(1, {image.data(), image.size()}) == pd::ScratchStatus::ok
            && first->page_count() == 2 && first->carried_failure() == nullptr,
            "destroying a sibling releases its charge and clears the old verdict on success");
}

void io_control_and_success() {
    Environment environment("PINEFORGE_COMMAND_SCRATCH_MAX_BYTES", nullptr);
    EnvironmentBudget budget;
    {
        CommandStrategy host(512, true);
        run(host);
        require(byte_cap_test::injected_writes == 1 && !byte_cap_test::fail_write,
                "the I/O control must reach a real command-file pwrite");
        require_failed(host, RunFailureCode::scratch_io_failed);
        require(std::strcmp(strategy_get_last_error_args(handle(host)), "{}") == 0,
                "a genuine write error must keep the existing empty I/O arguments");
        require(host.exit_started && !host.exit_returned && host.accepted == 512,
                "the source write fault must follow the real entry fills");
    }
    {
        CommandStrategy host(512);
        run(host);
        require(host.native_state().kind == NativeLifecycleKind::Completed
                && host.last_run_status() == 0 && host.last_error().empty()
                && strategy_get_last_error_code(handle(host))[0] == '\0'
                && host.accepted == 512 && host.callbacks == 3 && host.exit_returned,
                "the same uncapped workload must complete with the write fault disabled");
    }
}

void unsigned_argument_transport() {
    const auto cap = static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()) + 1;
    const auto needed = cap + 1;
    const auto failure = make_run_failure(RunFailureCode::scratch_capacity_exceeded,
        {{"resource", "command_bytes"}, {"cap", cap}, {"needed", needed}});
    require(failure.code == RunFailureCode::scratch_capacity_exceeded && failure.args,
            "full uint64 byte arguments must pass catalog validation");
    require(require_capacity_args(failure.args->c_str(), cap) == needed,
            "uint64 byte arguments must serialize exactly without narrowing");
    const auto legacy = make_run_failure(RunFailureCode::scratch_capacity_exceeded,
        {{"resource", "layout"}});
    require(legacy.code == RunFailureCode::scratch_capacity_exceeded && legacy.args
            && *legacy.args == "{\"resource\":\"layout\"}",
            "existing capacity raisers must keep their original argument object");
}
} // namespace

int main() {
    try {
        require(pd::ScratchArena::attached() == nullptr, "start without an attached arena");
        const auto page_size = ::sysconf(_SC_PAGESIZE);
        require(page_size > 512, "source and native command stores have distinct page geometry");
        byte_cap_test::source_page_size = static_cast<std::size_t>(page_size);
        const auto cap_unit = 6 * static_cast<std::uint64_t>(page_size);
        Directory directory;
        Environment selected("PINEFORGE_COMMAND_SCRATCH_DIR", directory.path.c_str());
        for (const auto cap : {cap_unit - 1, cap_unit + 17}) capacity_run(cap);
        backing_refusal(64 * 1024);
        backing_refusal(0);
        {
            command_arena_test::ArenaAttachment arena(32 * pd::ScratchArena::kChunkBytes);
            for (const auto cap : {cap_unit - 1, cap_unit + 17}) capacity_run(cap);
            backing_refusal(0);
        }
        io_control_and_success();
        unsigned_argument_transport();
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "FAIL command scratch byte capacity: %s\n", error.what());
        return 1;
    }
}
