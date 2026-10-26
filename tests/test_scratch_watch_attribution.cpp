// RED FIRST against 2561cfb464: these rows use the old fixture's public API.
// Co-tenant creations must neither fail a negative nor satisfy a positive.
#define PINEFORGE_TEST_SCRATCH_ATTRIBUTION 1
#include "default_scratch_tmpfs_fixture.hpp"

#if defined(__linux__)
#include <pineforge/detail/scratch_file.hpp>
#include <sys/wait.h>

namespace {
using namespace s4e_tmp;

class PlantedFile {
public:
    explicit PlantedFile(std::string pattern, bool use_mkstemp = true)
        : path_(std::move(pattern)) {
        std::fprintf(stderr, "attribution control path/template: %s\n", path_.c_str());
        descriptor_.value = use_mkstemp ? ::mkstemp(path_.data())
            : ::open(path_.c_str(), O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
        require(descriptor_.value >= 0, "cannot create the attribution control file");
    }
    ~PlantedFile() {
        if (named_ && ::unlink(path_.c_str()) != 0) {
            std::fprintf(stderr, "cannot clean attribution control: %s\n", path_.c_str());
            std::abort();
        }
    }
    PlantedFile(const PlantedFile&) = delete;
    PlantedFile& operator=(const PlantedFile&) = delete;
    const std::string& path() const { return path_; }
    void remove_and_close() {
        require(::unlink(path_.c_str()) == 0, "cannot unlink the attribution control file");
        named_ = false;
        descriptor_.close();
    }
private:
    std::string path_;
    Fd descriptor_;
    bool named_ = true;
};

// Independent evidence that the co-tenant really produces a shared-directory
// event. It follows no files and only accepts this row's generated name prefix.
class SharedEvent {
public:
    SharedEvent() : descriptor_(::inotify_init1(IN_NONBLOCK | IN_CLOEXEC)) {
        require(descriptor_.value >= 0
                    && ::inotify_add_watch(descriptor_.value, "/tmp", IN_CREATE) >= 0,
                "cannot watch the co-tenant control's /tmp event");
    }
    void require_creation(const std::string& prefix) {
        bool seen = false;
        alignas(inotify_event) std::array<char, 16384> buffer{};
        for (;;) {
            const auto bytes = ::read(descriptor_.value, buffer.data(), buffer.size());
            if (bytes < 0 && errno == EINTR) continue;
            if (bytes < 0 && errno == EAGAIN) break;
            require(bytes > 0, "cannot read the co-tenant control's events");
            std::size_t offset = 0;
            while (offset < static_cast<std::size_t>(bytes)) {
                require(static_cast<std::size_t>(bytes) - offset >= sizeof(inotify_event),
                        "short co-tenant control event");
                const auto* event = reinterpret_cast<const inotify_event*>(buffer.data() + offset);
                require(!(event->mask & (IN_Q_OVERFLOW | IN_IGNORED))
                            && event->len <= static_cast<std::size_t>(bytes) - offset - sizeof(*event),
                        "lost co-tenant control event");
                if ((event->mask & IN_CREATE) && event->len
                    && std::strncmp(event->name, prefix.c_str(), prefix.size()) == 0)
                    seen = true;
                offset += sizeof(*event) + event->len;
            }
        }
        require(seen, "co-tenant control did not produce its real /tmp creation event");
    }
private:
    Fd descriptor_;
};

// No sleeps: the child keeps every planted file named and open until the
// parent's release pipe closes, including when a parent assertion throws.
class CoTenant {
public:
    explicit CoTenant(bool private_directory = false)
        : token_(std::to_string(::getpid())),
          private_directory_(private_directory ? std::getenv("TMPDIR") : "") {
        std::array<int, 2> pair{};
        require(::pipe(pair.data()) == 0, "cannot create the co-tenant ready pipe");
        ready_reader_.value = pair[0];
        ready_writer_.value = pair[1];
        require(::pipe(pair.data()) == 0, "cannot create the co-tenant release pipe");
        release_reader_.value = pair[0];
        release_writer_.value = pair[1];
    }
    ~CoTenant() {
        release_writer_.close();
        if (child_ > 0) {
            int status = 0;
            while (::waitpid(child_, &status, 0) < 0 && errno == EINTR) {}
        }
    }
    CoTenant(const CoTenant&) = delete;
    CoTenant& operator=(const CoTenant&) = delete;
    std::string prefix() const { return "pineforge-cotenant-" + token_ + "-"; }
    void start() {
        child_ = ::fork();
        require(child_ >= 0, "cannot fork the co-tenant control");
        if (child_ == 0) {
            ready_reader_.close();
            release_writer_.close();
            const int result = child_body();
            ::_exit(result);
        }
        ready_writer_.close();
        release_reader_.close();
        char ready = 0;
        ssize_t count;
        do { count = ::read(ready_reader_.value, &ready, 1); } while (count < 0 && errno == EINTR);
        require(count == 1 && ready == 'R', "co-tenant failed before its files were ready");
    }
    void finish() {
        release_writer_.close();
        int status = 0;
        pid_t result;
        do { result = ::waitpid(child_, &status, 0); } while (result < 0 && errno == EINTR);
        require(result == child_, "cannot reap the co-tenant control");
        child_ = -1;
        require(WIFEXITED(status) && WEXITSTATUS(status) == 0,
                "co-tenant control failed or crashed");
    }
private:
    void wait_for_release() {
        const char ready = 'R';
        ssize_t count;
        do { count = ::write(ready_writer_.value, &ready, 1); } while (count < 0 && errno == EINTR);
        require(count == 1, "cannot signal the co-tenant's ready files");
        ready_writer_.close();
        char release = 0;
        do { count = ::read(release_reader_.value, &release, 1); } while (count < 0 && errno == EINTR);
        require(count == 0, "cannot wait for the co-tenant's release");
    }
    int child_body() {
        try {
            if (!private_directory_.empty()) {
                // A child deliberately using our private directory is part of
                // this row and must be caught even without calling mkstemp.
                PlantedFile private_file(private_directory_ + "/other-child-" + token_, false);
                wait_for_release();
            } else {
                PlantedFile temporary("/tmp/" + prefix() + "XXXXXX");
                PlantedFile unrelated("/tmp/other-" + token_ + "-XXXXXX");
                PlantedFile memory("/dev/shm/" + prefix() + "XXXXXX");
                PlantedFile disk("/var/tmp/" + prefix() + "XXXXXX");
                wait_for_release();
            }
            return 0;
        } catch (const std::exception& error) {
            std::fprintf(stderr, "co-tenant control: %s\n", error.what());
            return 1;
        }
    }
    std::string token_, private_directory_;
    Fd ready_reader_, ready_writer_, release_reader_, release_writer_;
    pid_t child_ = -1;
};

template<class Check>
void require_rejection(Check check, const std::string& expected) {
    bool rejected = false;
    try { check(); }
    catch (const std::runtime_error& error) {
        require(error.what() == expected, "negative control failed for an unrelated reason");
        rejected = true;
    }
    require(rejected, "attribution policy accepted a planted own-process violation");
}

void cotenant_default() {
    require_filesystems();
    PlainEnvironment environment;
    CreationWatch watch;
    SharedEvent events;
    CoTenant child;
    child.start();
    events.require_creation(child.prefix());
    Host host;
    run(host);
    require_success(host);
    Report report(host);
    watch.collect();
    // Main lstat()s the child's still-named files here and throws.
    watch.require_removed();
    require(!watch.created_in("/dev/shm") && !watch.created_in("/tmp"),
            "co-tenant files were attributed to the default run");
    require(watch.created_in("/var/tmp"), "real native scratch lacks its own disk witness");
    child.finish();
}

void cotenant_jailed() {
    require_filesystems();
    PlainEnvironment environment;
    CreationWatch watch;
    SharedEvent events;
    CoTenant child;
    child.start();
    events.require_creation(child.prefix());
    watch.collect();
    require(watch.empty(), "co-tenant events failed the jailed no-creation policy");
    child.finish();
}

void cotenant_positive() {
    require_filesystems();
    PlainEnvironment environment;
    CreationWatch watch;
    SharedEvent events;
    CoTenant child;
    child.start();
    events.require_creation(child.prefix());
    watch.collect();
    require(!watch.created_in("/var/tmp"),
            "a co-tenant supplied the default run's positive /var/tmp witness");
    PlantedFile own("/var/tmp/pineforge-command-XXXXXX");
    own.remove_and_close(); // Even a file closed before collect() must count.
    watch.collect();
    require(watch.created_in("/var/tmp"), "own /var/tmp descriptor was not attributed");
    watch.require_removed();
    child.finish();
}

void own_tmp() {
    require_filesystems();
    PlainEnvironment environment;
    CreationWatch watch;
    PlantedFile own("/tmp/pineforge-command-XXXXXX");
    watch.collect();
    require(watch.created_in("/tmp"), "own /tmp descriptor was not attributed");
    const std::string message = "TMPDIR on tmpfs must resolve directly to /var/tmp";
    require_rejection([&] {
        require(!watch.created_in("/dev/shm") && !watch.created_in("/tmp"), message);
    }, message);
    require_rejection([&] { watch.require_removed(); },
                      "run left a named scratch file: " + own.path());
    own.remove_and_close();
    watch.collect();
    watch.require_removed();
}

void own_transient_tmp() {
    require_filesystems();
    PlainEnvironment environment;
    CreationWatch watch;
    PlantedFile own("/tmp/pineforge-command-XXXXXX");
    own.remove_and_close();
    watch.collect();
    require(watch.created_in("/tmp"), "own transient /tmp file escaped attribution");
    const std::string message = "TMPDIR on tmpfs must resolve directly to /var/tmp";
    require_rejection([&] {
        require(!watch.created_in("/dev/shm") && !watch.created_in("/tmp"), message);
    }, message);
    watch.require_removed();
}

void own_other_tmp() {
    require_filesystems();
    PlainEnvironment environment;
    CreationWatch watch;
    PlantedFile own("/tmp/other-XXXXXX");
    own.remove_and_close();
    watch.collect();
    const std::string message = "jailed run created a scratch file under TMPDIR, /tmp or /var/tmp";
    require_rejection([&] { require(watch.empty(), message); }, message);
    watch.require_removed();
}

void own_shm() {
    require_filesystems();
    PlainEnvironment environment;
    CreationWatch watch;
    PlantedFile own(std::string(std::getenv("TMPDIR")) + "/pineforge-command-XXXXXX");
    own.remove_and_close();
    watch.collect();
    require(watch.created_in("/dev/shm"), "own tmpfs creation was not attributed");
    const std::string message = "TMPDIR on tmpfs must resolve directly to /var/tmp";
    require_rejection([&] {
        require(!watch.created_in("/dev/shm") && !watch.created_in("/tmp"), message);
    }, message);
    watch.require_removed();
}

void private_non_mkstemp() {
    require_filesystems();
    PlainEnvironment environment;
    CreationWatch watch;
    CoTenant child(true);
    child.start();
    watch.collect();
    const std::string message = "jailed run created a scratch file under TMPDIR, /tmp or /var/tmp";
    require_rejection([&] { require(watch.empty(), message); }, message);
    child.finish();
}

void header_scratch() {
    require_filesystems();
    PlainEnvironment environment;
    CreationWatch watch;
    pineforge::detail::ScratchFile file;
    file.set_directory("/var/tmp");
    file.open("pineforge-header-control");
    watch.collect();
    require(watch.created_in("/var/tmp"),
            "header-inline scratch did not reach the executable's mkstemp witness");
    watch.require_removed();
    file.clear();
}

void rows() {
    cotenant_default();
    cotenant_jailed();
    cotenant_positive();
    own_tmp();
    own_transient_tmp();
    own_other_tmp();
    own_shm();
    private_non_mkstemp();
    header_scratch();
}
} // namespace

int main(int argc, char** argv) {
    if (argc == 1) return s4e_tmp::test_main("test_scratch_watch_attribution", rows);
    if (argc == 2) {
        const std::pair<const char*, void (*)()> cases[] = {
            {"cotenant_default", cotenant_default}, {"cotenant_jailed", cotenant_jailed},
            {"cotenant_positive", cotenant_positive}, {"own_tmp", own_tmp},
            {"own_transient_tmp", own_transient_tmp}, {"own_other_tmp", own_other_tmp},
            {"own_shm", own_shm}, {"private_non_mkstemp", private_non_mkstemp},
            {"header_scratch", header_scratch}
        };
        for (const auto& entry : cases)
            if (entry.first == std::string(argv[1])) return s4e_tmp::test_main(entry.first, entry.second);
    }
    std::fputs("expected one scratch attribution row name; no file arguments accepted\n", stderr);
    return 1;
}
#endif // __linux__
