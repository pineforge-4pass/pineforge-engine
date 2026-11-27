// The engine layout the live runner shares with a stamped strategy module.
//
// pineforge-live reads the handle of a module that declares
// PF_STREAM_ENGINE_LAYOUT_VERSION as a C++ object (runner/main.cpp, the four
// static_cast<BacktestEngine*> sites) and then runs its own copy of
// src/live_report.cpp on it. That code reads members of BacktestEngine and of
// NativeExecutionConsumer in place, so the runner and the module must agree on
// where those members are. The stamp is their only agreement; this test ties
// the stamp to the layout:
//
//   * it measures sizeof of both classes and the offset and size of every
//     member listed below, on real objects;
//   * it compares them with tests/stream_engine_layout_pins.inc, the numbers
//     measured for one value of the stamp.
//
// A layout that moves under an unchanged stamp fails here. So do pins left
// behind by a raised stamp, and a pin file whose keys are not exactly the
// members listed below. The preflight stage abi-pin-guard
// (scripts/check_abi_pin_guard.py) refuses a change of the pin file that does
// not raise the stamp, so the pins cannot be edited to follow the layout.
//
// The pins are measured on Linux x86_64 with GNU libstdc++. The row is
// registered on Linux x86_64 only (tests/lanes.d/stream-engine-layout.cmake),
// and a build of this file for anything else, another C++ library included,
// reports NOT COMPARED and fails; it never passes.
//
// Every run prints the measured table in the pin file's own syntax, between
// two marker lines, whatever the verdict.
//
// Source-free: this file runs in the kernel-only profile.
#include "../src/native_execution_consumer.hpp"

#include <pineforge/native_host.hpp>
#include <pineforge/pineforge.h>

#include <cstddef>
#include <cstdio>
#include <memory>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace stream_engine_layout {

// The members the stamped reader touches, as (class, member, tag suffix, path
// below the member). The key of a row is the member followed by the path.
//
// BacktestEngine: read by NativeExecutionConsumer::fill_live_report_patch
// (src/live_report.cpp), three of them through an inline accessor of
// include/pineforge/engine.hpp: stream_phase_ (stream_is_realtime),
// last_error_ (last_error), and trades_ with range_end_trades_ again from the
// trade text companion (report_trade_count, get_report_trade). equity_curve_
// is also written there.
// NativeExecutionConsumer: rows_current_ and the three selected_epoch_ flags
// are read by the same function; consumed_high_water_, running_spec_ and
// state_ by the inline view() it calls (src/native_execution_consumer.hpp).
#define PF_STREAM_READER_MEMBERS(X) \
    X(BacktestEngine, active_equity_report_mode_, self, ) \
    X(BacktestEngine, bar_magnifier_enabled_, self, ) \
    X(BacktestEngine, bars_in_market_, self, ) \
    X(BacktestEngine, broker_state_hashes_, self, ) \
    X(BacktestEngine, chart_timezone_, self, ) \
    X(BacktestEngine, current_bar_, close, .close) \
    X(BacktestEngine, diag_input_bars_processed_, self, ) \
    X(BacktestEngine, diag_magnifier_sample_ticks_processed_, self, ) \
    X(BacktestEngine, diag_magnifier_sub_bars_processed_, self, ) \
    X(BacktestEngine, diag_needs_aggregation_, self, ) \
    X(BacktestEngine, diag_script_bars_processed_, self, ) \
    X(BacktestEngine, diag_script_tf_ratio_, self, ) \
    X(BacktestEngine, equity_curve_, self, ) \
    X(BacktestEngine, execution_consumer_slot_, ptr, .ptr) \
    X(BacktestEngine, first_bar_open_, self, ) \
    X(BacktestEngine, initial_capital_, self, ) \
    X(BacktestEngine, input_tf_, self, ) \
    X(BacktestEngine, last_error_, self, ) \
    X(BacktestEngine, last_run_status_, self, ) \
    X(BacktestEngine, optional_report_mode_, self, ) \
    X(BacktestEngine, range_end_trades_, self, ) \
    X(BacktestEngine, report_mode_, self, ) \
    X(BacktestEngine, report_sink_state_, enabled, .enabled) \
    X(BacktestEngine, report_sink_state_, receipt_complete, .receipt.complete) \
    X(BacktestEngine, report_sink_state_, selected_equity_stream, .selected_equity_stream) \
    X(BacktestEngine, script_tf_seconds_, self, ) \
    X(BacktestEngine, security_eval_states_, self, ) \
    X(BacktestEngine, stream_phase_, self, ) \
    X(BacktestEngine, trace_buffer_, self, ) \
    X(BacktestEngine, trace_names_, self, ) \
    X(BacktestEngine, trades_, self, ) \
    X(NativeExecutionConsumer, consumed_high_water_, self, ) \
    X(NativeExecutionConsumer, rows_current_, self, ) \
    X(NativeExecutionConsumer, running_spec_, self, ) \
    X(NativeExecutionConsumer, selected_epoch_, admitted_selected, .admitted_selected) \
    X(NativeExecutionConsumer, selected_epoch_, generation_counter, .generation_counter) \
    X(NativeExecutionConsumer, selected_epoch_, phase_observed, .phase_observed) \
    X(NativeExecutionConsumer, state_, self, )

