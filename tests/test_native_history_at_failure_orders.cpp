#include "../src/native_execution_consumer.hpp"
#include "../src/run_results_errors.hpp"
#include <pineforge/run_failure.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <variant>

namespace {
namespace pf = pineforge;
namespace no = pineforge::native_order;
using Released = pf::detail::ReleasedNativeJournalRead;

constexpr char kUnretainedText[] = "native journal position is not retained";
constexpr char kReleasedText[] = "native command journal: run results released";

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

bool report_exception(const char* name, const std::exception& error) {
    const auto* info = dynamic_cast<const pf::RunFailureInfo*>(&error);
    std::fprintf(stderr, "FAIL %s: caught %s (code=%s), what=\"%s\"\n",
                 name, typeid(error).name(),
                 info ? pf::run_failure_code_name(info->run_failure_code()) : "uncoded",
                 error.what());
    return false;
}

bool report_outcome(const char* name, const char* outcome) {
    std::fprintf(stderr, "FAIL %s: %s\n", name, outcome);
    return false;
}

bool check_refusal(const char* name, const no::WorkingRequestCore& core,
                   std::size_t position, bool expect_release) {
    try {
        (void)core.history_at(position);
    } catch (const Released& error) {
        const std::exception& observed = error;
        if (expect_release && !dynamic_cast<const std::out_of_range*>(&observed)
            && error.run_failure_code() == pf::RunFailureCode::engine_invariant
            && std::string_view(error.what()) == kReleasedText) {
            return true;
        }
        return report_exception(name, error);
    } catch (const std::out_of_range& error) {
        const auto* info = dynamic_cast<const pf::RunFailureInfo*>(&error);
        if (!expect_release && info && info->run_failure_code() == pf::RunFailureCode::engine_invariant
            && std::string_view(error.what()) == kUnretainedText) {
            return true;
        }
        return report_exception(name, error);
    } catch (const std::exception& error) {
        return report_exception(name, error);
    } catch (...) {
        return report_outcome(name, "caught a non-standard exception with no what() text");
    }
    return report_outcome(name, "history_at returned without throwing");
}

std::uint64_t ordinal_of(const no::CommandEvent& event) {
    return std::visit([](const auto& payload) { return payload.ordinal; }, event);
}

bool check_live_event(const no::WorkingRequestCore& core, std::size_t position,
                      std::uint64_t submitted_ordinal) {
    constexpr char name[] = "live/in-range";
    try {
        const auto* expected = core.event_at({core.identity(), submitted_ordinal});
        if (!expected) return report_outcome(name, "event_at did not find the submitted event");
        const auto expected_ordinal = ordinal_of(*expected);
        const auto actual_ordinal = ordinal_of(core.history_at(position));
        if (actual_ordinal == expected_ordinal) return true;
        const auto message = "history_at returned ordinal " + std::to_string(actual_ordinal)
            + ", expected " + std::to_string(expected_ordinal) + " from event_at";
        return report_outcome(name, message.c_str());
    } catch (const std::exception& error) {
        return report_exception(name, error);
    } catch (...) {
        return report_outcome(name, "caught a non-standard exception with no what() text");
    }
}

no::Request resting_buy() {
    no::Request request{no::Transact{1.0}, "rest", ""};
    request.trigger = no::Limit{50.0};
    return request;
}

class JournalHost final : public pf::NativeStrategyHost {
public:
    pf::NativeExecutionConsumer& consumer() { return pf::as_native_consumer(execution_consumer()); }

    std::uint64_t second_ordinal() const { return second_ordinal_; }

private:
    void on_native_bar(const pf::Bar&, const pf::NativeDecisionContext&) override {
        if (second_ordinal_ != 0) return;
        for (int i = 0; i < 2; ++i) {
            const auto result = submit(resting_buy());
            require(result.status == no::SubmitStatus::Accepted && result.handle,
                    "submit refused a resting request");
            second_ordinal_ = result.event_ordinal;
        }
    }

