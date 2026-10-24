// Real SIGTERM, real native callbacks/matching and the real buffered fd sink.
#include <pineforge/pineforge.h>
#include <pineforge/report_sink.h>
#include <pineforge/report_sink_writer.hpp>
#include "run_sigterm_abort_fixture.hpp"
#include "../src/report_sink_writer.hpp"

#include <cassert>
#include <cerrno>
#include <csignal>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>
#include <pthread.h>
#include <unistd.h>

namespace {

class SignalScope {
public:
    SignalScope() {
        assert(::sigemptyset(&term_) == 0);
        assert(::sigaddset(&term_, SIGTERM) == 0);
        assert(::pthread_sigmask(SIG_BLOCK, &term_, &previous_) == 0);
        assert(pf_run_abort_install_sigterm_v1() == 0);
        assert(pf_run_abort_requested_v1() == 0);
        assert(::pthread_sigmask(SIG_UNBLOCK, &term_, nullptr) == 0);
    }
    ~SignalScope() {
        assert(::pthread_sigmask(SIG_BLOCK, &term_, nullptr) == 0);
        assert(pf_run_abort_restore_sigterm_v1() == 0);
        assert(::pthread_sigmask(SIG_SETMASK, &previous_, nullptr) == 0);
    }
private:
    sigset_t term_{}, previous_{};
};

class Stream {
public:
    Stream() {
        assert(::pipe(fds_) == 0);
        reader_ = std::thread([this] {
            char buffer[8192];
            for (;;) {
                const auto count = ::read(fds_[0], buffer, sizeof buffer);
                if (count < 0 && errno == EINTR) continue;
                assert(count >= 0);
                if (count == 0) break;
                bytes.append(buffer, static_cast<std::size_t>(count));
            }
            assert(::close(fds_[0]) == 0);
        });
    }
    ~Stream() { finish(); }
    int fd() const { return fds_[1]; }
    void finish() {
        if (fds_[1] >= 0) {
            assert(::close(fds_[1]) == 0);
            fds_[1] = -1;
            reader_.join();
        }
    }
    std::string bytes;
private:
    int fds_[2]{-1, -1};
    std::thread reader_;
};

class SignalAt final : public q7_fixture::Host {
public:
    explicit SignalAt(int index) : index_(index) {}
private:
    void after_identity(int index) override {
        if (index != index_) return;
        assert(::raise(SIGTERM) == 0);
        assert(pf_run_abort_requested_v1() == 1);
        assert(last_run_status() == 0);
        assert(pf_run_abort_install_sigterm_v1() == 0); // cannot erase the flag
        assert(::raise(SIGTERM) == 0);
        assert(pf_run_abort_requested_v1() == 1);
        // Host::on_native_bar still has to submit, return, and match at close.
    }
    int index_;
};

class SignalOnInput final : public q7_fixture::Host {
public:
    void on_native_input(const pineforge::Bar&,
                         const pineforge::NativeInputContext& context) override {
        if (context.input_index == 2) assert(::raise(SIGTERM) == 0);
    }
};

std::vector<pineforge::Bar> feed() {
    std::vector<pineforge::Bar> result;
    for (int i = 0; i < 60; ++i) {
        const double price = 100.0 + i * 0.125;
        result.push_back({price, price + 1.0, price - 1.0, price, 1.0, i * INT64_C(60000)});
    }
    return result;
}

void install(q7_fixture::Host& host, Stream& stream) {
    pf_report_sink_fd_v1 sink{sizeof sink, 1, stream.fd(), "q7-native"};
    assert(strategy_set_report_sink_fd_v1(&host, &sink) == 0);
}

std::size_t occurrences(const std::string& text, const std::string& token) {
    std::size_t count = 0, at = 0;
    while ((at = text.find(token, at)) != std::string::npos) {
        ++count;
        at += token.size();
    }
    return count;
}

void incomplete(const Stream& stream, int completed) {
    assert(!stream.bytes.empty() && stream.bytes.back() == '\n');
    assert(stream.bytes.find("\"trailer\":") == std::string::npos);
    assert(occurrences(stream.bytes, ",\"tail\",") == static_cast<std::size_t>(completed));
    assert(occurrences(stream.bytes, ",\"applied\",") == static_cast<std::size_t>(completed));
    assert(stream.bytes.find("run_aborted") == std::string::npos);
}

void during_bar(const char* script_tf, int stop) {
    SignalScope signals;
    Stream stream;
    SignalAt host(stop);
    host.configure("1", script_tf);
    install(host, stream);
    const auto bars = feed();
    host.run(bars.data(), static_cast<int>(bars.size()), "1", script_tf);
    assert(host.last_run_status() == 1);
    assert(std::string(strategy_get_last_error_code(&host)) == "run_aborted");
    assert(host.callbacks == stop + 1 && host.applied == host.callbacks && host.begins == 1);
    const int completed_inputs = host.callbacks * std::stoi(script_tf);
    const std::string expected = "{\"bars_count\":" + std::to_string(completed_inputs)
        + ",\"strategy_started\":true}";
    assert(std::string(strategy_get_last_error_args(&host)) == expected);
    pineforge::ReportC report{};
    host.fill_report(&report); // capture cannot turn an abort into a completed sink
    assert(report.input_bars_processed == completed_inputs);
    assert(report.script_bars_processed == host.callbacks);
    pineforge::BacktestEngine::free_report(&report);
    assert(std::string(strategy_get_last_error_args(&host)) == expected);
    pf_report_sink_receipt_v1 receipt{};
    receipt.struct_size = sizeof receipt;
    receipt.version = 1;
    assert(strategy_report_sink_receipt_v1(&host, &receipt) == 0 && receipt.complete == 0);
    stream.finish(); // library borrowed the still-open fd; host owns this close
    incomplete(stream, host.callbacks);
}

void before_first_bar() {
    SignalScope signals;
    Stream stream;
    q7_fixture::Host host;
    host.configure("1", "1");
    install(host, stream);
    assert(::raise(SIGTERM) == 0 && ::raise(SIGTERM) == 0);
    const auto bars = feed();
    host.run(bars.data(), static_cast<int>(bars.size()), "1", "1");
    assert(host.last_run_status() == 1);
    assert(host.callbacks == 0 && host.applied == 0 && host.begins == 0);
    assert(std::string(strategy_get_last_error_code(&host)) == "run_aborted");
    assert(std::string(strategy_get_last_error_args(&host))
           == "{\"bars_count\":0,\"strategy_started\":false}");
    stream.finish();
    incomplete(stream, 0);
}

void pending_aggregate() {
    SignalScope signals;
    Stream stream;
    SignalOnInput host;
    host.configure("1", "5");
    install(host, stream);
    const auto bars = feed();
    host.run(bars.data(), static_cast<int>(bars.size()), "1", "5");
    assert(host.last_run_status() == 1 && host.callbacks == 0 && host.applied == 0);
    assert(std::string(strategy_get_last_error_code(&host)) == "run_aborted");
    assert(std::string(strategy_get_last_error_args(&host))
           == "{\"bars_count\":3,\"strategy_started\":true}");
    pineforge::ReportC report{};
    host.fill_report(&report);
    assert(report.input_bars_processed == 3 && report.script_bars_processed == 0);
    pineforge::BacktestEngine::free_report(&report);
    stream.finish();
    incomplete(stream, 0);
}

void normal() {
    // A complete run has identical sink bytes with and without the opt-in handler.
    auto run = [] {
        Stream stream;
        q7_fixture::Host host;
        host.configure("1", "5");
        install(host, stream);
        const auto bars = feed();
        host.run(bars.data(), static_cast<int>(bars.size()), "1", "5");
        assert(host.last_run_status() == 0 && host.last_error().empty());
        assert(host.callbacks == 12 && host.applied == 12);
        pineforge::ReportC report{};
        host.fill_report(&report);
        assert(report.input_bars_processed == static_cast<std::int64_t>(bars.size()));
        assert(report.script_bars_processed == host.callbacks);
        pineforge::BacktestEngine::free_report(&report);
        pf_report_sink_receipt_v1 receipt{};
        receipt.struct_size = sizeof receipt;
        receipt.version = 1;
        assert(strategy_report_sink_receipt_v1(&host, &receipt) == 0 && receipt.complete == 1);
        assert(receipt.count_e == host.callbacks && receipt.count_t == host.callbacks / 2);
        stream.finish();
        assert(occurrences(stream.bytes, "\"trailer\":") == 1);
        assert(stream.bytes.find("run_aborted") == std::string::npos);
        return stream.bytes;
    };
    const auto without = run();
    SignalScope signals;
    assert(run() == without);
}

void after_last_bar() {
    SignalScope signals;
    Stream stream;
    q7_fixture::Host host;
    host.configure("1", "1");
    install(host, stream);
    const auto bars = feed();
    host.run(bars.data(), static_cast<int>(bars.size()), "1", "1");
    assert(host.last_run_status() == 0 && host.callbacks == 60 && host.applied == 60);
    assert(::raise(SIGTERM) == 0);
    pineforge::ReportC report{};
    host.fill_report(&report);
    assert(host.last_run_status() == 1 && report.input_bars_processed == 60);
    assert(std::string(strategy_get_last_error_code(&host)) == "run_aborted");
    assert(std::string(strategy_get_last_error_args(&host))
           == "{\"bars_count\":60,\"strategy_started\":true}");
    pineforge::BacktestEngine::free_report(&report);
    pf_report_sink_receipt_v1 receipt{};
    receipt.struct_size = sizeof receipt;
    receipt.version = 1;
    assert(strategy_report_sink_receipt_v1(&host, &receipt) == 0 && receipt.complete == 0);
    stream.finish();
    incomplete(stream, host.callbacks);
}

void writer_suffix() {
    Stream stream;
    pf_report_sink_fd_v1 sink{sizeof sink, 1, stream.fd(), "q7-suffix"};
    pineforge::report_sink::LineWriter writer(sink);
    writer.write_header({false, true, false, 0, 0});
    pf_trace_entry_t entry{};
    entry.timestamp = 60000;
    entry.bar_index = 1;
    entry.value = 17.0;
    const std::string name(70000, 'x');
    writer.write_x(entry, name);
    writer.abandon();
    bool refused = false;
    try { writer.write_trailer(); }
    catch (const pineforge::report_sink::WriterError& error) {
        refused = error.reason() == pineforge::report_sink::WriterFailure::invalid_state;
    }
    assert(refused);
    stream.finish();
    assert(stream.bytes.find("[\"X\",60000,1,\"" + name + "\",17.0]\n") != std::string::npos);
    assert(stream.bytes.back() == '\n' && stream.bytes.find("\"trailer\":") == std::string::npos);
}

void witness_types() {
    using namespace pineforge;
    for (const RunFailureArgs& args : {
             RunFailureArgs{{"bars_count", 1.0}, {"strategy_started", false}},
             RunFailureArgs{{"bars_count", true}, {"strategy_started", true}},
             RunFailureArgs{{"bars_count", 1}, {"strategy_started", 1}},
             RunFailureArgs{{"bars_count", 1}, {"strategy_started", "true"}}}) {
        assert(make_run_failure(RunFailureCode::run_aborted, args).code
               == RunFailureCode::engine_invariant);
    }
    assert(make_run_failure(RunFailureCode::run_aborted, {}).code == RunFailureCode::run_aborted);
}

} // namespace

int main() {
    for (const char* tf : {"1", "5"})
        for (int stop : {0, 3, 9}) during_bar(tf, stop);
    before_first_bar();
    pending_aggregate();
    assert(pf_run_abort_requested_v1() == 0);
    normal();
    after_last_bar();
    writer_suffix();
    witness_types();
    std::puts("Q7 SIGTERM: completed bars, post-signal commands/fills, incomplete sink and normal control");
    return 0;
}