// Explicit member instantiation names a private member without changing the
// class or its access specifiers (the convention of test_abi_layout_probe.cpp).
template<class Tag, auto Member> struct Access {
    friend auto access(Tag) { return Member; }
};

#define PF_STREAM_TAG(Type, member, suffix) Tag_##Type##_##member##suffix
#define PF_STREAM_DECLARE(Type, member, suffix, path) \
    struct PF_STREAM_TAG(Type, member, suffix) { \
        friend auto access(PF_STREAM_TAG(Type, member, suffix)); \
    }; \
    template struct Access<PF_STREAM_TAG(Type, member, suffix), &pineforge::Type::member>;
PF_STREAM_READER_MEMBERS(PF_STREAM_DECLARE)
#undef PF_STREAM_DECLARE

// A native host is the smallest object that holds both classes: its
// constructor binds the consumer to the engine's slot.
class ReaderHost final : public pineforge::NativeStrategyHost {
    void on_native_bar(const pineforge::Bar&, const pineforge::NativeDecisionContext&) override {}
};

struct MemberRow {
    std::string type;
    std::string member;
    std::size_t offset;
    std::size_t size;
};

struct SizeRow {
    std::string type;
    std::size_t size;
};

// Actual subobject addresses: neither class is standard-layout, so offsetof
// does not apply.
template<class Object, class Field>
MemberRow measure(const char* type, const char* member, const Object& object,
                  const Field& field) {
    const auto* base = reinterpret_cast<const unsigned char*>(std::addressof(object));
    const auto* at = reinterpret_cast<const unsigned char*>(std::addressof(field));
    return {type, member, static_cast<std::size_t>(at - base), sizeof(Field)};
}