    std::uint64_t second_ordinal_ = 0;
};

void run_host(JournalHost& host) {
    pf::NativeRunSpec spec;
    spec.identity = {"history-at-release", 1};
    spec.input_tf = "5";
    spec.script_tf = "5";
    spec.tickerid = "TEST:JOURNAL";
    spec.timezone = "UTC";
    spec.session = "24x7";
    spec.slot_label_policy = pf::NativeSlotLabelPolicy::FeedTolerant;
    spec.initial_capital = 10000.0;
    spec.point_value = 1.0;
    spec.account_fx = 1.0;
    spec.price_tick = 0.25;
    spec.event_retention = pf::NativeEventRetention::Commands;
    if (host.configure_native(spec).status != pf::NativeSetupStatus::Applied)
        throw std::runtime_error("configure_native refused: " + host.last_error());

    std::array<pf::Bar, 3> bars{};
    std::int64_t timestamp = 1736121600000LL;
    for (auto& bar : bars) {
        bar.open = bar.high = bar.low = bar.close = 100.0;
        bar.volume = 1.0;
        bar.timestamp = timestamp;
        timestamp += 300000;
    }
    host.run(bars.data(), static_cast<int>(bars.size()));
    if (host.native_state().kind != pf::NativeLifecycleKind::Completed)
        throw std::runtime_error("run did not complete: " + host.last_error());
}

bool check_history_at_failure_orders() {
    JournalHost host;
    run_host(host);
    auto& consumer = host.consumer();
    const auto& core = consumer.request_core();
    const auto live_end = core.history_end();
    require(core.history_base() == 0 && live_end >= 2 && host.second_ordinal() != 0,
            "expected two retained command events");
    constexpr std::size_t second = 1;
    bool ok = check_live_event(core, second, host.second_ordinal());
    ok &= check_refusal("live/at-end", core, live_end, false);
    ok &= check_refusal("live/past-end", core, live_end + 1000, false);

    no::WorkingRequestCore window({"history-at-window", 1});
    std::uint64_t incarnation = 1;
    std::uint64_t ordinal = 1;
    const auto first = window.submit(resting_buy(), 0, incarnation, ordinal);
    const auto next = window.submit(resting_buy(), 0, incarnation, ordinal);
    require(first.status == no::SubmitStatus::Accepted && next.status == no::SubmitStatus::Accepted,
            "could not populate the retirement window");
    require(window.retire_history(first.event_ordinal) != 0 && window.history_base() > 0
                && window.history_base() < window.history_end(),
            "retire_history did not leave a retained suffix above position zero");
    ok &= check_refusal("live/below-base", window, 0, false);

    // Moving the live core leaves its source without a journal.
    [[maybe_unused]] no::WorkingRequestCore moved(std::move(window));
    require(window.history_base() == 0 && window.history_end() == 0,
            "move did not empty the source window");
    ok &= check_refusal("no-journal/zero", window, 0, false);

    consumer.release_run_results();
    const auto released_end = core.history_end();
    require(core.history_base() == released_end, "release did not leave an empty window");
    ok &= check_refusal("released/formerly-in-range", core, second, true);
    ok &= check_refusal("released/at-end", core, released_end, true);
    ok &= check_refusal("released/past-end", core, released_end + 1000, true);
    ok &= check_refusal("released/zero", core, 0, true);

    consumer.release_run_results();
    require(core.history_base() == released_end && core.history_end() == released_end,
            "second release changed the empty window");
    ok &= check_refusal("released-again/formerly-in-range", core, second, true);
    ok &= check_refusal("released-again/at-end", core, released_end, true);
    ok &= check_refusal("released-again/past-end", core, released_end + 1000, true);
    ok &= check_refusal("released-again/zero", core, 0, true);
    return ok;
}
} // namespace

int main() {
    try {
        return check_history_at_failure_orders() ? 0 : 1;
    } catch (const std::exception& error) {
        report_exception("history_at setup", error);
    } catch (...) {
        report_outcome("history_at setup", "caught a non-standard exception with no what() text");
    }
    return 1;
}
