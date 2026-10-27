// Real leaf creates fail with EMFILE after selecting disk-backed /var/tmp.
// Reverting either leaf to its bare create literal must fail this row (m8).
#include "default_scratch_tmpfs_fixture.hpp"
#include <pineforge/detail/scratch_file.hpp>

#include <algorithm>
#include <sys/resource.h>

namespace {
using namespace s4e_tmp;

struct DescriptorPopulation {
    rlim_t count = 0;
    rlim_t range = 0;
};

DescriptorPopulation descriptor_population() {
    std::puts("Inspection path: /proc/self/fd (entry names only; never follow descriptors)");
    DIR* directory = ::opendir("/proc/self/fd");
    require(directory != nullptr, "cannot enumerate current descriptors");
    DescriptorPopulation result;
    int read_error = 0;
    for (;;) {
        errno = 0;
        const auto* entry = ::readdir(directory);
        if (!entry) {
            read_error = errno;
            break;
        }
        if (entry->d_name[0] == '.') continue;
        char* end = nullptr;
        const long number = std::strtol(entry->d_name, &end, 10);
        if (errno != 0 || !end || *end != '\0' || number < 0) {
            read_error = EINVAL;
            break;
        }
        // The enumeration itself adds one fd; it is closed before the limit.
        if (number == ::dirfd(directory)) continue;
        ++result.count;
        result.range = std::max(result.range, static_cast<rlim_t>(number) + 1);
    }
    const int closed = ::closedir(directory);
    require(read_error == 0 && closed == 0, "cannot finish enumerating descriptors");
    return result;
}

struct FilledHoles {
    std::vector<int> descriptors;
    ~FilledHoles() { for (int descriptor : descriptors) ::close(descriptor); }

    rlim_t dense_count() {
        auto population = descriptor_population();
        // A runner may inherit sparse descriptors. Fill lower holes using
        // duplicates of stderr, without following or opening any named file.
        while (population.count < population.range) {
            const int descriptor = ::fcntl(STDERR_FILENO, F_DUPFD_CLOEXEC, 0);
            require(descriptor >= 0, "cannot fill an inherited descriptor hole");
            try { descriptors.push_back(descriptor); }
            catch (...) { ::close(descriptor); throw; }
            population = descriptor_population();
        }
        require(population.count != 0 && population.count == population.range,
                "EMFILE fixture requires a dense descriptor population");
        return population.count;
    }
};

struct DescriptorLimit {
    struct rlimit saved{};
    bool active = false;
    explicit DescriptorLimit(rlim_t count) {
        require(::getrlimit(RLIMIT_NOFILE, &saved) == 0, "cannot read RLIMIT_NOFILE");
        auto limited = saved;
        limited.rlim_cur = count;
        require(::setrlimit(RLIMIT_NOFILE, &limited) == 0, "cannot lower RLIMIT_NOFILE");
        active = true;
    }
    ~DescriptorLimit() { if (active) ::setrlimit(RLIMIT_NOFILE, &saved); }
    void restore() {
        require(::setrlimit(RLIMIT_NOFILE, &saved) == 0, "cannot restore RLIMIT_NOFILE");
        active = false;
    }
};

template<class Error, class Operation>
void expect_create_failure(Operation operation, const char* prefix,
                           pineforge::RunFailureCode code) {
    try {
        operation();
    } catch (const Error& error) {
        const auto failure = pineforge::classify_run_failure(error);
        const std::string text = error.what();
        require(error.code() == std::error_code(EMFILE, std::generic_category())
                    && failure.code == code,
                "real fallback create must fail with EMFILE and its established failure code");
        require(code == pineforge::RunFailureCode::report_sink_failed
                    ? (failure.args && *failure.args == "{\"reason\":\"scratch_io\"}")
                    : !failure.args,
                "real fallback create must retain its failure arguments");
        require(text.find(prefix) == 0
                    && text.find("directory '/var/tmp'") != std::string::npos
                    && text.find("TMPDIR '/dev/shm': tmpfs") != std::string::npos
                    && text.find("set TMPDIR to a disk-backed directory") != std::string::npos,
                std::string("real fallback create lost its selected/rejected directory or remedy: ")
                    + text);
        return;
    }
    throw std::runtime_error("fallback create unexpectedly succeeded despite RLIMIT_NOFILE");
}

void row() {
    PlainEnvironment environment;
    require_filesystems();
    FilledHoles holes;
    const auto count = holes.dense_count();
    DescriptorLimit limit(count);
    for (const bool bounded : {false, true}) {
        pineforge::detail::ScratchFile file;
        if (bounded) file.bound_cache();
        expect_create_failure<pineforge::detail::ReportScratchIoError>(
            [&] { file.open("pineforge-fallback-emfile"); }, "scratch create",
            pineforge::RunFailureCode::report_sink_failed);
    }
    expect_create_failure<std::system_error>(
        [] { (void)pineforge::detail::make_command_scratch_backing(4096); },
        "command scratch create", pineforge::RunFailureCode::scratch_io_failed);
    limit.restore();
}
} // namespace

int main() {
    return s4e_tmp::test_main("test_default_scratch_fallback_create_failure_e2e", row);
}
