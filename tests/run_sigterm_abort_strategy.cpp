// Whole-archive ctypes fixture: all run/sink/failure behavior is the runtime's.
#include <pineforge/pineforge.h>
#include "run_sigterm_abort_fixture.hpp"

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <string>
#include <vector>
#include <unistd.h>

namespace {

class GateHost final : public q7_fixture::Host {
public:
    int gate_bar = 3;
    int gate_input = -1;
    int control = -1;
    std::int64_t completed_inputs = 0;

    GateHost() {
        if (const char* fd = std::getenv("PINEFORGE_Q7_CONTROL_FD")) control = std::stoi(fd);
        if (std::getenv("PINEFORGE_Q7_BEFORE_START")) gate("create\n");
    }

    void send(const std::string& text) const {
        if (control < 0) return;
        std::size_t at = 0;
        while (at < text.size()) {
            const auto count = ::write(control, text.data() + at, text.size() - at);
            if (count < 0 && errno == EINTR) continue;
            if (count <= 0) throw pineforge::coded<std::logic_error>(
                pineforge::RunFailureCode::engine_invariant, {}, "Q7 fixture control write");
            at += static_cast<std::size_t>(count);
        }
    }

    void done() const {
        send("done " + std::to_string(callbacks) + " " + std::to_string(applied)
             + " " + std::to_string(begins) + " " + std::to_string(completed_inputs) + "\n");
    }

    void before_capture() {
        if (std::getenv("PINEFORGE_Q7_BEFORE_CAPTURE")) gate("capture\n");
    }

    void on_native_input(const pineforge::Bar&,
                         const pineforge::NativeInputContext& context) override {
        if (context.input_index == gate_input)
            gate("input " + std::to_string(context.input_index) + "\n");
    }

private:
    void gate(const std::string& event) {
        if (control < 0) return;
        send(event);
        for (;;) {
            char command = 0;
            const auto count = ::read(control, &command, 1);
            if (count < 0 && errno == EINTR) continue;
            if (count == 1 && command == 'R') return;
            if (count == 1 && command == '?') {
                // Probe the real signal flag while the actual bar is paused.
                // This does not consume it or bypass any runtime boundary.
                send(pf_run_abort_requested_v1() ? "flag 1\n" : "flag 0\n");
                continue;
            }
            throw pineforge::coded<std::logic_error>(
                pineforge::RunFailureCode::engine_invariant, {}, "Q7 fixture control read");
        }
    }

    void after_identity(int index) override {
        if (index == gate_bar) gate("bar " + std::to_string(index) + "\n");
    }
};

pineforge::BacktestEngine* engine(pf_strategy_t s) {
    return static_cast<pineforge::BacktestEngine*>(s);
}

GateHost* host(pf_strategy_t s) { return static_cast<GateHost*>(engine(s)); }

} // namespace

extern "C" {

pf_strategy_t strategy_create(const char*) {
    return static_cast<pineforge::BacktestEngine*>(new GateHost);
}

void strategy_set_input(pf_strategy_t s, const char* key, const char* value) {
    if (std::strcmp(key, "gate_bar") == 0) host(s)->gate_bar = std::stoi(value);
    if (std::strcmp(key, "gate_input") == 0) host(s)->gate_input = std::stoi(value);
}

void strategy_set_override(pf_strategy_t, const char*, const char*) {}

void run_backtest_full(pf_strategy_t s, pf_bar_t* bars, int n, const char* input_tf,
                       const char* script_tf, int magnifier, int samples,
                       pf_magnifier_distribution_t distribution, pf_report_t* out) {
    *out = {};
    try {
        std::vector<pineforge::Bar> tape;
        tape.reserve(static_cast<std::size_t>(n));
        for (int i = 0; i < n; ++i)
            tape.push_back({bars[i].open, bars[i].high, bars[i].low, bars[i].close,
                            bars[i].volume, bars[i].timestamp});
        const std::string input = input_tf && *input_tf ? input_tf : "1";
        const std::string script = script_tf && *script_tf ? script_tf : input;
        host(s)->configure(input, script);
        host(s)->run(tape.data(), n, input, script, magnifier != 0, samples,
                     static_cast<pineforge::MagnifierDistribution>(distribution));
        host(s)->before_capture();
        host(s)->fill_report(reinterpret_cast<pineforge::ReportC*>(out));
        host(s)->completed_inputs = out->input_bars_processed;
    } catch (const std::exception& error) {
        pineforge::BacktestEngine::free_report(reinterpret_cast<pineforge::ReportC*>(out));
        *out = {};
        pineforge::note_run_failure(*engine(s), "run_backtest_full", error);
    }
}

void run_backtest(pf_strategy_t s, pf_bar_t* bars, int n, pf_report_t* out) {
    run_backtest_full(s, bars, n, "1", "1", 0, 4, PF_MAGNIFIER_ENDPOINTS, out);
}

void report_free(pf_report_t* out) {
    pineforge::BacktestEngine::free_report(reinterpret_cast<pineforge::ReportC*>(out));
}

void strategy_free(pf_strategy_t s) {
    host(s)->done();
    delete engine(s);
}

} // extern "C"
