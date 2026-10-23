// B2-04: BASE lets the fd setter discard a path generation, or detach an
// unfinished fd generation, even while its scratch curve is authoritative.
#include "report_modes_fix_fixture.hpp"

#include <cerrno>
#include <cstdlib>
#include <sys/socket.h>

using namespace report_modes_fix;

namespace {
struct Socket {
    int descriptors[2]{-1, -1};
    Socket() {
        require(::socketpair(AF_UNIX, SOCK_STREAM, 0, descriptors) == 0,
                "socketpair failed");
    }
    ~Socket() {
        for (int descriptor : descriptors)
            if (descriptor >= 0) ::close(descriptor);
    }
    std::string finish() {
        require(::shutdown(descriptors[1], SHUT_WR) == 0, "socket shutdown failed");
        std::string bytes;
        char buffer[4096];
        for (;;) {
            const auto count = ::read(descriptors[0], buffer, sizeof buffer);
            if (count < 0 && errno == EINTR) continue;
            require(count >= 0, "socket read failed");
            if (count == 0) return bytes;
            bytes.append(buffer, static_cast<std::size_t>(count));
        }
    }
};

void same_report(const ReportC& after, const ReportC& before) {
    require(after.script_bars_processed == 8, "fixture did not process eight bars");
    require(after.equity_curve == nullptr && after.equity_curve_len == 0,
            "NULL removal changed a sink report into a one-point array");
    require(after.trades == nullptr && after.trades_len == 0, "NULL removal changed trade shape");
    require(std::memcmp(&after.metrics, &before.metrics, sizeof(pf_metrics_t)) == 0,
            "refused removal changed metrics");
}
}

int main() {
    return test_main("test_report_sink_fd_null_removal_guard", [] {
        const auto bars = tape(8);
        {
            Host host;
            configure(host);
            const auto path = sink_path("path-null-fd-guard");
            install_sink(host, path);
            run(host, bars);
            Report before;
            host.fill_report(&before.value);
            const auto saved = receipt(host);
            require(saved.complete == 1, "path fixture did not finish");
            require(host.set_report_sink_v1(nullptr) == -3, "path guard was not reached");
            require(host.set_report_sink_fd_v1(nullptr) == -3,
                    "fd NULL removal bypassed the path scratch guard");
            const auto after_receipt = receipt(host);
            require(std::memcmp(&saved, &after_receipt, sizeof saved) == 0,
                    "refused removal discarded receipt");
            Report after;
            host.fill_report(&after.value);
            same_report(after.value, before.value);
            require_trailer(read_sink(path));
        }
        {
            const auto directory = scratch_directory();
            require(::setenv("TMPDIR", directory.c_str(), 1) == 0, "cannot set fd scratch directory");
            Socket output;
            Host host;
            configure(host);
            const pf_report_sink_fd_v1 sink{sizeof sink, 1, output.descriptors[1], "s4e-fd"};
            require(host.set_report_sink_fd_v1(&sink) == 0, "fd sink install refused");
            run(host, bars);
            require(receipt(host).complete == 0, "fd fixture finished before report capture");
            require(host.set_report_sink_v1(nullptr) == -3, "unfinished fd lacks path guard");
            require(host.set_report_sink_fd_v1(nullptr) == -3,
                    "unfinished fd NULL removal discarded its writer");
            Report before;
            host.fill_report(&before.value);
            const auto saved = receipt(host);
            require(saved.complete == 1 && saved.count_e == 8,
                    "guarded fd generation did not emit all observations");
            require(host.set_report_sink_fd_v1(nullptr) == 0,
                    "completed fd detachment must remain supported");
            Report after;
            host.fill_report(&after.value);
            same_report(after.value, before.value);
            const auto after_receipt = receipt(host);
            require(std::memcmp(&saved, &after_receipt, sizeof saved) == 0,
                    "completed fd detachment discarded receipt");
            require_trailer(output.finish());
        }
    });
}
