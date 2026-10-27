// Co-tenant controls are RED against the pid-less fixture at 2561cfb464.
// CR-319 adds RED controls at 6b33c47fd for exclusive open, fork descendants,
// and path/PID diagnostics. The controls use the pre-change watch API.
#define PINEFORGE_TEST_SCRATCH_ATTRIBUTION 1
#include "default_scratch_tmpfs_fixture.hpp"

#if defined(__linux__)
#include <pineforge/detail/scratch_file.hpp>
#include <sys/syscall.h>
#include <sys/wait.h>

namespace {
using namespace s4e_tmp;

class PlantedFile {
public:
    explicit PlantedFile(std::string pattern, bool use_mkstemp = true, bool raw_open = false)
        : path_(std::move(pattern)) {
        std::fprintf(stderr, "attribution control path/template: %s\n", path_.c_str());
        constexpr int flags = O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC;
        descriptor_.value = raw_open
            ? static_cast<int>(::syscall(SYS_openat, AT_FDCWD, path_.c_str(), flags, 0600))
            : use_mkstemp ? ::mkstemp(path_.data()) : ::open(path_.c_str(), flags, 0600);
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
    explicit SharedEvent(const char* directory = "/tmp")
        : descriptor_(::inotify_init1(IN_NONBLOCK | IN_CLOEXEC)), directory_(directory) {
        require(descriptor_.value >= 0
                    && ::inotify_add_watch(descriptor_.value, directory, IN_CREATE) >= 0,
                "cannot watch the co-tenant control's event: " + directory_);
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
        require(seen, "co-tenant control did not produce its real creation event: " + directory_);
    }
private:
    Fd descriptor_;
    std::string directory_;
};

// Construct co-tenants BEFORE CreationWatch: fork+exec discards all witness
// state, and a start pipe delays their creations until the watch is armed.
// The explicit fork-descendant control instead constructs this AFTER the watch
// with exec_helper=false, retaining the inherited witness.
// Every planted file stays named and open until release, even on assertion failure.
class CoTenant {
public:
    explicit CoTenant(bool private_directory = false, bool exec_helper = true)
        : token_(std::to_string(::getpid())),
          private_directory_(private_directory ? std::getenv("TMPDIR") : "") {
        std::array<int, 2> pair{};
        require(::pipe(pair.data()) == 0, "cannot create the co-tenant ready pipe");
        ready_reader_.value = pair[0];
        ready_writer_.value = pair[1];
        require(::pipe(pair.data()) == 0, "cannot create the co-tenant release pipe");
        release_reader_.value = pair[0];
        release_writer_.value = pair[1];
        child_ = ::fork();
        require(child_ >= 0, "cannot fork the creator control");
        if (child_ == 0) {
            ready_reader_.close();
            release_writer_.close();
            if (exec_helper) {
                if (::dup2(release_reader_.value, STDIN_FILENO) < 0
                    || ::dup2(ready_writer_.value, STDOUT_FILENO) < 0) ::_exit(1);
                if (release_reader_.value > STDERR_FILENO) release_reader_.close();
                if (ready_writer_.value > STDERR_FILENO) ready_writer_.close();
                // Positional arguments carry paths without shell interpolation.
                // mktemp chooses new names; the trap cleans only those names.
                constexpr const char* script = R"sh(
set -eu
temporary= unrelated= memory= disk= private_file=
trap 'rm -f -- "$temporary" "$unrelated" "$memory" "$disk" "$private_file"' EXIT
read -r start || exit 0
[ "$start" = S ]
if [ -n "$2" ]; then
    candidate="$2/other-child-$1"
    set -C
    exec 4>"$candidate"
    private_file="$candidate"
else
    temporary=$(mktemp "/tmp/$1XXXXXX")
    unrelated=$(mktemp "/tmp/other-$1XXXXXX")
    memory=$(mktemp "/dev/shm/$1XXXXXX")
    disk=$(mktemp "/var/tmp/$1XXXXXX")
    exec 4<>"$temporary" 5<>"$unrelated" 6<>"$memory" 7<>"$disk"
fi
printf R
if read -r release; then exit 1; fi
)sh";
                const auto name_prefix = prefix();
                ::execl("/bin/sh", "sh", "-c", script, "scratch-cotenant",
                        name_prefix.c_str(), private_directory_.c_str(),
                        static_cast<char*>(nullptr));
                ::_exit(1);
            }
            char start = 0, newline = 0;
            ssize_t count;
            do { count = ::read(release_reader_.value, &start, 1); }
                while (count < 0 && errno == EINTR);
            if (count == 0) ::_exit(0); // Parent unwound before arming this control.
            if (count != 1 || start != 'S') ::_exit(1);
            do { count = ::read(release_reader_.value, &newline, 1); }
                while (count < 0 && errno == EINTR);
            if (count != 1 || newline != '\n') ::_exit(1);
            const int result = child_body();
            ::_exit(result);
        }
        ready_writer_.close();
        release_reader_.close();
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
    pid_t pid() const { return child_; }
    void start() {
        ssize_t count;
        do { count = ::write(release_writer_.value, "S\n", 2); } while (count < 0 && errno == EINTR);
        require(count == 2, "cannot start the creator control");
        char ready = 0;
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

template<class Check>
void require_rejection_details(Check check, const std::string& path, const std::string& identity) {
    bool rejected = false;
    try { check(); }
    catch (const std::runtime_error& error) {
        const std::string message = error.what();
        const auto prefix = "run left a named scratch file: " + path;
        require(message.compare(0, prefix.size(), prefix) == 0
                    && message.size() >= identity.size()
                    && message.compare(message.size() - identity.size(), identity.size(), identity) == 0,
                "scratch failure lacks its offending path/creator: " + message);
        rejected = true;
    }
    require(rejected, "scratch leak did not produce a diagnostic");
}

void cotenant_default() {
    require_filesystems();
    PlainEnvironment environment;
    CoTenant child;
    CreationWatch watch;
    SharedEvent events;
    SharedEvent memory_events("/dev/shm");
    child.start();
    events.require_creation(child.prefix());
    memory_events.require_creation(child.prefix());
    Host host;
    run(host);
    require_success(host);
    Report report(host);
    watch.collect();
    // The historical machine-wide watch lstat()s the co-tenant's files and throws.
    watch.require_removed();
    require(!watch.created_in("/dev/shm") && !watch.created_in("/tmp"),
            "co-tenant files were attributed to the default run");
    require(watch.created_in("/var/tmp"), "real native scratch lacks its own disk witness");
    child.finish();
}

void cotenant_jailed() {
    require_filesystems();
    PlainEnvironment environment;
    CoTenant child;
    CreationWatch watch;
    SharedEvent events;
    SharedEvent memory_events("/dev/shm");
    child.start();
    events.require_creation(child.prefix());
    memory_events.require_creation(child.prefix());
    watch.collect();
    require(watch.empty(), "co-tenant events failed the jailed no-creation policy");
    child.finish();
}

void cotenant_positive() {
    require_filesystems();
    PlainEnvironment environment;
    CoTenant child;
    CreationWatch watch;
    SharedEvent events;
    SharedEvent memory_events("/dev/shm");
    child.start();
    events.require_creation(child.prefix());
    memory_events.require_creation(child.prefix());
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
                      "run left a named scratch file: " + own.path()
                          + " pid=" + std::to_string(::getpid()));
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
    CoTenant child(true);
    CreationWatch watch;
    child.start();
    watch.collect();
    const std::string message = "jailed run created a scratch file under TMPDIR, /tmp or /var/tmp";
    require_rejection([&] { require(watch.empty(), message); }, message);
    child.finish();
}

// RED at 6b33c47fd: the old opted-in trace observes mkstemp only, and its
// inotify descriptor watches the private TMPDIR, so this /tmp open is invisible.
void own_open_tmp() {
    require_filesystems();
    PlainEnvironment environment;
    CreationWatch watch;
    const auto directory = std::string(std::getenv("TMPDIR"));
    PlantedFile own("/tmp/pineforge-open-" + directory.substr(directory.find_last_of('/') + 1),
                    false);
    watch.collect();
    require(watch.created_in("/tmp"), "own exclusive /tmp open escaped attribution");
    const std::string message = "TMPDIR on tmpfs must resolve directly to /var/tmp";
    require_rejection([&] {
        require(!watch.created_in("/dev/shm") && !watch.created_in("/tmp"), message);
    }, message);
    require_rejection_details([&] { watch.require_removed(); }, own.path(),
                              "pid=" + std::to_string(::getpid()));
    own.remove_and_close();
    watch.collect();
    require(watch.created_in("/tmp"), "unlinked exclusive-open evidence was discarded");
    watch.require_removed();
}

// RED at 6b33c47fd: owner != getpid() drops every fork-descendant mkstemp.
// This fork is deliberately AFTER watch construction, unlike the co-tenants.
void own_descendant_tmp() {
    require_filesystems();
    PlainEnvironment environment;
    CreationWatch watch;
    CoTenant descendant(false, false);
    descendant.start();
    watch.collect();
    require(watch.created_in("/tmp") && watch.created_in("/dev/shm")
                && watch.created_in("/var/tmp"),
            "own fork descendant's wrong-directory creations escaped attribution");
    require_rejection_details([&] { watch.require_removed(); }, "/tmp/" + descendant.prefix(),
                              "pid=" + std::to_string(descendant.pid()));
    descendant.finish();
    watch.collect();
    require(watch.created_in("/tmp"), "exited descendant's creation evidence was discarded");
    watch.require_removed();
}

// RED at 6b33c47fd: cleanup errors include the path but never a creator PID.
// A direct syscall separately exercises the inotify-only diagnostic, without
// pretending that inotify supplied a PID or guessing one from the filename.
void diagnostics() {
    require_filesystems();
    PlainEnvironment environment;
    CreationWatch watch;
    PlantedFile own("/tmp/pineforge-diagnostic-XXXXXX");
    watch.collect();
    require_rejection_details([&] { watch.require_removed(); }, own.path(),
                              "pid=" + std::to_string(::getpid()));
    own.remove_and_close();
    PlantedFile opaque(std::string(std::getenv("TMPDIR")) + "/other-uninstrumented", false, true);
    watch.collect();
    require_rejection_details([&] { watch.require_removed(); }, opaque.path(),
                              "pid=unattributable (inotify supplies no creator PID; "
                              "private TMPDIR event has no interposed descriptor)");
    opaque.remove_and_close();
    watch.collect();
    watch.require_removed();
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
    own_open_tmp();
    own_descendant_tmp();
    diagnostics();
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
            {"header_scratch", header_scratch}, {"own_open_tmp", own_open_tmp},
            {"own_descendant_tmp", own_descendant_tmp}, {"diagnostics", diagnostics}
        };
        for (const auto& entry : cases)
            if (entry.first == std::string(argv[1])) return s4e_tmp::test_main(entry.first, entry.second);
    }
    std::fputs("expected one scratch attribution row name; no file arguments accepted\n", stderr);
    return 1;
}
#endif // __linux__