std::vector<MemberRow> measured_members(
        const pineforge::BacktestEngine& subject_BacktestEngine,
        const pineforge::NativeExecutionConsumer& subject_NativeExecutionConsumer) {
    std::vector<MemberRow> rows;
#define PF_STREAM_MEASURE(Type, member, suffix, path) \
    rows.push_back(measure(#Type, #member #path, subject_##Type, \
        (subject_##Type.*access(PF_STREAM_TAG(Type, member, suffix){})) path));
    PF_STREAM_READER_MEMBERS(PF_STREAM_MEASURE)
#undef PF_STREAM_MEASURE
    return rows;
}

std::vector<SizeRow> measured_sizes() {
    return {{"BacktestEngine", sizeof(pineforge::BacktestEngine)},
            {"NativeExecutionConsumer", sizeof(pineforge::NativeExecutionConsumer)}};
}

// The pin file, read three times: its stamp rows, its size rows, its member rows.
std::vector<unsigned long long> pinned_stamps() {
    std::vector<unsigned long long> stamps;
#define PF_STREAM_LAYOUT_STAMP(value) stamps.push_back(value);
#define PF_STREAM_LAYOUT_SIZE(Type, bytes)
#define PF_STREAM_LAYOUT_MEMBER(Type, member, offset, bytes)
#include "stream_engine_layout_pins.inc"
#undef PF_STREAM_LAYOUT_STAMP
#undef PF_STREAM_LAYOUT_SIZE
#undef PF_STREAM_LAYOUT_MEMBER
    return stamps;
}

std::vector<SizeRow> pinned_sizes() {
    std::vector<SizeRow> rows;
#define PF_STREAM_LAYOUT_STAMP(value)
#define PF_STREAM_LAYOUT_SIZE(Type, bytes) rows.push_back({#Type, bytes});
#define PF_STREAM_LAYOUT_MEMBER(Type, member, offset, bytes)
#include "stream_engine_layout_pins.inc"
#undef PF_STREAM_LAYOUT_STAMP
#undef PF_STREAM_LAYOUT_SIZE
#undef PF_STREAM_LAYOUT_MEMBER
    return rows;
}

std::vector<MemberRow> pinned_members() {
    std::vector<MemberRow> rows;
#define PF_STREAM_LAYOUT_STAMP(value)
#define PF_STREAM_LAYOUT_SIZE(Type, bytes)
#define PF_STREAM_LAYOUT_MEMBER(Type, member, offset, bytes) \
    rows.push_back({#Type, #member, offset, bytes});
#include "stream_engine_layout_pins.inc"
#undef PF_STREAM_LAYOUT_STAMP
#undef PF_STREAM_LAYOUT_SIZE
#undef PF_STREAM_LAYOUT_MEMBER
    return rows;
}

const char* const kSay = "stream engine layout: ";
const char* const kPins = "tests/stream_engine_layout_pins.inc";

// The measured layout in the pin file's syntax, ready to replace its rows.
void print_measured(unsigned long long stamp, const std::vector<SizeRow>& sizes,
                    const std::vector<MemberRow>& members) {
    std::printf("%smeasured table begin\n", kSay);
    std::printf("PF_STREAM_LAYOUT_STAMP(%lluu)\n", stamp);
    for (const auto& size : sizes) {
        std::printf("PF_STREAM_LAYOUT_SIZE(%s, %zu)\n", size.type.c_str(), size.size);
        for (const auto& member : members) {
            if (member.type != size.type) continue;
            std::printf("PF_STREAM_LAYOUT_MEMBER(%s, %s, %zu, %zu)\n", member.type.c_str(),
                        member.member.c_str(), member.offset, member.size);
        }
    }
    std::printf("%smeasured table end\n", kSay);
}

}  // namespace stream_engine_layout

int main() {
    using namespace stream_engine_layout;
    const unsigned long long stamp = PF_STREAM_ENGINE_LAYOUT_VERSION;
    ReaderHost host;
    const pineforge::BacktestEngine& engine = host;
    const auto* consumer = static_cast<const pineforge::NativeExecutionConsumer*>(
        (engine.*access(PF_STREAM_TAG(BacktestEngine, execution_consumer_slot_, ptr){})).ptr.get());
    if (consumer == nullptr) {
        std::printf("%sFAIL: a native host holds no execution consumer to measure\n", kSay);
        return 1;
    }
    const auto sizes = measured_sizes();
    const auto members = measured_members(engine, *consumer);
    std::size_t engine_members = 0;
    for (const auto& member : members) engine_members += member.type == "BacktestEngine";
    std::printf("%sheader stamp PF_STREAM_ENGINE_LAYOUT_VERSION=%llu\n", kSay, stamp);
    std::printf("%sinventory: %zu BacktestEngine and %zu NativeExecutionConsumer member keys, "
                "%zu class sizes\n", kSay, engine_members, members.size() - engine_members,
                sizes.size());
    print_measured(stamp, sizes, members);

#if !(defined(__linux__) && defined(__x86_64__) && defined(__GLIBCXX__))
    std::printf("%sNOT COMPARED: the pins are measured on Linux x86_64 with GNU libstdc++; "
                "this build is another configuration and has no pins\n", kSay);
    return 1;
#else
    const auto stamps = pinned_stamps();
    const auto size_pins = pinned_sizes();
    const auto member_pins = pinned_members();
    std::printf("%spins: %zu stamp row(s), %zu size row(s), %zu member row(s) in %s\n", kSay,
                stamps.size(), size_pins.size(), member_pins.size(), kPins);
    if (stamps.empty() && size_pins.empty() && member_pins.empty()) {
        std::printf("%sNOT PINNED: %s holds no pins, so nothing ties stamp %llu to this layout\n",
                    kSay, kPins, stamp);
        std::printf("%sFAIL: pin the measured table above for stamp %llu\n", kSay, stamp);
        return 1;
    }

    std::size_t differences = 0;
    if (stamps.size() != 1) {
        std::printf("%sSTAMP ROWS: %s holds %zu stamp rows; exactly one is read\n", kSay, kPins,
                    stamps.size());
        ++differences;
    } else if (stamps.front() != stamp) {
        std::printf("%sSTALE PINS: the pins were measured for stamp %llu; the header defines "
                    "stamp %llu\n", kSay, stamps.front(), stamp);
        ++differences;
    }

    std::set<std::string> seen;
    for (const auto& pin : size_pins) {
        const auto key = "sizeof(" + pin.type + ")";
        if (!seen.insert(key).second) {
            std::printf("%sDUPLICATE PIN %s\n", kSay, key.c_str());
            ++differences;
            continue;
        }
        bool known = false;
        for (const auto& size : sizes) {
            if (size.type != pin.type) continue;
            known = true;
            if (size.size != pin.size) {
                std::printf("%sMOVED %s pinned=%zu measured=%zu\n", kSay, key.c_str(), pin.size,
                            size.size);
                ++differences;
            }
        }
        if (!known) {
            std::printf("%sUNKNOWN PIN %s: not a class this test measures\n", kSay, key.c_str());
            ++differences;
        }
    }
    for (const auto& size : sizes) {
        if (!seen.count("sizeof(" + size.type + ")")) {
            std::printf("%sMISSING PIN sizeof(%s) measured=%zu\n", kSay, size.type.c_str(),
                        size.size);
            ++differences;
        }
    }

    for (const auto& pin : member_pins) {
        const auto key = pin.type + "::" + pin.member;
        if (!seen.insert(key).second) {
            std::printf("%sDUPLICATE PIN %s\n", kSay, key.c_str());
            ++differences;
            continue;
        }
        bool known = false;
        for (const auto& member : members) {
            if (member.type != pin.type || member.member != pin.member) continue;
            known = true;
            if (member.offset != pin.offset) {
                std::printf("%sMOVED %s offset pinned=%zu measured=%zu\n", kSay, key.c_str(),
                            pin.offset, member.offset);
                ++differences;
            }
            if (member.size != pin.size) {
                std::printf("%sMOVED %s size pinned=%zu measured=%zu\n", kSay, key.c_str(),
                            pin.size, member.size);
                ++differences;
            }
        }
        if (!known) {
            std::printf("%sUNKNOWN PIN %s: not a member the reader's list names\n", kSay,
                        key.c_str());
            ++differences;
        }
    }
    for (const auto& member : members) {
        const auto key = member.type + "::" + member.member;
        if (!seen.count(key)) {
            std::printf("%sMISSING PIN %s measured offset=%zu size=%zu\n", kSay, key.c_str(),
                        member.offset, member.size);
            ++differences;
        }
    }

    if (differences != 0) {
        std::printf("%sFAIL: %zu difference(s) between the measured layout and %s; a layout "
                    "that moved needs PF_STREAM_ENGINE_LAYOUT_VERSION raised and the measured "
                    "table above pinned for the new stamp\n", kSay, differences, kPins);
        return 1;
    }
    std::printf("%sOK: %zu member pins and %zu class sizes match the layout of stamp %llu\n", kSay,
                member_pins.size(), size_pins.size(), stamp);
    return 0;
#endif
}
