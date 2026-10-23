// B2-09: a borrowed socket must be AF_UNIX SOCK_STREAM.
#include "p2_sink_writer_fixture.hpp"

#include <netinet/in.h>
#include <sys/socket.h>

namespace {
void tcp_refused() {
    p2_sink::Fd listener(::socket(AF_INET, SOCK_STREAM, 0));
    p2_sink::require(listener.value >= 0, "create TCP listener");
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = 0;
    p2_sink::require(::bind(listener.value, reinterpret_cast<sockaddr*>(&address),
                            sizeof address) == 0, "bind loopback TCP listener");
    p2_sink::require(::listen(listener.value, 1) == 0, "listen on loopback TCP");
    socklen_t length = sizeof address;
    p2_sink::require(::getsockname(listener.value, reinterpret_cast<sockaddr*>(&address),
                                   &length) == 0, "read loopback TCP address");
    p2_sink::Fd sender(::socket(AF_INET, SOCK_STREAM, 0));
    p2_sink::require(sender.value >= 0, "create TCP client");
    p2_sink::require(::connect(sender.value, reinterpret_cast<sockaddr*>(&address),
                               sizeof address) == 0, "connect loopback TCP pair");
    p2_sink::Fd peer(::accept(listener.value, nullptr, nullptr));
    p2_sink::require(peer.value >= 0, "accept loopback TCP pair");
    const pf_report_sink_fd_v1 sink{sizeof sink, 1, sender.value, "B2-09-tcp"};
    {
        p2_sink::rs::LineWriter writer(sink);
        p2_sink::expect_io([&] { writer.write_header({false, false, false, 0, 0}); }, EINVAL);
        p2_sink::require(writer.diagnostics().complete == 0, "TCP refusal completed the writer");
    }
    p2_sink::require(::fcntl(sender.value, F_GETFD) >= 0,
                     "TCP refusal closed the borrowed descriptor");
    char byte = 0;
    errno = 0;
    p2_sink::require(::recv(peer.value, &byte, 1, MSG_DONTWAIT) == -1
                         && (errno == EAGAIN || errno == EWOULDBLOCK),
                     "TCP refusal emitted report bytes");
}

void unix_stream_accepted() {
    int pair[2]{-1, -1};
    p2_sink::require(::socketpair(AF_UNIX, SOCK_STREAM, 0, pair) == 0,
                     "create AF_UNIX stream pair");
    p2_sink::Fd peer(pair[0]), sender(pair[1]);
    const pf_report_sink_fd_v1 sink{sizeof sink, 1, sender.value, "B2-09-unix"};
    {
        p2_sink::rs::LineWriter writer(sink);
        writer.write_header({false, false, false, 0, 0});
        writer.write_e({0, 10000.0, 0.0});
        p2_sink::require(writer.write_trailer().count_e == 1,
                         "AF_UNIX stream lost its E row");
        p2_sink::require(writer.diagnostics().complete == 1,
                         "AF_UNIX stream did not complete");
    }
    p2_sink::require(::fcntl(sender.value, F_GETFD) >= 0,
                     "AF_UNIX writer closed its borrowed descriptor");
    std::array<char, 4096> bytes{};
    std::string text;
    for (;;) {
        const auto count = ::recv(peer.value, bytes.data(), bytes.size(), MSG_DONTWAIT);
        if (count < 0 && errno == EINTR) continue;
        if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) break;
        p2_sink::require(count > 0, "AF_UNIX stream read failed");
        text.append(bytes.data(), static_cast<std::size_t>(count));
    }
    p2_sink::require(text.find("{\"schema\":") == 0
                         && text.find("{\"trailer\":") != std::string::npos
                         && text.back() == '\n', "AF_UNIX stream lost framing");
}

void fifo_accepted() {
    int pair[2]{-1, -1};
    p2_sink::require(::pipe(pair) == 0, "create FIFO control");
    p2_sink::Fd peer(pair[0]), sender(pair[1]);
    const pf_report_sink_fd_v1 sink{sizeof sink, 1, sender.value, "B2-09-fifo"};
    p2_sink::rs::LineWriter writer(sink);
    writer.write_header({false, false, false, 0, 0});
    p2_sink::require(writer.write_trailer().count_e == 0
                         && writer.diagnostics().complete == 1,
                     "FIFO control did not complete");
}
}

int main() {
    return p2_sink::test_main("B2-09 socket family", [] {
        tcp_refused();
        unix_stream_accepted();
        fifo_accepted();
    });
}
