// CR-289: direct C fd sink + native C host run/capture, without a finalizer.
#include <pineforge/engine.hpp>
#include <pineforge/native_c_api.h>
#include <pineforge/pineforge.h>
#include <pineforge/report_sink.h>

#include <array>
#include <cerrno>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <stdexcept>
#include <string>

#include <fcntl.h>
#include <unistd.h>

namespace {
constexpr std::int64_t start = INT64_C(1704067200000);
constexpr int bar_count = 4;

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

struct Pipe {
    int descriptors[2]{-1, -1};
    Pipe() {
        require(::pipe(descriptors) == 0, "pipe failed");
        const int flags = ::fcntl(descriptors[0], F_GETFL);
        require(flags >= 0 && ::fcntl(descriptors[0], F_SETFL, flags | O_NONBLOCK) == 0,
                "nonblocking reader setup failed");
    }
    ~Pipe() {
        for (const int fd : descriptors) if (fd >= 0) ::close(fd);
    }
    std::string drain() const {
        std::string bytes;
        std::array<char, 4096> buffer{};
        for (;;) {
            const auto count = ::read(descriptors[0], buffer.data(), buffer.size());
            if (count < 0 && errno == EINTR) continue;
            if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return bytes;
            require(count >= 0, "pipe read failed");
            if (count == 0) return bytes;
            bytes.append(buffer.data(), static_cast<std::size_t>(count));
        }
    }
};

// C callbacks cannot supply a kernel-recorded equity point. Export only the
// inherited member pointer to the test: invoke it on the real BacktestEngine
// base subobject returned by the C API, never cast that object to this helper.
struct HeldPointAccess : pineforge::BacktestEngine {
    using Member = pf_equity_point_t* (pineforge::BacktestEngine::*)() noexcept;
    static Member member() { return &HeldPointAccess::last_equity_point; }
};

struct Host {
    pf_strategy_t handle = nullptr;
    int field = 0;
    double value = 0.0;
    int callbacks = 0;
    bool injected = false;
    pf_equity_point_t injected_point{};
    ~Host() { strategy_native_host_free(handle); }
};

int on_bar(void* user, const pf_bar_t*, const pf_native_decision_v1*) {
    auto& host = *static_cast<Host*>(user);
    if (++host.callbacks != 2) return 0;
    auto& engine = *static_cast<pineforge::BacktestEngine*>(host.handle);
    auto* point = (engine.*HeldPointAccess::member())();
    if (!point || point->time_ms != start || !std::isfinite(point->equity)
        || !std::isfinite(point->open_profit)) return 1;
    // Nearest native injection before the normal recorder releases the held
    // point through prepare_equity_point -> the production LineWriter::write_e.
    if (host.field == 0) point->equity = host.value;
    else point->open_profit = host.value;
    host.injected_point = *point;
    host.injected = true;
    return 0;
}

void run_case(int field, double value, bool refuses) {
    // Mutation: remove this field's finite_value() call in write_e. The
    // recorder then advances count_e/continues callbacks; a later metrics
    // refusal cannot satisfy this test's immediate-writer-refusal assertions.
    Pipe pipe;
    Host host;
    host.field = field;
    host.value = value;
    pf_native_callbacks_v1 callbacks{};
    callbacks.struct_size = static_cast<std::uint32_t>(sizeof callbacks);
    callbacks.version = PF_NATIVE_API_VERSION;
    callbacks.user = &host;
    callbacks.on_bar = &on_bar;
    host.handle = strategy_native_host_create_v1(&callbacks);
    require(host.handle != nullptr, "C host creation failed");
    pf_native_run_spec_v1 spec{};
    spec.struct_size = static_cast<std::uint32_t>(sizeof spec);
    spec.session_key = "CR-289-cabi-fd";
    spec.run_number = 1;
    spec.input_tf = spec.script_tf = "1";
    spec.ticker = "CR289";
    spec.tickerid = "TEST:CR289";
    spec.type = "crypto";
    spec.currency = spec.basecurrency = "USD";
    spec.description = spec.volumetype = spec.chart_timezone = "";
    spec.timezone = "UTC";
    spec.session = "24x7";
    spec.initial_capital = 10000.0;
    spec.point_value = spec.account_fx = 1.0;
    spec.price_tick = 0.01;
    spec.fee_kind = PF_NATIVE_FEE_CASH_PER_UNIT;
    spec.allowed_open_directions = PF_NATIVE_OPEN_DIRECTIONS_BOTH;
    pf_native_run_spec_ext_v1 ext{};
    ext.struct_size = static_cast<std::uint32_t>(sizeof ext);
    ext.version = PF_NATIVE_API_VERSION;
    ext.present_mask = PF_NATIVE_SPEC_EXT_REPORT;
    ext.report_policy = PF_NATIVE_REPORT_KERNEL_RECORDED;
    require(strategy_configure_native_ext_v1(host.handle, &spec, &ext) == PF_NATIVE_OK,
            "C host configuration failed");
    const pf_report_sink_fd_v1 sink{sizeof sink, 1, pipe.descriptors[1], "CR-289"};
    require(strategy_set_report_sink_fd_v1(host.handle, &sink) == 0, "C fd setter refused");
    const int flags = ::fcntl(pipe.descriptors[1], F_GETFL);
    const int fd_flags = ::fcntl(pipe.descriptors[1], F_GETFD);
    std::array<pf_bar_t, bar_count> bars{};
    for (int i = 0; i < bar_count; ++i)
        bars[static_cast<std::size_t>(i)] =
            {100.0, 100.0, 100.0, 100.0, 1.0, start + i * INT64_C(60000)};
    struct Report {
        pf_report_t value{};
        ~Report() { strategy_native_report_free_v1(&value); }
    } report;
    const int status = strategy_native_run_v1(host.handle, bars.data(), bar_count, &report.value);
    require(host.injected, "native held point was never injected");
    const double actual = field == 0 ? host.injected_point.equity : host.injected_point.open_profit;
    require(std::isnan(value) ? std::isnan(actual) : actual == value, "native poison changed");
    require(field == 0 ? host.injected_point.open_profit == 0.0
                       : host.injected_point.equity == 10000.0, "companion is not finite");
    pf_report_sink_receipt_v1 receipt{};
    receipt.struct_size = sizeof receipt;
    receipt.version = 1;
    require(strategy_report_sink_receipt_v1(host.handle, &receipt) == 0, "receipt query failed");
    const auto bytes = pipe.drain();
    if (refuses) {
        // A failed run's partial report can itself refuse the poisoned scratch
        // point. Both public failure statuses must retain the typed cause.
        require((status == PF_NATIVE_E_RUN_FAILED || status == PF_NATIVE_E_STATE)
                && strategy_last_run_status(host.handle) == 1,
                "C run/capture did not report failure");
        require(host.callbacks == 2 && receipt.count_e == 0, "writer did not refuse the first E");
        pf_native_state_v1 state{};
        state.struct_size = static_cast<std::uint32_t>(sizeof state);
        require(strategy_native_state_v1(host.handle, &state) == PF_NATIVE_OK
                && state.lifecycle == PF_NATIVE_LIFECYCLE_FAILED, "native run did not fail");
        const char* code = strategy_get_last_error_code(host.handle);
        const char* args = strategy_get_last_error_args(host.handle);
        require(code && std::string(code) == "report_result_invalid", "C boundary changed code");
        const std::string expected = std::string("{\"field\":\"equity_curve.")
            + (field == 0 ? "equity" : "open_profit") + "\",\"reason\":\"nonfinite\"}";
        require(args && std::string(args) == expected, "C boundary changed field/reason");
        require(receipt.complete == 0 && receipt.sha256_hex[0] == '\0' && bytes.empty(),
                "failed writer published bytes or completion");
    } else {
        require(status == PF_NATIVE_OK && strategy_last_run_status(host.handle) == 0,
                "finite C run failed");
        require(host.callbacks == bar_count && receipt.complete == 1
                && receipt.count_e == bar_count && bytes.find("{\"trailer\":") != std::string::npos,
                "finite C fd stream did not complete");
    }
    require(flags >= 0 && fd_flags >= 0 && ::fcntl(pipe.descriptors[1], F_GETFL) == flags
            && ::fcntl(pipe.descriptors[1], F_GETFD) == fd_flags, "borrowed fd changed");
}
} // namespace

int main() {
    int failures = 0;
    const double values[] = {std::numeric_limits<double>::quiet_NaN(),
                             std::numeric_limits<double>::infinity(),
                             -std::numeric_limits<double>::infinity()};
    const char* names[] = {"nan", "posinf", "neginf"};
    for (int field = 0; field < 2; ++field) {
        for (std::size_t i = 0; i < 3; ++i) {
            try {
                run_case(field, values[i], true);
            } catch (const std::exception& error) {
                ++failures;
                std::fprintf(stderr, "C API fd %s-%s: %s\n",
                             field == 0 ? "equity" : "open_profit", names[i], error.what());
            }
        }
        try {
            run_case(field, field == 0 ? 10000.0 : 0.0, false);
        } catch (const std::exception& error) {
            ++failures;
            std::fprintf(stderr, "C API fd finite field %d: %s\n", field, error.what());
        }
    }
    return failures == 0 ? 0 : 1;
}
