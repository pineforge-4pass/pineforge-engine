// B2-10 RED: a later begin must refuse a different stream on the borrowed
// descriptor number. BASE revalidates the type, then writes to that stream.
#include "report_modes_fix_fixture.hpp"

#include <cerrno>
#include <cstdlib>
#include <sys/socket.h>
#include <utility>

using namespace report_modes_fix;

namespace {

// Explicit instantiation permits naming a private member without changing
// the production header or copying the noncopyable native host itself.
struct SinkStateAccess {
    friend auto report_sink_member(SinkStateAccess);
};
template<auto Member>
struct ExposeSinkState {
    friend auto report_sink_member(SinkStateAccess) { return Member; }
};
template struct ExposeSinkState<&BacktestEngine::report_sink_state_>;

auto& sink_state(Host& host) {
    return host.*report_sink_member(SinkStateAccess{});
}

struct Socket {
    int fd[2]{-1, -1};
    Socket() {
        require(::socketpair(AF_UNIX, SOCK_STREAM, 0, fd) == 0, "socketpair failed");
    }
    ~Socket() {
        for (int descriptor : fd)
            if (descriptor >= 0) ::close(descriptor);
    }
    std::string read_to_eof() const {
        std::string bytes;
        char buffer[4096];
        for (;;) {
            const auto count = ::read(fd[0], buffer, sizeof buffer);
            if (count < 0 && errno == EINTR) continue;
            require(count >= 0, "socket read failed");
            if (count == 0) return bytes;
            bytes.append(buffer, static_cast<std::size_t>(count));
        }
    }
};

void configure_run(Host& host, std::uint64_t generation = 1) {
    const auto setup = host.configure_native(spec(generation));
    require(setup.status == NativeSetupStatus::Applied,
            "native configure refused (run " + std::to_string(generation)
                + ", error " + std::to_string(static_cast<int>(setup.validation.error))
                + ", field " + std::to_string(static_cast<int>(setup.validation.field))
                + "): " + host.last_error());
}

// The caller stages exactly one Ready spec before installing or running a sink.
void complete(Host& host) {
    run(host, tape(2));
    Report report;
    host.fill_report(&report.value);
    require(receipt(host).complete == 1, "same-descriptor control did not complete");
}

void require_identity_refusal(Host& host) {
    configure_run(host, 2);
    const auto bars = tape(2);
    host.run(bars.data(), static_cast<int>(bars.size()));
    require(host.last_run_status() != 0
                && run_failure_value_of(host).code == RunFailureCode::report_sink_failed,
            "assigned sink state accepted a different descriptor identity");
    require(receipt(host).complete == 0,
            "assigned sink state published a complete receipt for a reused descriptor");
}

void copy_move_identity() {
    Socket original;
    Socket unrelated;
    Host source, copied, moved;
    configure_run(source);
    const int borrowed = original.fd[1];
    const pf_report_sink_fd_v1 sink{sizeof sink, 1, borrowed, "p2-fd-identity-assignment"};
    require(source.set_report_sink_fd_v1(&sink) == 0, "assignment source install refused");

    // A copy retains configuration without the source's run-local owners.
    sink_state(copied) = sink_state(source);
    configure_run(copied);
    complete(copied);
    // Move assignment must carry the identity beside the borrowed fd.
    sink_state(moved) = std::move(sink_state(copied));
    configure_run(moved);
    complete(moved);
    sink_state(copied) = sink_state(source);

    require(::close(borrowed) == 0, "cannot close assignment source fd");
    original.fd[1] = -1;
    require(!original.read_to_eof().empty(), "assigned sink controls wrote no report");
    require(::dup2(unrelated.fd[1], borrowed) == borrowed, "cannot reuse assigned fd number");
    original.fd[1] = borrowed; // Own the duplicate for the remainder of this case.
    require_identity_refusal(copied);
    require_identity_refusal(moved);
    require(::shutdown(unrelated.fd[1], SHUT_WR) == 0, "cannot finish assignment stream");
    require(unrelated.read_to_eof().empty(), "assigned sink leaked bytes to a different stream");

    // A fresh engine accepts this same descriptor number with its own new
    // identity; the identities retained by copied/moved must not bleed into it.
    Socket next_stream;
    require(::dup2(next_stream.fd[1], borrowed) == borrowed, "cannot reuse fd for independent engine");
    Host independent;
    configure_run(independent);
    require(independent.set_report_sink_fd_v1(&sink) == 0, "independent sink install refused");
    complete(independent);
    require(::shutdown(next_stream.fd[1], SHUT_WR) == 0, "cannot finish independent stream");
    require_trailer(next_stream.read_to_eof());
}

} // namespace

int main() {
    return test_main("test_p2_report_sink_fd_identity", [] {
        const auto directory = scratch_directory();
        require(::setenv("TMPDIR", directory.c_str(), 1) == 0, "cannot select scratch directory");
        Socket original;
        Socket unrelated;
        Host host;
        configure_run(host);
        const int borrowed = original.fd[1];
        const pf_report_sink_fd_v1 sink{sizeof sink, 1, borrowed, "p2-fd-identity"};
        require(host.set_report_sink_fd_v1(&sink) == 0, "fd sink install refused");

        // Keeping the same descriptor installed across runs is supported.
        run(host, tape(2));
        {
            Report report;
            host.fill_report(&report.value);
            require(receipt(host).complete == 1, "first generation did not complete");
        }
        configure_run(host, 2);
        complete(host);

        require(::close(borrowed) == 0, "cannot close original borrowed fd");
        original.fd[1] = -1;
        require(!original.read_to_eof().empty(), "original stream received no report");
        require(::dup2(unrelated.fd[1], borrowed) == borrowed, "cannot reuse borrowed fd number");

        configure_run(host, 3);
        const auto bars = tape(2);
        host.run(bars.data(), static_cast<int>(bars.size()));
        const bool typed_refusal = host.last_run_status() != 0
            && run_failure_value_of(host).code == RunFailureCode::report_sink_failed;
        const bool incomplete = receipt(host).complete == 0;
        require(::shutdown(unrelated.fd[1], SHUT_WR) == 0, "cannot finish unrelated stream");
        const auto leaked = unrelated.read_to_eof();
        require(typed_refusal, "later begin accepted a different stream on the borrowed fd number");
        require(incomplete, "reused descriptor acquired a complete receipt");
        require(leaked.empty(), "later begin wrote report bytes to an unrelated stream");

        copy_move_identity();
    });
}
