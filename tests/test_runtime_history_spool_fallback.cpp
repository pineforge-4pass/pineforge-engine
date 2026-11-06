// The file that holds the runtime history once it no longer fits in memory is
// an unnamed file where the filesystem has them. Where it has none (some
// overlay and network filesystems answer EOPNOTSUPP, older kernels EISDIR or
// EINVAL) the history must go to a named file that is removed at once; every
// other refusal must still be raised. The refusals are forced through the hook
// of src/runtime_context.cpp, which exists in builds without NDEBUG, so this is
// a Debug-build row.
#include <pineforge/runtime_context.hpp>

#include "../src/runtime_context_test_hook.hpp"

#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <dirent.h>
#include <exception>
#include <memory>
#include <optional>
#include <string>
#include <sys/stat.h>
#include <system_error>
#include <unistd.h>

namespace {
using namespace pineforge;
int failures = 0;
#define CHECK(value) do { if (!(value)) { \
    std::fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #value); ++failures; \
} } while (false)

int hook_calls = 0;
int hook_errno = 0;
int refuse_unnamed_open(const char*) {
    ++hook_calls;
    errno = hook_errno;
    return -1;
}
// Installs the refusal for its scope and restores what was installed before.
struct Refusal {
    detail::RuntimeHistoryUnnamedOpen previous;
    explicit Refusal(int error) {
        hook_calls = 0;
        hook_errno = error;
        previous = detail::exchange_runtime_history_unnamed_open(&refuse_unnamed_open);
    }
    ~Refusal() { detail::exchange_runtime_history_unnamed_open(previous); }
};

Bar bar(std::uint64_t i) {
    const double price = static_cast<double>(i + 1);
    return {price, price + 2, price - 1, price, 10, static_cast<std::int64_t>(i) * 60000};
}

struct Sum : RuntimeContextEvaluator {
    double total = 0;
    std::uint64_t calls = 0;
    void consume(RuntimeContextService&, RuntimeContextHandle,
                 const RuntimeContextEvent& event) override {
        total += event.bar.close;
        ++calls;
    }
    std::uint64_t storage_bytes() const noexcept override {
        OwnedStorage out;
        observe_owned_storage(out);
        return out.usage().storage_bytes();
    }
    void observe_owned_storage(OwnedStorage& out) const noexcept override {
        out.metadata(sizeof(*this));
    }
};

std::string make_directory() {
    char name[] = "pineforge-spool-fallback-XXXXXX";
    if (!::mkdtemp(name)) throw std::system_error(errno, std::generic_category(), "mkdtemp");
    return name;
}
std::size_t entries_in(const std::string& directory) {
    std::size_t count = 0;
    DIR* handle = ::opendir(directory.c_str());
    if (!handle) throw std::system_error(errno, std::generic_category(), "opendir");
    while (const dirent* entry = ::readdir(handle)) {
        const std::string name = entry->d_name;
        if (name != "." && name != "..") ++count;
    }
    ::closedir(handle);
    return count;
}

// Direct spill (no resident window): the first bar creates the file.
void configure_direct_spill(RuntimeContextService& service, const std::string& directory) {
    service.configure({0, std::nullopt, directory});
    service.enable_history();
    service.begin_live();
}

void unsupported_filesystem_uses_a_removed_named_file() {
    for (const int refusal : {EOPNOTSUPP, EISDIR, EINVAL}) {
        const auto directory = make_directory();
        {
            Refusal refuse(refusal);
            RuntimeContextService service;
            configure_direct_spill(service, directory);
            for (std::uint64_t i = 0; i < 5; ++i) service.append_live(bar(i));
            const auto handle = service.get_or_create(
                {1, "spool fallback", {}}, [] { return std::make_unique<Sum>(); });
            service.advance(handle, 5);
            const auto& sum = dynamic_cast<Sum&>(service.published(handle));
            CHECK(hook_calls == 1);
            CHECK(service.usage().spool_bytes == 5 * sizeof(Bar));
            CHECK(sum.calls == 5 && sum.total == 15);
            // Nothing is left under a name: the file was removed as soon as it was open.
            CHECK(entries_in(directory) == 0);
        }
        ::rmdir(directory.c_str());
    }
}

void every_other_refusal_is_raised() {
    for (const int refusal : {EACCES, ENOSPC, EMFILE, ENOENT}) {
        const auto directory = make_directory();
        {
            Refusal refuse(refusal);
            RuntimeContextService service;
            configure_direct_spill(service, directory);
            bool raised = false;
            try {
                service.append_live(bar(0));
            } catch (const std::system_error& error) {
                raised = error.code().value() == refusal
                    && std::string(error.what()).find("runtime history create spool")
                        != std::string::npos;
            }
            CHECK(raised);
            CHECK(hook_calls == 1);
            CHECK(entries_in(directory) == 0); // no fallback file was made
        }
        ::rmdir(directory.c_str());
    }
}

template<class F>
void run(const char* name, F&& body) {
    try {
        body();
    } catch (const std::exception& error) {
        std::fprintf(stderr, "%s: raised %s\n", name, error.what());
        ++failures;
    }
}
}  // namespace

int main() {
    run("unsupported_filesystem_uses_a_removed_named_file", unsupported_filesystem_uses_a_removed_named_file);
    run("every_other_refusal_is_raised", every_other_refusal_is_raised);
    if (failures != 0) {
        std::fprintf(stderr, "runtime history spool fallback: %d check(s) failed\n", failures);
        return 1;
    }
    std::puts("runtime history spool fallback: ok");
    return 0;
}
